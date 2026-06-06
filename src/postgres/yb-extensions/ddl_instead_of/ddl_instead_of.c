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
		ereport(WARNING,
				(errmsg("ddl_instead_of: error querying intercept_rule, "
						"skipping intercept: %s", edata->message)));
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
