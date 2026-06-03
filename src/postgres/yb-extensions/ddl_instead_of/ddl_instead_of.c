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
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/syscache.h"

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
 * Parse newsql and install the single resulting utility Node into pstmt.
 *
 * pstmt must already be a private copy (not the shared read-only tree).
 * The RawStmt wrapper and List cell from raw_parser are freed; only the inner
 * Node survives in pstmt->utilityStmt.
 */
static void
apply_rewrite(PlannedStmt *pstmt, const char *newsql)
{
	List	   *parsetree_list;
	RawStmt    *rawstmt;
	Node	   *stmt;

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

	/*
	 * Detach the inner Node, then free the RawStmt wrapper and the List cell.
	 * list_free only frees the List cells, not the pointed-to nodes, so we
	 * must pfree rawstmt explicitly.
	 */
	stmt = rawstmt->stmt;
	rawstmt->stmt = NULL;
	pfree(rawstmt);
	list_free(parsetree_list);

	pstmt->utilityStmt = stmt;
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

				/* rewritten is no longer needed once the parse tree is built */
				pfree(rewritten);
				rewritten = NULL;

				chain_ProcessUtility(pstmt, queryString, false, context,
									 params, queryEnv, dest, qc);
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
