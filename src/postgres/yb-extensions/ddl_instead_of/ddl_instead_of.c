/*-----------------------------------------------------------------------------
 *
 * ddl_instead_of
 *
 * ProcessUtility_hook that optionally rewrites top-level utility statements
 * before standard_ProcessUtility runs (therefore before ddl_command_start and
 * related event triggers).
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations under
 * the License.
 *
 *-----------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/primnodes.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

#define MAX_HANDLERS		64
#define MAX_RECURSION_GUARD	32

/*
 * Maximum number of characters from a SQL fragment or rewrite string that
 * are printed in debug messages.  Keeps log lines readable.
 */
#define DDL_DBG_SQL_MAXLEN	200

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static int	ddl_instead_of_depth = 0;

/*
 * ddl_instead_of.debug  (GUC, USERSET boolean, default off)
 *
 * When on, emits LOG-level messages at each interception point:
 *   - which command tag and SQL fragment were intercepted
 *   - how many handlers matched
 *   - what each handler returned (NULL = pass-through, or the rewrite text)
 *   - whether a rewrite was applied or the original executed unchanged
 *
 * Enable per-session:  SET ddl_instead_of.debug = on;
 */
static bool ddl_instead_of_debug = false;

/*
 * Emit a LOG message when ddl_instead_of.debug is on.
 * Uses %.Ns format truncation so no extra palloc is needed on the hot path.
 */
#define DDL_DBG(fmt, ...) \
	do { \
		if (ddl_instead_of_debug) \
			ereport(LOG, (errmsg("ddl_instead_of: " fmt, ##__VA_ARGS__))); \
	} while (0)

static void chain_ProcessUtility(PlannedStmt *pstmt,
								 const char *queryString,
								 bool readOnlyTree,
								 ProcessUtilityContext context,
								 ParamListInfo params,
								 QueryEnvironment *queryEnv,
								 DestReceiver *dest,
								 QueryCompletion *qc);

/* =========================================================================
 * parse_table_columns / parse_table_constraints implementation
 *
 * These two set-returning functions walk a raw CREATE TABLE parse tree and
 * return structured column and constraint information, allowing handler
 * functions to inspect the table signature without any regex-based parsing.
 * ========================================================================= */

/*
 * Map pg_catalog internal type names to their SQL-standard equivalents.
 * Returns NULL if the name has no special mapping (use it verbatim).
 */
static const char *
ddlii_pg_catalog_type_name(const char *iname)
{
	/* Keep sorted by internal name for readability only; linear scan is fine */
	static const struct { const char *internal; const char *sql; } map[] =
	{
		{ "bit",         "bit" },
		{ "bool",        "boolean" },
		{ "bpchar",      "character" },
		{ "date",        "date" },
		{ "float4",      "real" },
		{ "float8",      "double precision" },
		{ "int2",        "smallint" },
		{ "int4",        "integer" },
		{ "int8",        "bigint" },
		{ "interval",    "interval" },
		{ "numeric",     "numeric" },
		{ "time",        "time" },
		{ "timetz",      "time with time zone" },
		{ "timestamp",   "timestamp" },
		{ "timestamptz", "timestamp with time zone" },
		{ "varbit",      "bit varying" },
		{ "varchar",     "character varying" },
	};
	for (int i = 0; i < (int) lengthof(map); i++)
		if (strcmp(iname, map[i].internal) == 0)
			return map[i].sql;
	return NULL;
}

/*
 * Format a TypeName node as a human-readable SQL type string.
 * Includes precision/scale modifiers and array brackets.
 * Caller must pfree the returned palloc'd string.
 */
static char *
ddlii_format_type(TypeName *typeName)
{
	StringInfoData	buf;
	const char	   *schema_part = NULL;
	const char	   *name_part = NULL;
	int				nnames;
	bool			is_pg_catalog;

	initStringInfo(&buf);

	if (typeName == NULL)
	{
		appendStringInfoString(&buf, "unknown");
		return buf.data;
	}

	nnames = list_length(typeName->names);

	if (nnames == 0)
		name_part = "unknown";
	else if (nnames == 1)
		name_part = strVal(linitial(typeName->names));
	else if (nnames == 2)
	{
		schema_part = strVal(linitial(typeName->names));
		name_part   = strVal(lsecond(typeName->names));
	}
	else
	{
		schema_part = strVal(list_nth(typeName->names, nnames - 2));
		name_part   = strVal(llast(typeName->names));
	}

	is_pg_catalog = (schema_part != NULL &&
					 strcmp(schema_part, "pg_catalog") == 0);

	if (is_pg_catalog || schema_part == NULL)
	{
		const char *sql_name = ddlii_pg_catalog_type_name(name_part);
		appendStringInfoString(&buf, sql_name ? sql_name : name_part);
	}
	else
		appendStringInfo(&buf, "%s.%s", schema_part, name_part);

	/* Type modifiers: (precision), (precision, scale), (length), etc. */
	if (typeName->typmods != NIL)
	{
		ListCell   *lc;
		bool		first = true;

		appendStringInfoChar(&buf, '(');
		foreach(lc, typeName->typmods)
		{
			Node *mod = lfirst(lc);

			if (!first)
				appendStringInfoString(&buf, ", ");
			first = false;

			if (IsA(mod, A_Const))
			{
				A_Const *ac = (A_Const *) mod;
				if (!ac->isnull)
				{
					if (IsA(&ac->val, Integer))
						appendStringInfo(&buf, "%d", ac->val.ival.ival);
					else if (IsA(&ac->val, Float))
						appendStringInfoString(&buf, ac->val.fval.fval);
					else if (IsA(&ac->val, String))
						appendStringInfoString(&buf, ac->val.sval.sval);
				}
			}
			else if (IsA(mod, Integer))
				appendStringInfo(&buf, "%d", ((Integer *) mod)->ival);
		}
		appendStringInfoChar(&buf, ')');
	}

	/* Array dimensions: one "[]" per element in arrayBounds */
	for (int i = 0; i < list_length(typeName->arrayBounds); i++)
		appendStringInfoString(&buf, "[]");

	return buf.data;
}

/*
 * Forward declaration so ddlii_format_expr can be recursive.
 */
static char *ddlii_format_expr(Node *expr);

/*
 * Format a raw A_Const literal as a SQL text representation.
 */
static char *
ddlii_format_aconst(A_Const *ac)
{
	StringInfoData buf;

	initStringInfo(&buf);

	if (ac->isnull)
	{
		appendStringInfoString(&buf, "NULL");
	}
	else if (IsA(&ac->val, Integer))
	{
		appendStringInfo(&buf, "%d", ac->val.ival.ival);
	}
	else if (IsA(&ac->val, Float))
	{
		appendStringInfoString(&buf, ac->val.fval.fval);
	}
	else if (IsA(&ac->val, Boolean))
	{
		appendStringInfoString(&buf, ac->val.boolval.boolval ? "true" : "false");
	}
	else if (IsA(&ac->val, String))
	{
		const char *s = ac->val.sval.sval;
		appendStringInfoChar(&buf, '\'');
		while (*s)
		{
			if (*s == '\'')
				appendStringInfoChar(&buf, '\''); /* double embedded quotes */
			appendStringInfoChar(&buf, *s++);
		}
		appendStringInfoChar(&buf, '\'');
	}
	else if (IsA(&ac->val, BitString))
	{
		/* Starts with 'b' (binary literal) or 'x' (hex literal) */
		const char *bs = ac->val.bsval.bsval;
		if (*bs == 'b' || *bs == 'B')
			appendStringInfo(&buf, "B'%s'", bs + 1);
		else
			appendStringInfo(&buf, "X'%s'", bs + 1);
	}
	else
		appendStringInfoString(&buf, "<constant>");

	return buf.data;
}

/*
 * Format a raw expression node as a SQL text string.
 *
 * Handles the most common forms that appear in DEFAULT and CHECK expressions:
 * literals, type casts, function calls, column references, and operators.
 * Returns "<expression>" for anything else rather than crashing.
 *
 * Caller must pfree the returned palloc'd string.
 */
static char *
ddlii_format_expr(Node *expr)
{
	StringInfoData buf;

	if (expr == NULL)
		return pstrdup("NULL");

	initStringInfo(&buf);

	if (IsA(expr, A_Const))
	{
		char *s = ddlii_format_aconst((A_Const *) expr);
		appendStringInfoString(&buf, s);
		pfree(s);
	}
	else if (IsA(expr, TypeCast))
	{
		TypeCast *tc = (TypeCast *) expr;
		char	 *arg_str  = ddlii_format_expr(tc->arg);
		char	 *type_str = ddlii_format_type(tc->typeName);

		appendStringInfo(&buf, "%s::%s", arg_str, type_str);
		pfree(arg_str);
		pfree(type_str);
	}
	else if (IsA(expr, FuncCall))
	{
		FuncCall   *fc = (FuncCall *) expr;
		ListCell   *lc;
		bool		first = true;
		const char *schema = NULL;
		const char *fname  = NULL;
		int			nnames = list_length(fc->funcname);

		if (nnames == 1)
			fname = strVal(linitial(fc->funcname));
		else if (nnames >= 2)
		{
			schema = strVal(list_nth(fc->funcname, nnames - 2));
			fname  = strVal(llast(fc->funcname));
		}

		/* Suppress pg_catalog schema prefix — it's redundant for users */
		if (schema && strcmp(schema, "pg_catalog") != 0)
			appendStringInfo(&buf, "%s.", schema);
		appendStringInfoString(&buf, fname ? fname : "<func>");

		appendStringInfoChar(&buf, '(');
		if (fc->agg_star)
			appendStringInfoChar(&buf, '*');
		else
		{
			foreach(lc, fc->args)
			{
				char *arg_str = ddlii_format_expr((Node *) lfirst(lc));
				if (!first) appendStringInfoString(&buf, ", ");
				first = false;
				appendStringInfoString(&buf, arg_str);
				pfree(arg_str);
			}
		}
		appendStringInfoChar(&buf, ')');
	}
	else if (IsA(expr, ColumnRef))
	{
		ColumnRef  *cr = (ColumnRef *) expr;
		ListCell   *lc;
		bool		first = true;

		foreach(lc, cr->fields)
		{
			Node *field = (Node *) lfirst(lc);
			if (!first) appendStringInfoChar(&buf, '.');
			first = false;
			if (IsA(field, String))
				appendStringInfoString(&buf, strVal((String *) field));
			else
				appendStringInfoChar(&buf, '*');
		}
	}
	else if (IsA(expr, A_Expr))
	{
		A_Expr	   *ae = (A_Expr *) expr;
		const char *op_name = NULL;

		if (list_length(ae->name) == 1 &&
			IsA(linitial(ae->name), String))
			op_name = strVal((String *) linitial(ae->name));

		switch (ae->kind)
		{
			case AEXPR_OP:
			case AEXPR_OP_ANY:
			case AEXPR_OP_ALL:
			case AEXPR_DISTINCT:
			case AEXPR_NOT_DISTINCT:
			case AEXPR_NULLIF:
			{
				if (ae->lexpr && ae->rexpr)
				{
					char	   *lstr = ddlii_format_expr(ae->lexpr);
					char	   *rstr = ddlii_format_expr(ae->rexpr);
					const char *mod  = (ae->kind == AEXPR_OP_ANY) ? " ANY" :
									   (ae->kind == AEXPR_OP_ALL) ? " ALL" : "";
					appendStringInfo(&buf, "(%s %s%s %s)",
									lstr, op_name ? op_name : "?", mod, rstr);
					pfree(lstr);
					pfree(rstr);
				}
				else if (!ae->lexpr && ae->rexpr)
				{
					char *rstr = ddlii_format_expr(ae->rexpr);
					appendStringInfo(&buf, "(%s %s)",
									op_name ? op_name : "?", rstr);
					pfree(rstr);
				}
				else if (ae->lexpr && !ae->rexpr)
				{
					char *lstr = ddlii_format_expr(ae->lexpr);
					appendStringInfo(&buf, "(%s %s)",
									lstr, op_name ? op_name : "?");
					pfree(lstr);
				}
				else
					appendStringInfoString(&buf, "<expression>");
				break;
			}

			case AEXPR_IN:
			{
				char	   *lstr = ae->lexpr ? ddlii_format_expr(ae->lexpr)
										    : pstrdup("?");
				const char *in_kw = (op_name && strcmp(op_name, "=") == 0)
									? "IN" : "NOT IN";
				appendStringInfo(&buf, "(%s %s (", lstr, in_kw);
				pfree(lstr);
				if (ae->rexpr && IsA(ae->rexpr, List))
				{
					ListCell *lc;
					bool	  first = true;
					foreach(lc, (List *) ae->rexpr)
					{
						char *elem = ddlii_format_expr((Node *) lfirst(lc));
						if (!first) appendStringInfoString(&buf, ", ");
						first = false;
						appendStringInfoString(&buf, elem);
						pfree(elem);
					}
				}
				appendStringInfoString(&buf, "))");
				break;
			}

			case AEXPR_BETWEEN:
			case AEXPR_NOT_BETWEEN:
			case AEXPR_BETWEEN_SYM:
			case AEXPR_NOT_BETWEEN_SYM:
			{
				const char *bw_kw =
					(ae->kind == AEXPR_BETWEEN)         ? "BETWEEN" :
					(ae->kind == AEXPR_NOT_BETWEEN)     ? "NOT BETWEEN" :
					(ae->kind == AEXPR_BETWEEN_SYM)     ? "BETWEEN SYMMETRIC" :
														  "NOT BETWEEN SYMMETRIC";
				char *lstr = ae->lexpr ? ddlii_format_expr(ae->lexpr)
									   : pstrdup("?");
				appendStringInfo(&buf, "(%s %s", lstr, bw_kw);
				pfree(lstr);
				if (ae->rexpr && IsA(ae->rexpr, List))
				{
					List *bounds = (List *) ae->rexpr;
					if (list_length(bounds) >= 2)
					{
						char *lo = ddlii_format_expr((Node *) linitial(bounds));
						char *hi = ddlii_format_expr((Node *) lsecond(bounds));
						appendStringInfo(&buf, " %s AND %s", lo, hi);
						pfree(lo);
						pfree(hi);
					}
				}
				appendStringInfoChar(&buf, ')');
				break;
			}

			case AEXPR_LIKE:
			case AEXPR_ILIKE:
			{
				char	   *lstr = ae->lexpr ? ddlii_format_expr(ae->lexpr)
										    : pstrdup("?");
				char	   *rstr = ae->rexpr ? ddlii_format_expr(ae->rexpr)
										    : pstrdup("?");
				bool negated = (op_name &&
								(strcmp(op_name, "!~~") == 0 ||
								 strcmp(op_name, "!~~*") == 0));
				appendStringInfo(&buf, "(%s %s%s %s)",
								lstr,
								negated ? "NOT " : "",
								ae->kind == AEXPR_LIKE ? "LIKE" : "ILIKE",
								rstr);
				pfree(lstr);
				pfree(rstr);
				break;
			}

			default:
			{
				if (ae->lexpr && ae->rexpr)
				{
					char *lstr = ddlii_format_expr(ae->lexpr);
					char *rstr = ddlii_format_expr(ae->rexpr);
					appendStringInfo(&buf, "(%s %s %s)",
									lstr, op_name ? op_name : "?", rstr);
					pfree(lstr);
					pfree(rstr);
				}
				else
					appendStringInfoString(&buf, "<expression>");
				break;
			}
		}
	}
	else if (IsA(expr, NullTest))
	{
		NullTest *nt  = (NullTest *) expr;
		char	 *arg = ddlii_format_expr((Node *) nt->arg);
		appendStringInfo(&buf, "(%s %s)", arg,
						nt->nulltesttype == IS_NULL ? "IS NULL" : "IS NOT NULL");
		pfree(arg);
	}
	else if (IsA(expr, BooleanTest))
	{
		BooleanTest *bt  = (BooleanTest *) expr;
		char		*arg = ddlii_format_expr((Node *) bt->arg);
		const char  *kw;
		switch (bt->booltesttype)
		{
			case IS_TRUE:        kw = "IS TRUE";        break;
			case IS_NOT_TRUE:    kw = "IS NOT TRUE";    break;
			case IS_FALSE:       kw = "IS FALSE";       break;
			case IS_NOT_FALSE:   kw = "IS NOT FALSE";   break;
			case IS_UNKNOWN:     kw = "IS UNKNOWN";     break;
			case IS_NOT_UNKNOWN: kw = "IS NOT UNKNOWN"; break;
			default:             kw = "IS ?";           break;
		}
		appendStringInfo(&buf, "(%s %s)", arg, kw);
		pfree(arg);
	}
	else if (IsA(expr, BoolExpr))
	{
		BoolExpr   *be    = (BoolExpr *) expr;
		ListCell   *lc;
		bool		first = true;

		if (be->boolop == NOT_EXPR)
		{
			char *arg = ddlii_format_expr((Node *) linitial(be->args));
			appendStringInfo(&buf, "(NOT %s)", arg);
			pfree(arg);
		}
		else
		{
			const char *op = (be->boolop == AND_EXPR) ? " AND " : " OR ";
			appendStringInfoChar(&buf, '(');
			foreach(lc, be->args)
			{
				char *arg = ddlii_format_expr((Node *) lfirst(lc));
				if (!first) appendStringInfoString(&buf, op);
				first = false;
				appendStringInfoString(&buf, arg);
				pfree(arg);
			}
			appendStringInfoChar(&buf, ')');
		}
	}
	else if (IsA(expr, ParamRef))
	{
		appendStringInfo(&buf, "$%d", ((ParamRef *) expr)->number);
	}
	else if (IsA(expr, CollateClause))
	{
		CollateClause *cc  = (CollateClause *) expr;
		char		  *arg = ddlii_format_expr(cc->arg);
		appendStringInfo(&buf, "%s COLLATE ...", arg);
		pfree(arg);
	}
	else if (IsA(expr, SubLink))
	{
		appendStringInfoString(&buf, "<subquery>");
	}
	else
	{
		appendStringInfoString(&buf, "<expression>");
	}

	return buf.data;
}

/*
 * Join a list of String nodes into a comma-separated palloc'd string.
 */
static char *
ddlii_strlist_join(List *strlist, const char *sep)
{
	StringInfoData	buf;
	ListCell	   *lc;
	bool			first = true;

	initStringInfo(&buf);
	foreach(lc, strlist)
	{
		if (!first) appendStringInfoString(&buf, sep);
		first = false;
		appendStringInfoString(&buf, strVal(lfirst(lc)));
	}
	return buf.data;
}

/*
 * Format a list of IndexElem nodes as "col [HASH|ASC|DESC], ..." .
 * Used to render YugabyteDB PRIMARY KEY / UNIQUE ordering information.
 */
static char *
ddlii_indexelem_list_to_str(List *ielems)
{
	StringInfoData	buf;
	ListCell	   *lc;
	bool			first = true;

	initStringInfo(&buf);
	foreach(lc, ielems)
	{
		IndexElem  *ie = lfirst_node(IndexElem, lc);
		if (!first) appendStringInfoString(&buf, ", ");
		first = false;

		if (ie->name)
			appendStringInfoString(&buf, ie->name);
		else
			appendStringInfoString(&buf, "<expr>");

		/* YugabyteDB extended ordering suffix */
		switch (ie->ordering)
		{
			case SORTBY_HASH:   appendStringInfoString(&buf, " HASH"); break;
			case SORTBY_ASC:    appendStringInfoString(&buf, " ASC");  break;
			case SORTBY_DESC:   appendStringInfoString(&buf, " DESC"); break;
			default:            break;  /* SORTBY_DEFAULT: no suffix */
		}
	}
	return buf.data;
}

/*
 * Return the SQL keyword for a FK referential action character.
 */
static const char *
ddlii_fk_action_str(char action)
{
	switch (action)
	{
		case FKCONSTR_ACTION_NOACTION:   return "NO ACTION";
		case FKCONSTR_ACTION_RESTRICT:   return "RESTRICT";
		case FKCONSTR_ACTION_CASCADE:    return "CASCADE";
		case FKCONSTR_ACTION_SETNULL:    return "SET NULL";
		case FKCONSTR_ACTION_SETDEFAULT: return "SET DEFAULT";
		default:                         return "NO ACTION";
	}
}

/* ---------- parse_table_columns ---------- */

/*
 * ddl_instead_of_parse_table_columns
 *
 * Parse a CREATE TABLE statement and return one row per column:
 *
 *   ordinal_position integer  -- 1-based
 *   column_name      text
 *   type_name        text     -- human-readable SQL type (e.g. "character varying(100)")
 *   not_null         boolean  -- true when NOT NULL is present
 *   has_default      boolean  -- true when a DEFAULT clause is present
 *   default_expr     text     -- the default expression as text, or NULL
 */
PG_FUNCTION_INFO_V1(ddl_instead_of_parse_table_columns);
Datum
ddl_instead_of_parse_table_columns(PG_FUNCTION_ARGS)
{
	text		  *stmt_text = PG_GETARG_TEXT_PP(0);
	char		  *stmt_cstr;
	List		  *raw_list;
	RawStmt		  *rawstmt;
	Node		  *stmt;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	CreateStmt	  *cs;
	ListCell	  *lc;
	int			   ordinal = 0;

	InitMaterializedSRF(fcinfo, 0);

	stmt_cstr = text_to_cstring(stmt_text);
	raw_list  = raw_parser(stmt_cstr, RAW_PARSE_DEFAULT);
	pfree(stmt_cstr);

	if (list_length(raw_list) < 1)
		return (Datum) 0;

	rawstmt = linitial_node(RawStmt, raw_list);
	stmt    = rawstmt->stmt;

	if (stmt == NULL || !IsA(stmt, CreateStmt))
		return (Datum) 0;

	cs = (CreateStmt *) stmt;

	foreach(lc, cs->tableElts)
	{
		Node	   *elt = (Node *) lfirst(lc);
		ColumnDef  *col;
		ListCell   *clc;
		bool		col_not_null = false;
		Node	   *col_default  = NULL;
		Datum		values[6];
		bool		nulls[6];
		char	   *type_str;

		if (!IsA(elt, ColumnDef))
			continue;   /* skip table-level Constraint nodes */

		col = (ColumnDef *) elt;
		ordinal++;

		/*
		 * In the raw parse tree, NOT NULL and DEFAULT live in the constraints
		 * list (CONSTR_NOTNULL / CONSTR_DEFAULT); the ColumnDef.is_not_null
		 * and raw_default fields are populated only after semantic analysis.
		 */
		foreach(clc, col->constraints)
		{
			Constraint *con = lfirst_node(Constraint, clc);
			switch (con->contype)
			{
				case CONSTR_NOTNULL:
					col_not_null = true;
					break;
				case CONSTR_DEFAULT:
					if (con->raw_expr != NULL)
						col_default = con->raw_expr;
					break;
				case CONSTR_PRIMARY:
				case CONSTR_IDENTITY:
				case CONSTR_GENERATED:
					/* These imply NOT NULL even without an explicit constraint */
					col_not_null = true;
					break;
				default:
					break;
			}
		}

		MemSet(nulls, 0, sizeof(nulls));

		/* ordinal_position */
		values[0] = Int32GetDatum(ordinal);

		/* column_name */
		values[1] = CStringGetTextDatum(col->colname);

		/* type_name */
		if (col->typeName)
		{
			type_str = ddlii_format_type(col->typeName);
			values[2] = CStringGetTextDatum(type_str);
			pfree(type_str);
		}
		else
			nulls[2] = true;

		/* not_null */
		values[3] = BoolGetDatum(col_not_null);

		/* has_default */
		values[4] = BoolGetDatum(col_default != NULL);

		/* default_expr */
		if (col_default != NULL)
		{
			char *expr_str = ddlii_format_expr(col_default);
			values[5] = CStringGetTextDatum(expr_str);
			pfree(expr_str);
		}
		else
			nulls[5] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/* ---------- parse_table_constraints ---------- */

/*
 * Emit one constraint row into the SRF tuplestore.
 *
 * parent_colname: non-NULL for a column-level constraint (e.g. REFERENCES
 * written inline with a column definition); NULL for a table-level constraint.
 */
static void
ddlii_emit_constraint_row(ReturnSetInfo *rsinfo,
						  Constraint	*con,
						  const char	*parent_colname)
{
	Datum		values[8];
	bool		nulls[8];
	const char *con_type;

	switch (con->contype)
	{
		case CONSTR_PRIMARY:   con_type = "PRIMARY KEY"; break;
		case CONSTR_UNIQUE:    con_type = "UNIQUE";      break;
		case CONSTR_CHECK:     con_type = "CHECK";       break;
		case CONSTR_FOREIGN:   con_type = "FOREIGN KEY"; break;
		case CONSTR_EXCLUSION: con_type = "EXCLUDE";     break;
		default:               return;  /* ignore NULL/NOTNULL/DEFAULT etc. */
	}

	MemSet(nulls, 0, sizeof(nulls));

	/* constraint_name */
	if (con->conname && con->conname[0] != '\0')
		values[0] = CStringGetTextDatum(con->conname);
	else
		nulls[0] = true;

	/* constraint_type */
	values[1] = CStringGetTextDatum(con_type);

	/*
	 * column_names: key column list.
	 *
	 * Preference order for PK/UNIQUE:
	 *   1. yb_index_params (non-NIL) — carries YugabyteDB HASH/ASC/DESC ordering
	 *   2. keys            (non-NIL) — plain column name list from standard grammar
	 *   3. parent_colname  (non-NULL) — column-level constraint: the column itself
	 *
	 * For FOREIGN KEY the local column list comes from fk_attrs; for a
	 * column-level REFERENCES the parser leaves fk_attrs NIL and the column
	 * is identified by parent_colname.
	 */
	if (con->contype == CONSTR_FOREIGN)
	{
		if (parent_colname != NULL)
			/* column-level REFERENCES: the FK column is the declaring column */
			values[2] = CStringGetTextDatum(parent_colname);
		else if (con->fk_attrs != NIL)
		{
			char *s = ddlii_strlist_join(con->fk_attrs, ", ");
			values[2] = CStringGetTextDatum(s);
			pfree(s);
		}
		else
			nulls[2] = true;
	}
	else if (con->yb_index_params != NIL)
	{
		/* Use YB ordering-aware representation */
		char *s = ddlii_indexelem_list_to_str(con->yb_index_params);
		values[2] = CStringGetTextDatum(s);
		pfree(s);
	}
	else if (con->keys != NIL)
	{
		char *s = ddlii_strlist_join(con->keys, ", ");
		values[2] = CStringGetTextDatum(s);
		pfree(s);
	}
	else if (parent_colname != NULL)
		values[2] = CStringGetTextDatum(parent_colname);
	else
		nulls[2] = true;

	/* check_expr — for CHECK constraints */
	if (con->contype == CONSTR_CHECK && con->raw_expr != NULL)
	{
		char *s = ddlii_format_expr(con->raw_expr);
		values[3] = CStringGetTextDatum(s);
		pfree(s);
	}
	else
		nulls[3] = true;

	/* ref_table — for FOREIGN KEY */
	if (con->contype == CONSTR_FOREIGN && con->pktable != NULL)
	{
		StringInfoData buf;
		initStringInfo(&buf);
		if (con->pktable->schemaname && con->pktable->schemaname[0] != '\0')
			appendStringInfo(&buf, "%s.%s",
							con->pktable->schemaname, con->pktable->relname);
		else
			appendStringInfoString(&buf, con->pktable->relname);
		values[4] = CStringGetTextDatum(buf.data);
		pfree(buf.data);
	}
	else
		nulls[4] = true;

	/*
	 * ref_columns — referenced columns for FK.
	 * NULL means "the primary key of ref_table" (PostgreSQL default).
	 */
	if (con->contype == CONSTR_FOREIGN && con->pk_attrs != NIL)
	{
		char *s = ddlii_strlist_join(con->pk_attrs, ", ");
		values[5] = CStringGetTextDatum(s);
		pfree(s);
	}
	else
		nulls[5] = true;

	/* fk_on_delete, fk_on_update — FK referential actions */
	if (con->contype == CONSTR_FOREIGN)
	{
		values[6] = CStringGetTextDatum(ddlii_fk_action_str(con->fk_del_action));
		values[7] = CStringGetTextDatum(ddlii_fk_action_str(con->fk_upd_action));
	}
	else
	{
		nulls[6] = true;
		nulls[7] = true;
	}

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * ddl_instead_of_parse_table_constraints
 *
 * Parse a CREATE TABLE statement and return one row per constraint:
 *
 *   constraint_name  text     -- name given by user, or NULL if unnamed
 *   constraint_type  text     -- 'PRIMARY KEY', 'UNIQUE', 'CHECK', 'FOREIGN KEY', 'EXCLUDE'
 *   column_names     text     -- comma-separated key columns (with HASH/ASC/DESC for YugabyteDB)
 *   check_expr       text     -- CHECK predicate text, or NULL
 *   ref_table        text     -- [schema.]table for FOREIGN KEY, or NULL
 *   ref_columns      text     -- referenced columns for FK, or NULL (= use PK)
 *   fk_on_delete     text     -- FK ON DELETE action, or NULL
 *   fk_on_update     text     -- FK ON UPDATE action, or NULL
 *
 * Table-level constraints (listed after the last column) are emitted first,
 * followed by column-level key constraints (PRIMARY KEY, UNIQUE, CHECK,
 * FOREIGN KEY declared inline with a column).  NOT NULL and DEFAULT are not
 * emitted here; use parse_table_columns for those.
 */
PG_FUNCTION_INFO_V1(ddl_instead_of_parse_table_constraints);
Datum
ddl_instead_of_parse_table_constraints(PG_FUNCTION_ARGS)
{
	text		  *stmt_text = PG_GETARG_TEXT_PP(0);
	char		  *stmt_cstr;
	List		  *raw_list;
	RawStmt		  *rawstmt;
	Node		  *stmt;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	CreateStmt	  *cs;
	ListCell	  *lc;

	InitMaterializedSRF(fcinfo, 0);

	stmt_cstr = text_to_cstring(stmt_text);
	raw_list  = raw_parser(stmt_cstr, RAW_PARSE_DEFAULT);
	pfree(stmt_cstr);

	if (list_length(raw_list) < 1)
		return (Datum) 0;

	rawstmt = linitial_node(RawStmt, raw_list);
	stmt    = rawstmt->stmt;

	if (stmt == NULL || !IsA(stmt, CreateStmt))
		return (Datum) 0;

	cs = (CreateStmt *) stmt;

	/* First: table-level Constraint nodes in tableElts */
	foreach(lc, cs->tableElts)
	{
		Node *elt = (Node *) lfirst(lc);
		if (IsA(elt, Constraint))
			ddlii_emit_constraint_row(rsinfo, (Constraint *) elt, NULL);
	}

	/* Second: column-level key constraints from each ColumnDef */
	foreach(lc, cs->tableElts)
	{
		Node	   *elt = (Node *) lfirst(lc);
		ColumnDef  *col;
		ListCell   *clc;

		if (!IsA(elt, ColumnDef))
			continue;

		col = (ColumnDef *) elt;
		foreach(clc, col->constraints)
		{
			Constraint *con = lfirst_node(Constraint, clc);
			switch (con->contype)
			{
				case CONSTR_PRIMARY:
				case CONSTR_UNIQUE:
				case CONSTR_CHECK:
				case CONSTR_FOREIGN:
					ddlii_emit_constraint_row(rsinfo, con, col->colname);
					break;
				default:
					break;
			}
		}
	}

	return (Datum) 0;
}

/* =========================================================================
 * End of parse_table_columns / parse_table_constraints implementation
 * ========================================================================= */

/* =========================================================================
 * parse_index_columns / parse_index_predicates implementation
 *
 * These two set-returning functions decompose a raw CREATE INDEX statement
 * into its constituent parts so that PL/pgSQL can compute storage estimates
 * without having to re-parse the SQL text or walk the AST itself.
 * ========================================================================= */

/*
 * Return the unqualified column name from a ColumnRef, or NULL if the node
 * is not a plain ColumnRef (e.g. an expression).
 */
static const char *
ddlii_colref_name(Node *node)
{
	ColumnRef  *cr;

	if (node == NULL || !IsA(node, ColumnRef))
		return NULL;
	cr = (ColumnRef *) node;
	if (list_length(cr->fields) < 1)
		return NULL;
	/* Use the last field; any table-qualifier before it is irrelevant here */
	return strVal(llast(cr->fields));
}

/* ---- parse_index_columns ---- */

static void
ddlii_emit_index_col_row(ReturnSetInfo *rsinfo,
						 bool		   is_key,
						 int		   ordinal,
						 const char   *column_name,
						 const char   *expression,
						 SortByDir	   ordering,
						 SortByNulls   nulls_ordering,
						 const char   *where_clause)
{
	Datum		values[7];
	bool		nulls[7];
	const char *ord_str;

	MemSet(nulls, 0, sizeof(nulls));

	/* is_key */
	values[0] = BoolGetDatum(is_key);

	/* ordinal */
	values[1] = Int32GetDatum(ordinal);

	/* column_name */
	if (column_name)
		values[2] = CStringGetTextDatum(column_name);
	else
		nulls[2] = true;

	/* expression */
	if (expression)
		values[3] = CStringGetTextDatum(expression);
	else
		nulls[3] = true;

	/* ordering */
	switch (ordering)
	{
		case SORTBY_HASH:	ord_str = "HASH"; break;
		case SORTBY_ASC:	ord_str = "ASC";  break;
		case SORTBY_DESC:	ord_str = "DESC"; break;
		default:			ord_str = NULL;   break;  /* SORTBY_DEFAULT */
	}
	if (ord_str)
		values[4] = CStringGetTextDatum(ord_str);
	else
		nulls[4] = true;

	/* nulls_first */
	switch (nulls_ordering)
	{
		case SORTBY_NULLS_FIRST:  values[5] = BoolGetDatum(true);  break;
		case SORTBY_NULLS_LAST:   values[5] = BoolGetDatum(false); break;
		default:                  nulls[5]  = true;                break;
	}

	/* where_clause */
	if (where_clause)
		values[6] = CStringGetTextDatum(where_clause);
	else
		nulls[6] = true;

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * ddl_instead_of_parse_index_columns
 *
 * Parse a CREATE INDEX statement and return one row per key column followed
 * by one row per INCLUDE column:
 *
 *   is_key       boolean  -- true = key column, false = INCLUDE column
 *   ordinal      integer  -- 1-based position within the key or include list
 *   column_name  text     -- column name, or NULL for expression indexes
 *   expression   text     -- formatted expression, or NULL for plain columns
 *   ordering     text     -- 'HASH' | 'ASC' | 'DESC' | NULL (default)
 *   nulls_first  boolean  -- NULL = database default
 *   where_clause text     -- WHERE predicate text (same on every row); NULL if absent
 */
PG_FUNCTION_INFO_V1(ddl_instead_of_parse_index_columns);
Datum
ddl_instead_of_parse_index_columns(PG_FUNCTION_ARGS)
{
	text		  *stmt_text = PG_GETARG_TEXT_PP(0);
	char		  *stmt_cstr;
	List		  *raw_list;
	RawStmt		  *rawstmt;
	Node		  *stmt;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	IndexStmt	  *is;
	ListCell	  *lc;
	int			   ordinal;
	char		  *where_str = NULL;

	InitMaterializedSRF(fcinfo, 0);

	stmt_cstr = text_to_cstring(stmt_text);
	raw_list  = raw_parser(stmt_cstr, RAW_PARSE_DEFAULT);
	pfree(stmt_cstr);

	if (list_length(raw_list) < 1)
		return (Datum) 0;

	rawstmt = linitial_node(RawStmt, raw_list);
	stmt    = rawstmt->stmt;

	if (stmt == NULL || !IsA(stmt, IndexStmt))
		return (Datum) 0;

	is = (IndexStmt *) stmt;

	/* Format the WHERE predicate once; the same text appears on every row */
	if (is->whereClause != NULL)
		where_str = ddlii_format_expr(is->whereClause);

	/* Key columns (indexParams) */
	ordinal = 0;
	foreach(lc, is->indexParams)
	{
		IndexElem  *ie = lfirst_node(IndexElem, lc);
		char	   *expr_str = NULL;

		ordinal++;

		if (ie->expr != NULL)
			expr_str = ddlii_format_expr(ie->expr);

		ddlii_emit_index_col_row(rsinfo,
								 true, /* is_key */
								 ordinal,
								 ie->name,
								 expr_str,
								 ie->ordering,
								 ie->nulls_ordering,
								 where_str);
		if (expr_str)
			pfree(expr_str);
	}

	/* INCLUDE columns (indexIncludingParams) */
	ordinal = 0;
	foreach(lc, is->indexIncludingParams)
	{
		IndexElem  *ie = lfirst_node(IndexElem, lc);
		char	   *expr_str = NULL;

		ordinal++;

		if (ie->expr != NULL)
			expr_str = ddlii_format_expr(ie->expr);

		ddlii_emit_index_col_row(rsinfo,
								 false, /* is_key = false means INCLUDE */
								 ordinal,
								 ie->name,
								 expr_str,
								 ie->ordering,
								 ie->nulls_ordering,
								 where_str);
		if (expr_str)
			pfree(expr_str);
	}

	if (where_str)
		pfree(where_str);

	return (Datum) 0;
}

/* ---- parse_index_predicates ---- */

static void ddlii_walk_predicate(ReturnSetInfo *rsinfo,
								 Node *predicate,
								 const char *conjunction,
								 bool negated);

static void
ddlii_emit_predicate_row(ReturnSetInfo *rsinfo,
						 const char   *conjunction,
						 const char   *column_name,
						 const char   *operator_name,
						 const char   *literal,
						 const char   *literal_list,
						 bool		   negated)
{
	Datum  values[6];
	bool   nulls[6];

	MemSet(nulls, 0, sizeof(nulls));

	/* conjunction */
	values[0] = CStringGetTextDatum(conjunction ? conjunction : "NONE");

	/* column_name */
	if (column_name)
		values[1] = CStringGetTextDatum(column_name);
	else
		nulls[1] = true;

	/* operator */
	values[2] = CStringGetTextDatum(operator_name ? operator_name : "UNKNOWN");

	/* literal */
	if (literal)
		values[3] = CStringGetTextDatum(literal);
	else
		nulls[3] = true;

	/* literal_list (for IN / BETWEEN) */
	if (literal_list)
		values[4] = CStringGetTextDatum(literal_list);
	else
		nulls[4] = true;

	/* negated */
	values[5] = BoolGetDatum(negated);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * Recursively walk a raw-parse-tree predicate node and emit one row per
 * leaf condition into the SRF tuplestore.
 *
 * conjunction: 'AND', 'OR', or NULL/'NONE' for the top-level single predicate.
 * negated:     true when the current sub-tree is inside a NOT expression.
 *
 * AND/OR BoolExpr nodes are not emitted themselves; they recurse and tag each
 * leaf with the parent conjunction so PL/pgSQL can reconstruct the combination
 * formula (multiply selectivities for AND; use 1-(1-s1)*(1-s2) for OR).
 */
static void
ddlii_walk_predicate(ReturnSetInfo *rsinfo,
					 Node		  *predicate,
					 const char   *conjunction,
					 bool		   negated)
{
	if (predicate == NULL)
		return;

	if (IsA(predicate, BoolExpr))
	{
		BoolExpr   *be = (BoolExpr *) predicate;
		ListCell   *lc;

		if (be->boolop == NOT_EXPR)
		{
			/* Flip negation for the single child */
			ddlii_walk_predicate(rsinfo, (Node *) linitial(be->args),
								 conjunction, !negated);
		}
		else
		{
			const char *child_conj = (be->boolop == AND_EXPR) ? "AND" : "OR";
			foreach(lc, be->args)
				ddlii_walk_predicate(rsinfo, (Node *) lfirst(lc),
									 child_conj, negated);
		}
	}
	else if (IsA(predicate, NullTest))
	{
		NullTest   *nt  = (NullTest *) predicate;
		const char *col = ddlii_colref_name((Node *) nt->arg);
		const char *op  = (nt->nulltesttype == IS_NULL)
						  ? "IS NULL" : "IS NOT NULL";

		ddlii_emit_predicate_row(rsinfo, conjunction ? conjunction : "NONE",
								 col, op, NULL, NULL, negated);
	}
	else if (IsA(predicate, BooleanTest))
	{
		BooleanTest *bt  = (BooleanTest *) predicate;
		const char  *col = ddlii_colref_name((Node *) bt->arg);
		const char  *op;

		switch (bt->booltesttype)
		{
			case IS_TRUE:        op = "IS TRUE";        break;
			case IS_NOT_TRUE:    op = "IS NOT TRUE";    break;
			case IS_FALSE:       op = "IS FALSE";       break;
			case IS_NOT_FALSE:   op = "IS NOT FALSE";   break;
			case IS_UNKNOWN:     op = "IS UNKNOWN";     break;
			case IS_NOT_UNKNOWN: op = "IS NOT UNKNOWN"; break;
			default:             op = "UNKNOWN";        break;
		}

		ddlii_emit_predicate_row(rsinfo, conjunction ? conjunction : "NONE",
								 col, op, NULL, NULL, negated);
	}
	else if (IsA(predicate, A_Expr))
	{
		A_Expr	   *ae = (A_Expr *) predicate;
		const char *op_name = NULL;
		const char *col;
		char	   *literal      = NULL;
		char	   *literal_list = NULL;
		const char *mapped_op;

		/* Operator name */
		if (list_length(ae->name) >= 1 && IsA(linitial(ae->name), String))
			op_name = strVal((String *) linitial(ae->name));

		/* Column on the LHS (may be NULL for expression predicates) */
		col = ddlii_colref_name(ae->lexpr);

		switch (ae->kind)
		{
			case AEXPR_OP:
				if      (!op_name)                    mapped_op = "UNKNOWN";
				else if (strcmp(op_name, "=")  == 0)  mapped_op = "=";
				else if (strcmp(op_name, "<")  == 0)  mapped_op = "<";
				else if (strcmp(op_name, ">")  == 0)  mapped_op = ">";
				else if (strcmp(op_name, "<=") == 0)  mapped_op = "<=";
				else if (strcmp(op_name, ">=") == 0)  mapped_op = ">=";
				else if (strcmp(op_name, "<>") == 0)  mapped_op = "<>";
				else                                   mapped_op = op_name;

				if (ae->rexpr != NULL)
					literal = ddlii_format_expr(ae->rexpr);
				break;

			case AEXPR_IN:
				mapped_op = (op_name && strcmp(op_name, "=") == 0)
							? "IN" : "NOT IN";
				if (ae->rexpr != NULL && IsA(ae->rexpr, List))
				{
					StringInfoData	buf;
					ListCell	   *lc2;
					bool			first = true;

					initStringInfo(&buf);
					foreach(lc2, (List *) ae->rexpr)
					{
						char *elem = ddlii_format_expr((Node *) lfirst(lc2));
						if (!first) appendStringInfoString(&buf, ", ");
						first = false;
						appendStringInfoString(&buf, elem);
						pfree(elem);
					}
					literal_list = buf.data;
				}
				break;

			case AEXPR_BETWEEN:
			case AEXPR_NOT_BETWEEN:
			case AEXPR_BETWEEN_SYM:
			case AEXPR_NOT_BETWEEN_SYM:
			{
				StringInfoData buf;

				mapped_op =
					(ae->kind == AEXPR_BETWEEN)         ? "BETWEEN" :
					(ae->kind == AEXPR_NOT_BETWEEN)     ? "NOT BETWEEN" :
					(ae->kind == AEXPR_BETWEEN_SYM)     ? "BETWEEN SYMMETRIC" :
														  "NOT BETWEEN SYMMETRIC";

				if (ae->rexpr != NULL && IsA(ae->rexpr, List))
				{
					List *bounds = (List *) ae->rexpr;
					if (list_length(bounds) >= 2)
					{
						char *lo = ddlii_format_expr((Node *) linitial(bounds));
						char *hi = ddlii_format_expr((Node *) lsecond(bounds));
						initStringInfo(&buf);
						appendStringInfo(&buf, "%s AND %s", lo, hi);
						pfree(lo); pfree(hi);
						literal_list = buf.data;
					}
				}
				break;
			}

			case AEXPR_LIKE:
			case AEXPR_ILIKE:
			{
				bool is_like = (ae->kind == AEXPR_LIKE);
				bool neg_op  = (op_name &&
								(strcmp(op_name, "!~~") == 0 ||
								 strcmp(op_name, "!~~*") == 0));
				mapped_op = is_like ? (neg_op ? "NOT LIKE"  : "LIKE")
									: (neg_op ? "NOT ILIKE" : "ILIKE");
				if (ae->rexpr != NULL)
					literal = ddlii_format_expr(ae->rexpr);
				break;
			}

			default:
				mapped_op = "UNKNOWN";
				break;
		}

		ddlii_emit_predicate_row(rsinfo, conjunction ? conjunction : "NONE",
								 col, mapped_op, literal, literal_list, negated);

		if (literal)	  pfree(literal);
		if (literal_list) pfree(literal_list);
	}
	else
	{
		/* Unrecognised node: format and emit as UNKNOWN */
		char *expr_str = ddlii_format_expr(predicate);
		ddlii_emit_predicate_row(rsinfo, conjunction ? conjunction : "NONE",
								 NULL, "UNKNOWN", expr_str, NULL, negated);
		pfree(expr_str);
	}
}

/*
 * ddl_instead_of_parse_index_predicates
 *
 * Parse a CREATE INDEX statement and return one row per leaf condition in
 * the WHERE clause.  Returns zero rows when there is no WHERE clause.
 *
 *   conjunction   text     -- 'AND' | 'OR' | 'NONE' (top-level single predicate)
 *   column_name   text     -- referenced column, or NULL for expressions
 *   operator      text     -- '=' | '<' | '>' | '<=' | '>=' | '<>' |
 *                             'IS NULL' | 'IS NOT NULL' | 'IN' | 'NOT IN' |
 *                             'BETWEEN' | 'LIKE' | 'ILIKE' | 'UNKNOWN'
 *   literal       text     -- constant value (NULL for IS NULL / IS NOT NULL)
 *   literal_list  text     -- comma-separated values for IN / BETWEEN
 *   negated       boolean  -- true when wrapped in NOT
 */
PG_FUNCTION_INFO_V1(ddl_instead_of_parse_index_predicates);
Datum
ddl_instead_of_parse_index_predicates(PG_FUNCTION_ARGS)
{
	text		  *stmt_text = PG_GETARG_TEXT_PP(0);
	char		  *stmt_cstr;
	List		  *raw_list;
	RawStmt		  *rawstmt;
	Node		  *stmt;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	IndexStmt	  *is;

	InitMaterializedSRF(fcinfo, 0);

	stmt_cstr = text_to_cstring(stmt_text);
	raw_list  = raw_parser(stmt_cstr, RAW_PARSE_DEFAULT);
	pfree(stmt_cstr);

	if (list_length(raw_list) < 1)
		return (Datum) 0;

	rawstmt = linitial_node(RawStmt, raw_list);
	stmt    = rawstmt->stmt;

	if (stmt == NULL || !IsA(stmt, IndexStmt))
		return (Datum) 0;

	is = (IndexStmt *) stmt;

	if (is->whereClause != NULL)
		ddlii_walk_predicate(rsinfo, is->whereClause, NULL, false);

	return (Datum) 0;
}

/* =========================================================================
 * End of parse_index_columns / parse_index_predicates implementation
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * parse_command_info helpers
 *
 * These walk the raw parse tree of a DDL statement and extract the primary
 * object's type, schema name, object name, and a dotted identity string.
 * This is analogous to pg_event_trigger_ddl_commands() but works pre-
 * execution on raw SQL text so handlers can use it without brittle regex.
 * -------------------------------------------------------------------------
 */

/*
 * Map a subset of ObjectType enum values to human-readable strings.
 * Returns "unknown" for values not listed here (future-proof).
 */
static const char *
ddlii_objecttype_str(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_TABLE:			return "table";
		case OBJECT_INDEX:			return "index";
		case OBJECT_VIEW:			return "view";
		case OBJECT_MATVIEW:		return "materialized view";
		case OBJECT_SEQUENCE:		return "sequence";
		case OBJECT_SCHEMA:			return "schema";
		case OBJECT_FUNCTION:		return "function";
		case OBJECT_PROCEDURE:		return "procedure";
		case OBJECT_ROUTINE:		return "routine";
		case OBJECT_AGGREGATE:		return "aggregate";
		case OBJECT_TYPE:			return "type";
		case OBJECT_DOMAIN:			return "domain";
		case OBJECT_TRIGGER:		return "trigger";
		case OBJECT_RULE:			return "rule";
		case OBJECT_POLICY:			return "policy";
		case OBJECT_COLUMN:			return "column";
		case OBJECT_FOREIGN_TABLE:	return "foreign table";
		case OBJECT_COLLATION:		return "collation";
		case OBJECT_CONVERSION:		return "conversion";
		case OBJECT_EXTENSION:		return "extension";
		case OBJECT_LANGUAGE:		return "language";
		case OBJECT_PUBLICATION:	return "publication";
		case OBJECT_ROLE:			return "role";
		case OBJECT_TABLESPACE:		return "tablespace";
		case OBJECT_FDW:			return "foreign-data wrapper";
		case OBJECT_FOREIGN_SERVER:	return "server";
		case OBJECT_OPERATOR:		return "operator";
		case OBJECT_OPCLASS:		return "operator class";
		case OBJECT_OPFAMILY:		return "operator family";
		case OBJECT_STATISTIC_EXT:	return "statistics";
		case OBJECT_EVENT_TRIGGER:	return "event trigger";
		case OBJECT_ACCESS_METHOD:	return "access method";
		case OBJECT_TSCONFIGURATION: return "text search configuration";
		case OBJECT_TSDICTIONARY:	return "text search dictionary";
		case OBJECT_TSPARSER:		return "text search parser";
		case OBJECT_TSTEMPLATE:		return "text search template";
		default:					return "unknown";
	}
}

/*
 * Extract schema_name and object_name from a qualified-name list
 * (a List of String nodes produced by the parser).
 *
 * Handles 1-part (name), 2-part (schema.name), and 3-part
 * (catalog.schema.name) forms; the last two components are used.
 */
static void
ddlii_name_from_strlist(List *names,
						const char **schema_out, const char **name_out)
{
	int			nnames = list_length(names);

	*schema_out = NULL;
	*name_out = NULL;

	if (nnames == 0)
		return;
	else if (nnames == 1)
		*name_out = strVal(linitial(names));
	else if (nnames == 2)
	{
		*schema_out = strVal(linitial(names));
		*name_out = strVal(lsecond(names));
	}
	else
	{
		/* catalog.schema.name — take the rightmost two elements */
		*schema_out = strVal(list_nth(names, nnames - 2));
		*name_out = strVal(llast(names));
	}
}

/*
 * Append one result row to the SRF tuplestore.
 * Any of the string arguments may be NULL, in which case the column is NULL.
 * object_identity is built automatically as [schema.]name.
 */
static void
ddlii_emit_row(ReturnSetInfo *rsinfo,
			   const char *object_type,
			   const char *schema_name,
			   const char *object_name)
{
	Datum		values[4];
	bool		nulls[4];

	MemSet(nulls, 0, sizeof(nulls));

	if (object_type)
		values[0] = CStringGetTextDatum(object_type);
	else
		nulls[0] = true;

	if (schema_name && schema_name[0] != '\0')
		values[1] = CStringGetTextDatum(schema_name);
	else
		nulls[1] = true;

	if (object_name && object_name[0] != '\0')
		values[2] = CStringGetTextDatum(object_name);
	else
		nulls[2] = true;

	/* object_identity: [schema.]name */
	if (object_name && object_name[0] != '\0')
	{
		if (schema_name && schema_name[0] != '\0')
		{
			StringInfoData buf;

			initStringInfo(&buf);
			appendStringInfo(&buf, "%s.%s", schema_name, object_name);
			values[3] = CStringGetTextDatum(buf.data);
			pfree(buf.data);
		}
		else
			values[3] = CStringGetTextDatum(object_name);
	}
	else
		nulls[3] = true;

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* Convenience: emit a row whose names come from a RangeVar */
static void
ddlii_emit_rangevar(ReturnSetInfo *rsinfo,
					const char *object_type,
					const RangeVar *rv)
{
	ddlii_emit_row(rsinfo, object_type,
				   rv ? rv->schemaname : NULL,
				   rv ? rv->relname    : NULL);
}

/*
 * ddl_instead_of_parse_command_info
 *
 * Parse a DDL statement and return structured information about the primary
 * object(s) it addresses.  Returns a set of rows:
 *
 *   (object_type text, schema_name text, object_name text,
 *    object_identity text)
 *
 * Mirrors the most useful columns from pg_event_trigger_ddl_commands() but
 * works pre-execution on raw SQL text so that handler functions do not need
 * to implement fragile regex-based name extraction.
 *
 * For statements that affect multiple objects (TRUNCATE, DROP TABLE a, b)
 * each object produces its own row.
 *
 * schema_name is NULL when the object type does not have a parent schema
 * (schemas, extensions, roles, tablespaces) or when the statement did not
 * supply an explicit schema qualifier.
 *
 * Note: this function parses the statement with raw_parser, so OIDs are not
 * resolved and system-catalog lookups are not performed.  The names returned
 * are exactly what appeared in the SQL text.
 */
PG_FUNCTION_INFO_V1(ddl_instead_of_parse_command_info);
Datum
ddl_instead_of_parse_command_info(PG_FUNCTION_ARGS)
{
	text	   *cmd_tag_text = PG_GETARG_TEXT_PP(0);
	text	   *stmt_text = PG_GETARG_TEXT_PP(1);
	char	   *stmt_cstr;
	List	   *raw_list;
	RawStmt    *rawstmt;
	Node	   *stmt;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	stmt_cstr = text_to_cstring(stmt_text);
	raw_list = raw_parser(stmt_cstr, RAW_PARSE_DEFAULT);
	pfree(stmt_cstr);

	if (list_length(raw_list) < 1)
		return (Datum) 0;

	rawstmt = linitial_node(RawStmt, raw_list);
	stmt = rawstmt->stmt;
	if (stmt == NULL)
		return (Datum) 0;

	switch (nodeTag(stmt))
	{
		/* ----------------------------------------------------------------
		 * CREATE TABLE / CREATE TABLE AS / CREATE MATERIALIZED VIEW
		 * ---------------------------------------------------------------- */
		case T_CreateStmt:
			ddlii_emit_rangevar(rsinfo, "table",
								((CreateStmt *) stmt)->relation);
			break;

		case T_CreateTableAsStmt:
		{
			CreateTableAsStmt *ctas = (CreateTableAsStmt *) stmt;
			const char *obj_type = (ctas->objtype == OBJECT_MATVIEW) ?
				"materialized view" : "table";

			ddlii_emit_rangevar(rsinfo, obj_type,
								ctas->into ? ctas->into->rel : NULL);
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE VIEW
		 * ---------------------------------------------------------------- */
		case T_ViewStmt:
			ddlii_emit_rangevar(rsinfo, "view",
								((ViewStmt *) stmt)->view);
			break;

		/* ----------------------------------------------------------------
		 * REFRESH MATERIALIZED VIEW
		 * ---------------------------------------------------------------- */
		case T_RefreshMatViewStmt:
			ddlii_emit_rangevar(rsinfo, "materialized view",
								((RefreshMatViewStmt *) stmt)->relation);
			break;

		/* ----------------------------------------------------------------
		 * CREATE SEQUENCE / ALTER SEQUENCE
		 * ---------------------------------------------------------------- */
		case T_CreateSeqStmt:
			ddlii_emit_rangevar(rsinfo, "sequence",
								((CreateSeqStmt *) stmt)->sequence);
			break;

		case T_AlterSeqStmt:
			ddlii_emit_rangevar(rsinfo, "sequence",
								((AlterSeqStmt *) stmt)->sequence);
			break;

		/* ----------------------------------------------------------------
		 * CREATE INDEX
		 *
		 * The index's schema is inherited from the table; object_name is
		 * the index name (may be NULL for unnamed system-generated indexes).
		 * ---------------------------------------------------------------- */
		case T_IndexStmt:
		{
			IndexStmt  *ix = (IndexStmt *) stmt;

			ddlii_emit_row(rsinfo, "index",
						   ix->relation ? ix->relation->schemaname : NULL,
						   ix->idxname);
			break;
		}

		/* ----------------------------------------------------------------
		 * ALTER TABLE / INDEX / VIEW / MATERIALIZED VIEW / FOREIGN TABLE
		 *
		 * objtype tells us what kind of object is being altered.
		 * ---------------------------------------------------------------- */
		case T_AlterTableStmt:
		{
			AlterTableStmt *at = (AlterTableStmt *) stmt;

			ddlii_emit_rangevar(rsinfo,
								ddlii_objecttype_str(at->objtype),
								at->relation);
			break;
		}

		/* ----------------------------------------------------------------
		 * TRUNCATE (can list multiple relations)
		 * ---------------------------------------------------------------- */
		case T_TruncateStmt:
		{
			TruncateStmt *trunc = (TruncateStmt *) stmt;
			ListCell   *lc;

			foreach(lc, trunc->relations)
				ddlii_emit_rangevar(rsinfo, "table",
									(RangeVar *) lfirst(lc));
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE SCHEMA
		 * ---------------------------------------------------------------- */
		case T_CreateSchemaStmt:
		{
			CreateSchemaStmt *cs = (CreateSchemaStmt *) stmt;

			ddlii_emit_row(rsinfo, "schema", NULL, cs->schemaname);
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE FUNCTION / CREATE PROCEDURE
		 * ---------------------------------------------------------------- */
		case T_CreateFunctionStmt:
		{
			CreateFunctionStmt *cf = (CreateFunctionStmt *) stmt;
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			ddlii_name_from_strlist(cf->funcname, &schema_name, &obj_name);
			ddlii_emit_row(rsinfo,
						   cf->is_procedure ? "procedure" : "function",
						   schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * CALL procedure(...)
		 * ---------------------------------------------------------------- */
		case T_CallStmt:
		{
			CallStmt   *cs = (CallStmt *) stmt;
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			if (cs->funccall)
				ddlii_name_from_strlist(cs->funccall->funcname,
										&schema_name, &obj_name);
			ddlii_emit_row(rsinfo, "procedure", schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * RENAME (ALTER ... RENAME TO ...)
		 *
		 * Reports the object's current (pre-rename) name.
		 * ---------------------------------------------------------------- */
		case T_RenameStmt:
		{
			RenameStmt *rs = (RenameStmt *) stmt;
			const char *obj_type = ddlii_objecttype_str(rs->renameType);
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			if (rs->relation)
			{
				schema_name = rs->relation->schemaname;
				obj_name = rs->relation->relname;
			}
			else if (rs->object)
			{
				if (IsA(rs->object, List))
					ddlii_name_from_strlist((List *) rs->object,
											&schema_name, &obj_name);
				else if (IsA(rs->object, String))
					obj_name = strVal((String *) rs->object);
				else if (IsA(rs->object, ObjectWithArgs))
					ddlii_name_from_strlist(
						((ObjectWithArgs *) rs->object)->objname,
						&schema_name, &obj_name);
			}
			else if (rs->subname)
				/* e.g. ALTER SCHEMA old RENAME TO new */
				obj_name = rs->subname;

			ddlii_emit_row(rsinfo, obj_type, schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * ALTER object SET SCHEMA
		 * ---------------------------------------------------------------- */
		case T_AlterObjectSchemaStmt:
		{
			AlterObjectSchemaStmt *aoss = (AlterObjectSchemaStmt *) stmt;
			const char *obj_type = ddlii_objecttype_str(aoss->objectType);
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			if (aoss->relation)
			{
				schema_name = aoss->relation->schemaname;
				obj_name = aoss->relation->relname;
			}
			else if (aoss->object)
			{
				if (IsA(aoss->object, List))
					ddlii_name_from_strlist((List *) aoss->object,
											&schema_name, &obj_name);
				else if (IsA(aoss->object, String))
					obj_name = strVal((String *) aoss->object);
				else if (IsA(aoss->object, ObjectWithArgs))
					ddlii_name_from_strlist(
						((ObjectWithArgs *) aoss->object)->objname,
						&schema_name, &obj_name);
				else if (IsA(aoss->object, TypeName))
					ddlii_name_from_strlist(
						((TypeName *) aoss->object)->names,
						&schema_name, &obj_name);
			}
			ddlii_emit_row(rsinfo, obj_type, schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * DROP (various types, potentially multiple objects per statement)
		 * ---------------------------------------------------------------- */
		case T_DropStmt:
		{
			DropStmt   *drop = (DropStmt *) stmt;
			const char *obj_type = ddlii_objecttype_str(drop->removeType);
			ListCell   *lc;

			foreach(lc, drop->objects)
			{
				Node	   *obj = lfirst(lc);
				const char *schema_name = NULL;
				const char *obj_name = NULL;

				switch (drop->removeType)
				{
					/*
					 * Relation-like objects: each element is a List of String
					 * (a qualified name such as [schema, name]).
					 */
					case OBJECT_TABLE:
					case OBJECT_INDEX:
					case OBJECT_VIEW:
					case OBJECT_MATVIEW:
					case OBJECT_SEQUENCE:
					case OBJECT_FOREIGN_TABLE:
						if (IsA(obj, List))
							ddlii_name_from_strlist((List *) obj,
													&schema_name, &obj_name);
						break;

					/*
					 * Objects with no parent schema: each element is a bare
					 * String (schema name, extension name, etc.).
					 */
					case OBJECT_SCHEMA:
					case OBJECT_LANGUAGE:
					case OBJECT_EXTENSION:
					case OBJECT_TABLESPACE:
					case OBJECT_ROLE:
					case OBJECT_PUBLICATION:
					case OBJECT_SUBSCRIPTION:
						if (IsA(obj, String))
							obj_name = strVal((String *) obj);
						break;

					/*
					 * Function-like objects: each element is ObjectWithArgs
					 * where objname is a qualified-name list.
					 */
					case OBJECT_FUNCTION:
					case OBJECT_PROCEDURE:
					case OBJECT_ROUTINE:
					case OBJECT_AGGREGATE:
					case OBJECT_OPERATOR:
						if (IsA(obj, ObjectWithArgs))
							ddlii_name_from_strlist(
								((ObjectWithArgs *) obj)->objname,
								&schema_name, &obj_name);
						break;

					/*
					 * Type / domain: TypeName whose names is a qualified list.
					 */
					case OBJECT_TYPE:
					case OBJECT_DOMAIN:
						if (IsA(obj, TypeName))
							ddlii_name_from_strlist(
								((TypeName *) obj)->names,
								&schema_name, &obj_name);
						break;

					default:
						/* Best-effort for any unrecognised sub-type */
						if (IsA(obj, List))
							ddlii_name_from_strlist((List *) obj,
													&schema_name, &obj_name);
						else if (IsA(obj, String))
							obj_name = strVal((String *) obj);
						break;
				}

				ddlii_emit_row(rsinfo, obj_type, schema_name, obj_name);
			}
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE TYPE (composite, enum, range)
		 * ---------------------------------------------------------------- */
		case T_CompositeTypeStmt:
			ddlii_emit_rangevar(rsinfo, "type",
								((CompositeTypeStmt *) stmt)->typevar);
			break;

		case T_CreateEnumStmt:
		{
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			ddlii_name_from_strlist(((CreateEnumStmt *) stmt)->typeName,
									&schema_name, &obj_name);
			ddlii_emit_row(rsinfo, "type", schema_name, obj_name);
			break;
		}

		case T_CreateRangeStmt:
		{
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			ddlii_name_from_strlist(((CreateRangeStmt *) stmt)->typeName,
									&schema_name, &obj_name);
			ddlii_emit_row(rsinfo, "type", schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE DOMAIN
		 * ---------------------------------------------------------------- */
		case T_CreateDomainStmt:
		{
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			ddlii_name_from_strlist(((CreateDomainStmt *) stmt)->domainname,
									&schema_name, &obj_name);
			ddlii_emit_row(rsinfo, "domain", schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE TRIGGER
		 * ---------------------------------------------------------------- */
		case T_CreateTrigStmt:
		{
			CreateTrigStmt *ct = (CreateTrigStmt *) stmt;

			ddlii_emit_row(rsinfo, "trigger",
						   ct->relation ? ct->relation->schemaname : NULL,
						   ct->trigname);
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE POLICY
		 * ---------------------------------------------------------------- */
		case T_CreatePolicyStmt:
		{
			CreatePolicyStmt *cp = (CreatePolicyStmt *) stmt;

			ddlii_emit_row(rsinfo, "policy",
						   cp->table ? cp->table->schemaname : NULL,
						   cp->policy_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * CREATE EXTENSION / ALTER EXTENSION ... UPDATE
		 * ---------------------------------------------------------------- */
		case T_CreateExtensionStmt:
			ddlii_emit_row(rsinfo, "extension", NULL,
						   ((CreateExtensionStmt *) stmt)->extname);
			break;

		case T_AlterExtensionStmt:
			ddlii_emit_row(rsinfo, "extension", NULL,
						   ((AlterExtensionStmt *) stmt)->extname);
			break;

		/* ----------------------------------------------------------------
		 * COMMENT ON
		 * ---------------------------------------------------------------- */
		case T_CommentStmt:
		{
			CommentStmt *cm = (CommentStmt *) stmt;
			const char *obj_type = ddlii_objecttype_str(cm->objtype);
			const char *schema_name = NULL;
			const char *obj_name = NULL;

			if (cm->object)
			{
				if (IsA(cm->object, RangeVar))
				{
					schema_name = ((RangeVar *) cm->object)->schemaname;
					obj_name = ((RangeVar *) cm->object)->relname;
				}
				else if (IsA(cm->object, List))
					ddlii_name_from_strlist((List *) cm->object,
											&schema_name, &obj_name);
				else if (IsA(cm->object, String))
					obj_name = strVal((String *) cm->object);
				else if (IsA(cm->object, TypeName))
					ddlii_name_from_strlist(
						((TypeName *) cm->object)->names,
						&schema_name, &obj_name);
				else if (IsA(cm->object, ObjectWithArgs))
					ddlii_name_from_strlist(
						((ObjectWithArgs *) cm->object)->objname,
						&schema_name, &obj_name);
			}
			ddlii_emit_row(rsinfo, obj_type, schema_name, obj_name);
			break;
		}

		/* ----------------------------------------------------------------
		 * Fallback: return a single row with the command_tag as object_type
		 * and NULL for the name columns.  This catches any DDL node type
		 * not explicitly handled above.
		 * ---------------------------------------------------------------- */
		default:
		{
			char	   *tag = text_to_cstring(cmd_tag_text);

			ddlii_emit_row(rsinfo, tag, NULL, NULL);
			pfree(tag);
			break;
		}
	}

	return (Datum) 0;
}

/*
 * Validate that fnoid is a function with signature (text, text) RETURNS text.
 * Called at rule-registration time only (add_rule), not on the hot path.
 */
void
ddl_instead_of_validate_handler(Oid fnoid)
{
	HeapTuple	tup;
	Form_pg_proc procform;
	bool		ok;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "ddl_instead_of: cache lookup failed for function %u", fnoid);

	procform = (Form_pg_proc) GETSTRUCT(tup);

	ok = (procform->pronargs == 2 &&
		  procform->proargtypes.values[0] == TEXTOID &&
		  procform->proargtypes.values[1] == TEXTOID &&
		  procform->prorettype == TEXTOID);

	ReleaseSysCache(tup);

	if (!ok)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ddl_instead_of handler must have signature"
						" (text, text) RETURNS text")));
}

PG_FUNCTION_INFO_V1(ddl_instead_of_validate_handler_sql);
Datum
ddl_instead_of_validate_handler_sql(PG_FUNCTION_ARGS)
{
	Oid		fnoid = PG_GETARG_OID(0);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("ddl_instead_of: superuser privileges required")));

	ddl_instead_of_validate_handler(fnoid);
	PG_RETURN_VOID();
}

/*
 * Query ddl_instead_of.intercept_rule for handlers matching command_tag.
 * Returns the number of OIDs written into handlers[].
 *
 * On any SPI or catalog failure we emit a WARNING and return 0 so that the
 * original DDL is executed unchanged rather than failing unexpectedly.
 */
static int
load_handler_oids(const char *command_tag, Oid *handlers, int maxhandlers)
{
	Oid			argtypes[1];
	Datum		values[1];
	char		nulls[1];
	Datum		tag_datum;
	int			i;
	int			n = 0;
	bool		isnull;

	DDL_DBG("load_handler_oids: querying rules for tag=\"%s\"", command_tag);

	/*
	 * Allocate tag_datum BEFORE SPI_connect().  SPI_connect() switches
	 * CurrentMemoryContext to its internal procCxt, so any palloc after that
	 * point lands in procCxt.  SPI_finish() deletes procCxt, which would make
	 * tag_datum a dangling pointer and the pfree below a use-after-free,
	 * corrupting the allocator and leaving CurrentMemoryContext at a garbage
	 * value.  Allocating here keeps tag_datum in the caller's context where
	 * the pfree after SPI_finish() is safe.
	 */
	tag_datum = CStringGetTextDatum(command_tag);

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		pfree(DatumGetPointer(tag_datum));
		ereport(WARNING,
				(errmsg("ddl_instead_of: SPI_connect failed, skipping intercept")));
		return 0;
	}

	argtypes[0] = TEXTOID;
	values[0] = tag_datum;
	nulls[0] = ' ';

	PG_TRY();
	{
		if (SPI_execute_with_args("SELECT r.handler::oid AS fn "
								  "FROM ddl_instead_of.intercept_rule r "
								  "WHERE r.enabled "
								  "AND (r.command_tag = $1 OR r.command_tag = '*') "
								  "ORDER BY r.priority, r.rule_name",
								  1, argtypes, values, nulls, true, 0) == SPI_OK_SELECT
			&& SPI_processed > 0)
		{
			SPITupleTable *tuptab = SPI_tuptable;
			TupleDesc	tupdesc = tuptab->tupdesc;

			for (i = 0; i < (int) SPI_processed && n < maxhandlers; i++)
			{
				Datum		d = SPI_getbinval(tuptab->vals[i], tupdesc, 1,
											  &isnull);

				if (!isnull)
					handlers[n++] = DatumGetObjectId(d);
			}
		}
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();

		FlushErrorState();

		/*
		 * ERRCODE_UNDEFINED_TABLE  (42P01) and ERRCODE_INVALID_SCHEMA_NAME
		 * (3F000) both indicate that the ddl_instead_of extension has not been
		 * installed in this database.  This is a fully expected condition when
		 * shared_preload_libraries includes the library but the extension has
		 * not been created in every database.  Treat it as a silent no-op so
		 * that users never see spurious warnings in databases that deliberately
		 * omit the extension.
		 *
		 * All other errors are genuinely unexpected and still deserve a WARNING
		 * so that operators can investigate.
		 */
		if (edata->sqlerrcode != ERRCODE_UNDEFINED_TABLE &&
			edata->sqlerrcode != ERRCODE_INVALID_SCHEMA_NAME)
			ereport(WARNING,
					(errmsg("ddl_instead_of: error querying intercept_rule, "
							"skipping intercept: %s", edata->message)));

		DDL_DBG("load_handler_oids: skipping intercept (%s)",
				edata->message);
		FreeErrorData(edata);
		n = 0;
	}
	PG_END_TRY();

	SPI_finish();
	pfree(DatumGetPointer(tag_datum));

	DDL_DBG("load_handler_oids: found %d handler(s) for tag=\"%s\"",
			n, command_tag);
	return n;
}

/*
 * Invoke handler fnoid(command_tag, statement_text) and return the result as a
 * palloc'd C string in the caller's memory context, or NULL if the handler
 * returned NULL or an empty string.
 *
 * Caller is responsible for pfree'ing the returned string.
 *
 * We do NOT switch CurrentMemoryContext before calling fmgr_info or
 * FunctionCallInvoke.  fmgr_info captures CurrentMemoryContext as fn_mcxt,
 * and the SQL/PL function executors (fmgr_sql, plpgsql_call_handler) create
 * their per-call cache sub-contexts from fn_mcxt.  If fn_mcxt points to a
 * short-lived throw-away context, those sub-contexts receive invalid addresses
 * and the next palloc inside the executor crashes with SIGSEGV.
 *
 * Instead, the two argument text Datums are allocated in the caller's context
 * and freed explicitly after the call.  The result Datum (and any detoasted
 * copy of it) is also in the caller's context; we copy the C string out of it
 * and then free the original.
 */
static char *
call_handler(Oid fnoid, const char *command_tag, const char *fragment)
{
	FmgrInfo	flinfo;
	LOCAL_FCINFO(fcinfo, 2);
	Datum		arg0;
	Datum		arg1;
	Datum		result;
	char	   *out = NULL;

	/*
	 * fmgr_info runs in the caller's CurrentMemoryContext, so fn_mcxt is a
	 * long-lived context that the SQL/PL executors can safely use for their
	 * internal function-cache sub-contexts.
	 */
	fmgr_info(fnoid, &flinfo);

	InitFunctionCallInfoData(*fcinfo, &flinfo, 2,
							 InvalidOid, NULL, NULL);

	arg0 = CStringGetTextDatum(command_tag);
	arg1 = CStringGetTextDatum(fragment);

	fcinfo->args[0].value = arg0;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg1;
	fcinfo->args[1].isnull = false;

	PG_TRY();
	{
		result = FunctionCallInvoke(fcinfo);

		if (!fcinfo->isnull)
		{
			/*
			 * DatumGetTextPP may return a detoasted palloc'd copy.  Copy the
			 * C string out, then free the (possibly detoasted) text Datum so
			 * we don't leave a large buffer behind.
			 */
			text	   *txt = DatumGetTextPP(result);

			if (VARSIZE_ANY_EXHDR(txt) > 0)
				out = text_to_cstring(txt);

			/* Free the detoasted copy if it differs from the raw datum. */
			if ((Pointer) txt != DatumGetPointer(result))
				pfree(txt);
		}
	}
	PG_CATCH();
	{
		pfree(DatumGetPointer(arg0));
		pfree(DatumGetPointer(arg1));
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(DatumGetPointer(arg0));
	pfree(DatumGetPointer(arg1));
	return out;
}

/*
 * Extract the utility statement text from queryString using stmt_location and
 * stmt_len stored in pstmt.  Returns a palloc'd C string in the current memory
 * context, or NULL when queryString is unavailable.
 */
static char *
extract_statement_text(const PlannedStmt *pstmt, const char *queryString)
{
	int			loc = pstmt->stmt_location;
	int			len = pstmt->stmt_len;
	int			qlen;
	char	   *buf;

	if (queryString == NULL)
		return NULL;

	qlen = (int) strlen(queryString);

	if (loc < 0)
		loc = 0;
	if (loc > qlen)
		loc = qlen;

	if (len < 0)
		len = qlen - loc;
	if (loc + len > qlen)
		len = qlen - loc;
	if (len < 0)
		len = 0;

	buf = (char *) palloc((Size) len + 1);
	if (len > 0)
		memcpy(buf, queryString + loc, len);
	buf[len] = '\0';
	return buf;
}

/*
 * Parse and analyze newsql, then install the single resulting utility Node
 * into pstmt.
 *
 * pstmt must already be a private copy (not the shared read-only tree).
 *
 * We deliberately go through the full parse + analyze pipeline
 * (pg_analyze_and_rewrite_fixedparams) rather than using raw_parser alone.
 * For statements such as CALL, the executor (ExecuteCallStmt) expects semantic
 * fields (e.g. CallStmt.funcexpr) to have been populated by the analysis
 * phase.  If we skip analysis and pass a raw-parsed node, ExecuteCallStmt
 * crashes with a NULL-dereference (rbx=0, fault at 0x4) because funcexpr is
 * NULL.  In YugabyteDB the analysis is done by exec_simple_query before
 * ProcessUtility is called; since we bypass that path we must do it ourselves.
 */
static void
apply_rewrite(PlannedStmt *pstmt, const char *newsql)
{
	List	   *raw_list;
	List	   *query_list;
	RawStmt    *rawstmt;
	Query	   *query;

	raw_list = raw_parser(newsql, RAW_PARSE_DEFAULT);

	if (list_length(raw_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ddl_instead_of rewrite must yield exactly one statement"),
				 errdetail("Rewritten SQL was: %s", newsql)));

	rawstmt = linitial_node(RawStmt, raw_list);

	if (rawstmt->stmt == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ddl_instead_of rewrite produced an empty parse tree")));

	/*
	 * Run semantic analysis.  For CALL this populates CallStmt.funcexpr;
	 * for most DDL it is a no-op.  The call also applies any relevant query
	 * rewrite rules (also a no-op for utility statements in practice).
	 */
	query_list = pg_analyze_and_rewrite_fixedparams(rawstmt, newsql,
													 NULL, 0, NULL);

	if (list_length(query_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ddl_instead_of rewrite did not produce exactly one analyzed statement")));

	query = linitial_node(Query, query_list);

	pstmt->utilityStmt = query->utilityStmt;
}

static void
ddl_instead_of_ProcessUtility(PlannedStmt *pstmt,
							  const char *queryString,
							  bool readOnlyTree,
							  ProcessUtilityContext context,
							  ParamListInfo params,
							  QueryEnvironment *queryEnv,
							  DestReceiver *dest,
							  QueryCompletion *qc)
{
	Node	   *utility;
	CommandTag	tag;
	const char *tagname;
	char	   *fragment = NULL;
	char	   *rewritten = NULL;
	Oid			handlers[MAX_HANDLERS];
	int			nhandlers;
	int			i;

	/*
	 * Recursion guard: when ddl_instead_of_depth > 0 we are already inside an
	 * intercept dispatch (e.g. chain_ProcessUtility fired another utility).
	 * Skip interception entirely to avoid infinite loops.
	 */
	if (ddl_instead_of_depth > MAX_RECURSION_GUARD)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("ddl_instead_of: maximum recursion depth exceeded")));

	if (ddl_instead_of_depth > 0 ||
		!IsUnderPostmaster ||
		IsBinaryUpgrade ||
		IsYsqlUpgrade ||
		creating_extension ||
		pstmt->commandType != CMD_UTILITY ||
		context != PROCESS_UTILITY_TOPLEVEL ||
		pstmt->utilityStmt == NULL)
	{
		DDL_DBG("skipping interception (depth=%d creating_extension=%d"
				" context=%d commandType=%d)",
				ddl_instead_of_depth, (int) creating_extension,
				(int) context, (int) pstmt->commandType);
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	utility = pstmt->utilityStmt;
	tag = CreateCommandTag(utility);
	tagname = GetCommandTagName(tag);

	fragment = extract_statement_text(pstmt, queryString);
	if (fragment == NULL)
	{
		DDL_DBG("tag=\"%s\" no queryString available, passing through", tagname);
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	DDL_DBG("intercepted tag=\"%s\" fragment=\"%.*s%s\"",
			tagname,
			(int) Min(strlen(fragment), DDL_DBG_SQL_MAXLEN), fragment,
			strlen(fragment) > DDL_DBG_SQL_MAXLEN ? "..." : "");

	nhandlers = load_handler_oids(tagname, handlers, MAX_HANDLERS);

	DDL_DBG("tag=\"%s\" matched %d handler(s)", tagname, nhandlers);

	ddl_instead_of_depth++;
	PG_TRY();
	{
		for (i = 0; i < nhandlers; i++)
		{
			DDL_DBG("calling handler[%d] oid=%u for tag=\"%s\"",
					i, handlers[i], tagname);

			rewritten = call_handler(handlers[i], tagname, fragment);

			if (rewritten != NULL)
			{
				DDL_DBG("handler[%d] oid=%u returned rewrite: \"%.*s%s\"",
						i, handlers[i],
						(int) Min(strlen(rewritten), DDL_DBG_SQL_MAXLEN), rewritten,
						strlen(rewritten) > DDL_DBG_SQL_MAXLEN ? "..." : "");

				/*
				 * Fix #1: if the incoming pstmt is shared/read-only, make a
				 * private copy before mutating utilityStmt.  Pass
				 * readOnlyTree=false to the chain since we own the copy.
				 */
				if (readOnlyTree)
					pstmt = copyObject(pstmt);

				apply_rewrite(pstmt, rewritten);

				DDL_DBG("rewrite applied successfully for tag=\"%s\"", tagname);

				/*
				 * Pass the rewritten SQL as queryString so downstream hooks
				 * (e.g. pg_stat_statements) record the correct text.  Keep
				 * rewritten alive across the chain call; free it after.
				 */
				chain_ProcessUtility(pstmt, rewritten, false, context,
									 params, queryEnv, dest, qc);
				pfree(rewritten);
				rewritten = NULL;
				goto done;
			}

			DDL_DBG("handler[%d] oid=%u returned NULL, continuing to next handler",
					i, handlers[i]);
		}

		/* No handler rewrote the statement; execute as-is. */
		DDL_DBG("no handler rewrote tag=\"%s\", executing original statement",
				tagname);
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
	}
	PG_CATCH();
	{
		/* Fix #2: free rewritten if apply_rewrite or the chain threw. */
		if (rewritten != NULL)
		{
			pfree(rewritten);
			rewritten = NULL;
		}
		pfree(fragment);
		ddl_instead_of_depth--;
		PG_RE_THROW();
	}
	PG_END_TRY();

done:
	pfree(fragment);
	ddl_instead_of_depth--;
}

static void
chain_ProcessUtility(PlannedStmt *pstmt,
					 const char *queryString,
					 bool readOnlyTree,
					 ProcessUtilityContext context,
					 ParamListInfo params,
					 QueryEnvironment *queryEnv,
					 DestReceiver *dest,
					 QueryCompletion *qc)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

void
_PG_init(void)
{
	if (IsBinaryUpgrade)
		return;

	if (ProcessUtility_hook == ddl_instead_of_ProcessUtility)
		return;

	/*
	 * Register the debug GUC before installing the hook so it is available
	 * as soon as the library is loaded.
	 *
	 * SET ddl_instead_of.debug = on;   -- enable per-session
	 * SET ddl_instead_of.debug = off;  -- disable
	 *
	 * Messages are emitted at LOG level so they appear in the server log and
	 * at the client when client_min_messages >= log (the default).
	 */
	DefineCustomBoolVariable("ddl_instead_of.debug",
							 "Log rule matching and rewrite decisions.",
							 "When on, emits LOG messages showing which command tag was "
							 "intercepted, how many handlers matched, what each handler "
							 "returned, and whether a rewrite was applied or the original "
							 "statement executed unchanged.",
							 &ddl_instead_of_debug,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("ddl_instead_of");

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = ddl_instead_of_ProcessUtility;
}

/* Fix #8: restore the previous hook when the library is unloaded. */
void
_PG_fini(void)
{
	ProcessUtility_hook = prev_ProcessUtility;
}
