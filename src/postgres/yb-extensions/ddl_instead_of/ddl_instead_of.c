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
#include "parser/parser.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

#define MAX_HANDLERS 64
#define MAX_RECURSION_GUARD 32

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static int	ddl_instead_of_depth = 0;

static void chain_ProcessUtility(PlannedStmt *pstmt,
								   const char *queryString,
								   bool readOnlyTree,
								   ProcessUtilityContext context,
								   ParamListInfo params,
								   QueryEnvironment *queryEnv,
								   DestReceiver *dest,
								   QueryCompletion *qc);

static void
validate_handler_signature(Oid fnoid)
{
	HeapTuple	tup;
	Form_pg_proc procform;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", fnoid);

	procform = (Form_pg_proc) GETSTRUCT(tup);

	if (procform->pronargs != 2)
	{
		ReleaseSysCache(tup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ddl_instead_of handler must have exactly two arguments")));
	}

	if (procform->proargtypes.values[0] != TEXTOID ||
		procform->proargtypes.values[1] != TEXTOID)
	{
		ReleaseSysCache(tup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ddl_instead_of handler arguments must be (text, text)")));
	}

	if (procform->prorettype != TEXTOID)
	{
		ReleaseSysCache(tup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ddl_instead_of handler must return text")));
	}

	ReleaseSysCache(tup);
}

/*
 * Copy handler Oids from ddl_instead_of.intercept_rule.  On any SPI or catalog
 * failure, returns 0 (caller treats as no rules).
 */
static int
load_handler_oids(const char *command_tag, Oid *handlers, int maxhandlers)
{
	Oid			argtypes[1];
	Datum		values[1];
	char		nulls[1];
	int			i;
	int			n;
	bool		isnull;

	if (SPI_connect() != SPI_OK_CONNECT)
		return 0;

	n = 0;
	PG_TRY();
	{
		argtypes[0] = TEXTOID;
		values[0] = CStringGetTextDatum(command_tag);
		nulls[0] = ' ';

		if (SPI_execute_with_args("SELECT r.handler::oid AS fn "
								  "FROM ddl_instead_of.intercept_rule r "
								  "WHERE r.enabled "
								  "AND (r.command_tag = $1 OR r.command_tag = '*') "
								  "ORDER BY r.priority, r.rule_name",
								  1, argtypes, values, nulls, true, 0) == SPI_OK_SELECT &&
			SPI_processed > 0)
		{
			SPITupleTable *tuptab = SPI_tuptable;
			TupleDesc	tupdesc = tuptab->tupdesc;

			for (i = 0; i < (int) SPI_processed && n < maxhandlers; i++)
			{
				Datum		d;

				d = SPI_getbinval(tuptab->vals[i], tupdesc, 1, &isnull);
				if (!isnull)
					handlers[n++] = DatumGetObjectId(d);
			}
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
		n = 0;
	}
	PG_END_TRY();

	SPI_finish();
	return n;
}

static char *
call_handler(Oid fnoid, const char *command_tag, const char *fragment)
{
	FmgrInfo	flinfo;
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;
	char	   *out;

	validate_handler_signature(fnoid);

	fmgr_info(fnoid, &flinfo);

	InitFunctionCallInfoData(*fcinfo, &flinfo, 2,
							 InvalidOid, NULL, NULL);

	fcinfo->args[0].value = CStringGetTextDatum(command_tag);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = CStringGetTextDatum(fragment);
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	if (fcinfo->isnull)
		return NULL;

	out = text_to_cstring(DatumGetTextPP(result));
	if (out[0] == '\0')
	{
		pfree(out);
		return NULL;
	}

	return out;
}

/*
 * Extract the current utility statement text from queryString using
 * stmt_location and stmt_len from PlannedStmt.
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

	qlen = strlen(queryString);

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

static void
apply_rewrite(PlannedStmt *pstmt, char *newsql)
{
	List	   *parsetree_list;
	RawStmt    *rawstmt;

	parsetree_list = raw_parser(newsql, RAW_PARSE_DEFAULT);

	if (list_length(parsetree_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ddl_instead_of rewrite must yield exactly one statement"),
				 errdetail("Rewritten SQL was: %s", newsql)));

	rawstmt = linitial_node(RawStmt, parsetree_list);

	if (rawstmt->stmt == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ddl_instead_of rewrite produced an empty parse tree")));

	pstmt->utilityStmt = rawstmt->stmt;
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
	char	   *fragment;
	Oid			handlers[MAX_HANDLERS];
	int			nhandlers;
	int			i;

	if (ddl_instead_of_depth > MAX_RECURSION_GUARD)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("ddl_instead_of: exceeded recursion guard")));

	if (ddl_instead_of_depth > 0)
	{
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	if (!IsUnderPostmaster || IsBinaryUpgrade || IsYsqlUpgrade || creating_extension)
	{
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	if (pstmt->commandType != CMD_UTILITY)
	{
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	if (context != PROCESS_UTILITY_TOPLEVEL)
	{
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	utility = pstmt->utilityStmt;
	if (utility == NULL)
	{
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	tag = CreateCommandTag(utility);
	tagname = GetCommandTagName(tag);

	fragment = extract_statement_text(pstmt, queryString);
	if (fragment == NULL)
	{
		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	nhandlers = load_handler_oids(tagname, handlers, MAX_HANDLERS);

	ddl_instead_of_depth++;
	PG_TRY();
	{
		if (nhandlers > 0)
		{
			for (i = 0; i < nhandlers; i++)
			{
				char	   *rewritten;

				rewritten = call_handler(handlers[i], tagname, fragment);
				if (rewritten != NULL)
				{
					apply_rewrite(pstmt, rewritten);
					pfree(rewritten);
					break;
				}
			}
		}

		chain_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		if (fragment != NULL)
		{
			pfree(fragment);
			fragment = NULL;
		}
		ddl_instead_of_depth--;
	}
	PG_END_TRY();
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

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = ddl_instead_of_ProcessUtility;
}
