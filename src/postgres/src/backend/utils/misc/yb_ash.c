/*-------------------------------------------------------------------------
 *
 * yb_ash.c
 *    Utilities for Active Session History/Yugabyte (Postgres layer) integration
 *    that have to be defined on the PostgreSQL side.
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/yb_ash.c
 *
 *-------------------------------------------------------------------------
 */

#include "yb_ash.h"

#include "access/hash.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "pg_yb_utils.h"
#include "yb/yql/pggate/ybc_pg_typedefs.h"
#include "yb/yql/pggate/ybc_pggate.h"
#include "yb/yql/pggate/util/ybc_util.h"

/* The number of columns in different versions of the view */
#define ACTIVE_SESSION_HISTORY_COLS_V1 12
#define ACTIVE_SESSION_HISTORY_COLS_V2 13
#define ACTIVE_SESSION_HISTORY_COLS_V3 14

#define MAX_NESTED_QUERY_LEVEL 64

#define set_query_id() (nested_level == 0 || \
	(yb_ash_track_nested_queries != NULL && yb_ash_track_nested_queries()))

/* GUC variables */
bool yb_ash_enable_infra;
bool yb_enable_ash;
int yb_ash_circular_buffer_size;
int yb_ash_sampling_interval_ms;
int yb_ash_sample_size;

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Flags set by interrupt handlers for later service in the main loop. */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

YbAshTrackNestedQueries yb_ash_track_nested_queries = NULL;

typedef struct YbAsh
{
	LWLock		lock;			/* Protects the circular buffer */
	int			index;			/* Index to insert new buffer entry */
	int			max_entries;	/* Maximum # of entries in the buffer */
	YBCAshSample circular_buffer[FLEXIBLE_ARRAY_MEMBER];
} YbAsh;

typedef struct YbAshNestedQueryIdStack
{
	int			top_index;		/* top index of the stack, -1 for empty stack */
	/* number of query ids not pushed due to the stack size being full */
	int			num_query_ids_not_pushed;
	uint64		query_ids[MAX_NESTED_QUERY_LEVEL];
} YbAshNestedQueryIdStack;

static YbAsh *yb_ash = NULL;
static YbAshNestedQueryIdStack query_id_stack;
static int nested_level = 0;

static void YbAshInstallHooks(void);
static int yb_ash_cb_max_entries(void);
static void YbAshSetQueryId(uint64 query_id);
static void YbAshResetQueryId(void);
static uint64 yb_ash_utility_query_id(const char *query, int query_len,
									  int query_location);
static void YbAshAcquireBufferLock(bool exclusive);
static void YbAshReleaseBufferLock();
static bool YbAshNestedQueryIdStackPush(uint64 query_id);
static uint64 YbAshNestedQueryIdStackPop(void);

static void yb_ash_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void yb_ash_ExecutorRun(QueryDesc *queryDesc,
							   ScanDirection direction,
							   uint64 count, bool execute_once);
static void yb_ash_ExecutorFinish(QueryDesc *queryDesc);
static void yb_ash_ExecutorEnd(QueryDesc *queryDesc);
static void yb_ash_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								  ProcessUtilityContext context, ParamListInfo params,
								  QueryEnvironment *queryEnv, DestReceiver *dest,
								  char *completionTag);

static const unsigned char *get_yql_endpoint_tserver_uuid();
static void copy_pgproc_sample_fields(PGPROC *proc);
static void copy_non_pgproc_sample_fields(float8 sample_weight, TimestampTz sample_time);
static void YbAshIncrementCircularBufferIndex(void);
static YBCAshSample *YbAshGetNextCircularBufferSlot(void);

static void uchar_to_uuid(unsigned char *in, pg_uuid_t *out);
static void client_ip_to_string(unsigned char *client_addr, uint16 client_port,
								uint8_t addr_family, char *client_ip);

bool
yb_enable_ash_check_hook(bool *newval, void **extra, GucSource source)
{
	if (*newval && !yb_ash_enable_infra)
	{
		GUC_check_errdetail("ysql_yb_ash_enable_infra must be enabled.");
		return false;
	}
	return true;
}

void
YbAshRegister(void)
{
	BackgroundWorker worker;
	memset(&worker, 0, sizeof(worker));
	sprintf(worker.bgw_name, "yb_ash collector");
	sprintf(worker.bgw_type, "yb_ash collector");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	/* Value of 1 allows the background worker for yb_ash to restart */
	worker.bgw_restart_time = 1;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "YbAshMain");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&worker);
}

void
YbAshInit(void)
{
	YbAshInstallHooks();
	query_id_stack.top_index = -1;
	query_id_stack.num_query_ids_not_pushed = 0;
}

void
YbAshInstallHooks(void)
{
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = yb_ash_ExecutorStart;

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = yb_ash_ExecutorRun;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = yb_ash_ExecutorEnd;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = yb_ash_ExecutorFinish;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = yb_ash_ProcessUtility;
}

void
YbAshSetSessionId(uint64 session_id)
{
	LWLockAcquire(&MyProc->yb_ash_metadata_lock, LW_EXCLUSIVE);
	MyProc->yb_ash_metadata.session_id = session_id;
	LWLockRelease(&MyProc->yb_ash_metadata_lock);
}

void
YbAshSetDatabaseId(Oid database_id)
{
	LWLockAcquire(&MyProc->yb_ash_metadata_lock, LW_EXCLUSIVE);
	MyProc->yb_ash_metadata.database_id = database_id;
	LWLockRelease(&MyProc->yb_ash_metadata_lock);
}

static int
yb_ash_cb_max_entries(void)
{
	return yb_ash_circular_buffer_size * 1024 / sizeof(YBCAshSample);
}

/*
 * Push a query id to the stack. In case the stack is full, we increment
 * a counter to maintain the number of query ids which were supposed to be
 * pushed but couldn't be pushed. So that later, when we are supposed to pop
 * from the stack, we know how many no-op pop operations we have to perform.
 */
static bool
YbAshNestedQueryIdStackPush(uint64 query_id)
{
	if (query_id_stack.top_index < MAX_NESTED_QUERY_LEVEL)
	{
		query_id_stack.query_ids[++query_id_stack.top_index] = query_id;
		return true;
	}

	ereport(LOG,
			(errmsg("ASH stack for nested query ids is full")));
	++query_id_stack.num_query_ids_not_pushed;
	return false;
}

/*
 * Pop a query id from the stack
 */
static uint64
YbAshNestedQueryIdStackPop(void)
{
	if (query_id_stack.num_query_ids_not_pushed > 0)
	{
		--query_id_stack.num_query_ids_not_pushed;
		return 0;
	}

	Assert(query_id_stack.top_index >= 0);
	return query_id_stack.query_ids[query_id_stack.top_index--];
}

/*
 * YbAshShmemSize
 *		Compute space needed for ASH-related shared memory
 */
Size
YbAshShmemSize(void)
{
	Size		size;

	size = offsetof(YbAsh, circular_buffer);
	size = add_size(size, mul_size(yb_ash_cb_max_entries(),
								   sizeof(YBCAshSample)));

	return size;
}

/*
 * YbAshShmemInit
 *		Allocate and initialize ASH-related shared memory
 */
void
YbAshShmemInit(void)
{
	bool		found = false;

	yb_ash = ShmemInitStruct("yb_ash_circular_buffer",
							 YbAshShmemSize(),
							 &found);

	LWLockRegisterTranche(LWTRANCHE_YB_ASH_CIRCULAR_BUFFER, "yb_ash_circular_buffer");

	if (!found)
	{
		LWLockInitialize(&yb_ash->lock, LWTRANCHE_YB_ASH_CIRCULAR_BUFFER);
		yb_ash->index = 0;
		yb_ash->max_entries = yb_ash_cb_max_entries();
		MemSet(yb_ash->circular_buffer, 0, yb_ash->max_entries * sizeof(YBCAshSample));
	}
}

static void
yb_ash_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (yb_enable_ash)
	{
		/* Query id can be zero here only if pg_stat_statements is disabled */
		uint64 query_id = queryDesc->plannedstmt->queryId != 0
						  ? queryDesc->plannedstmt->queryId
						  : yb_ash_utility_query_id(queryDesc->sourceText,
					   								queryDesc->plannedstmt->stmt_len,
													queryDesc->plannedstmt->stmt_location);
		YbAshSetQueryId(query_id);
	}

	PG_TRY();
	{
		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
	}
	PG_CATCH();
	{
		if (yb_enable_ash)
			YbAshResetQueryId();

		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
yb_ash_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				   bool execute_once)
{
	++nested_level;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		--nested_level;
	}
	PG_CATCH();
	{
		--nested_level;

		if (yb_enable_ash)
			YbAshResetQueryId();

		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
yb_ash_ExecutorFinish(QueryDesc *queryDesc)
{
	++nested_level;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		--nested_level;
	}
	PG_CATCH();
	{
		--nested_level;

		if (yb_enable_ash)
			YbAshResetQueryId();

		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
yb_ash_ExecutorEnd(QueryDesc *queryDesc)
{
	PG_TRY();
	{
		if (prev_ExecutorEnd)
			prev_ExecutorEnd(queryDesc);
		else
			standard_ExecutorEnd(queryDesc);

		if (yb_enable_ash)
			YbAshResetQueryId();
	}
	PG_CATCH();
	{
		if (yb_enable_ash)
			YbAshResetQueryId();

		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
yb_ash_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  ProcessUtilityContext context, ParamListInfo params,
					  QueryEnvironment *queryEnv, DestReceiver *dest,
					  char *completionTag)
{
	if (yb_enable_ash)
	{
		uint64 query_id = pstmt->queryId != 0
						  ? pstmt->queryId
						  : yb_ash_utility_query_id(queryString,
					   								pstmt->stmt_len,
													pstmt->stmt_location);
		YbAshSetQueryId(query_id);
	}

	++nested_level;
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString,
								context, params, queryEnv,
								dest, completionTag);
		else
			standard_ProcessUtility(pstmt, queryString,
									context, params, queryEnv,
									dest, completionTag);
		--nested_level;

		if (yb_enable_ash)
			YbAshResetQueryId();
	}
	PG_CATCH();
	{
		--nested_level;

		if (yb_enable_ash)
			YbAshResetQueryId();

		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
YbAshSetQueryId(uint64 query_id)
{
	if (set_query_id())
	{
		if (YbAshNestedQueryIdStackPush(MyProc->yb_ash_metadata.query_id))
		{
			LWLockAcquire(&MyProc->yb_ash_metadata_lock, LW_EXCLUSIVE);
			MyProc->yb_ash_metadata.query_id = query_id;
			LWLockRelease(&MyProc->yb_ash_metadata_lock);
		}
	}
}

static void
YbAshResetQueryId(void)
{
	if (set_query_id())
	{
		uint64 prev_query_id = YbAshNestedQueryIdStackPop();
		if (prev_query_id != 0)
		{
			LWLockAcquire(&MyProc->yb_ash_metadata_lock, LW_EXCLUSIVE);
			MyProc->yb_ash_metadata.query_id = prev_query_id;
			LWLockRelease(&MyProc->yb_ash_metadata_lock);
		}
	}
}

void
YbAshSetMetadata(void)
{
	/* The stack must be empty at the start of a request */
	Assert(query_id_stack.top_index == -1);

	LWLockAcquire(&MyProc->yb_ash_metadata_lock, LW_EXCLUSIVE);
	YBCGenerateAshRootRequestId(MyProc->yb_ash_metadata.root_request_id);
	LWLockRelease(&MyProc->yb_ash_metadata_lock);
}

void
YbAshUnsetMetadata(void)
{
	/*
	 * Some queryids may not be popped from the stack if YbAshResetQueryId
	 * returns an error. Reset the stack here. We can remove this if we
	 * make query_id atomic
	 */
	query_id_stack.top_index = -1;
	query_id_stack.num_query_ids_not_pushed = 0;

	LWLockAcquire(&MyProc->yb_ash_metadata_lock, LW_EXCLUSIVE);
	MemSet(MyProc->yb_ash_metadata.root_request_id, 0,
		sizeof(MyProc->yb_ash_metadata.root_request_id));
	LWLockRelease(&MyProc->yb_ash_metadata_lock);
}

/*
 * Calculate the query id for utility statements. This takes parts of pgss_store
 * from pg_stat_statements.
 */
static uint64
yb_ash_utility_query_id(const char *query, int query_len, int query_location)
{
	const char *redacted_query;
	int			redacted_query_len;

	Assert(query != NULL);

	if (query_location >= 0)
	{
		Assert(query_location <= strlen(query));
		query += query_location;
		/* Length of 0 (or -1) means "rest of string" */
		if (query_len <= 0)
			query_len = strlen(query);
		else
			Assert(query_len <= strlen(query));
	}
	else
	{
		/* If query location is unknown, distrust query_len as well */
		query_location = 0;
		query_len = strlen(query);
	}

	/*
	 * Discard leading and trailing whitespace, too.  Use scanner_isspace()
	 * not libc's isspace(), because we want to match the lexer's behavior.
	 */
	while (query_len > 0 && scanner_isspace(query[0]))
		query++, query_location++, query_len--;
	while (query_len > 0 && scanner_isspace(query[query_len - 1]))
		query_len--;

	/* Use the redacted query for checking purposes. */
	YbGetRedactedQueryString(query, query_len, &redacted_query, &redacted_query_len);

	return DatumGetUInt64(hash_any_extended((const unsigned char *) redacted_query,
											redacted_query_len, 0));
}

/*
 * Events such as ClientRead can take up a lot of space in the circular buffer
 * if there is an idle session. We don't want to include such wait events.
 * This list may increase in the future.
 */
bool
YbAshShouldIgnoreWaitEvent(uint32 wait_event_info)
{
	switch (wait_event_info)
	{
		case WAIT_EVENT_CLIENT_READ:
			return true;
		default:
			return false;
	}
	return false;
}

static void
YbAshAcquireBufferLock(bool exclusive)
{
	LWLockAcquire(&yb_ash->lock, exclusive ? LW_EXCLUSIVE : LW_SHARED);
}

static void
YbAshReleaseBufferLock()
{
	LWLockRelease(&yb_ash->lock);
}

static void
yb_ash_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

static void
yb_ash_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

void
YbAshMain(Datum main_arg)
{
	ereport(LOG,
			(errmsg("starting bgworker yb_ash collector with max buffer entries %d",
					yb_ash->max_entries)));

	/* Register functions for SIGTERM/SIGHUP management */
	pqsignal(SIGHUP, yb_ash_sighup);
	pqsignal(SIGTERM, yb_ash_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	pgstat_report_appname("yb_ash collector");

	while (!got_sigterm)
	{
		TimestampTz	sample_time;
		int 		rc;
		/* Wait necessary amount of time */
		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   yb_ash_sampling_interval_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);

		/* Bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Process signals */
		if (got_sighup)
		{
			/* Process config file */
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
			ereport(LOG,
					(errmsg("bgworker yb_ash signal: processed SIGHUP")));
		}

		if (yb_enable_ash && yb_ash_sample_size > 0)
		{
			sample_time = GetCurrentTimestamp();

			/*
			 * The circular buffer lock is acquired in exclusive mode inside
			 * YBCStoreTServerAshSamples after getting the ASH samples from
			 * tserver and just before copying it into the buffer. The lock
			 * is released after we copy the PG samples.
			 */
			YBCStoreTServerAshSamples(&YbAshAcquireBufferLock,
									  &YbAshGetNextCircularBufferSlot,
									  sample_time);
			YbStorePgAshSamples(sample_time);
			YbAshReleaseBufferLock();
		}
	}
	proc_exit(0);
}

static const unsigned char *
get_yql_endpoint_tserver_uuid()
{
	static const unsigned char *local_tserver_uuid = NULL;
	if (!local_tserver_uuid && IsYugaByteEnabled())
		local_tserver_uuid = YBCGetLocalTserverUuid();
	return local_tserver_uuid;
}

/*
 * Increments the index to insert in the circular buffer.
 */
static void
YbAshIncrementCircularBufferIndex(void)
{
	if (++yb_ash->index == yb_ash->max_entries)
		yb_ash->index = 0;
}

/*
 * Returns true if another sample should be stored in the circular buffer.
 */
bool
YbAshStoreSample(PGPROC *proc, int num_procs, TimestampTz sample_time,
				 int *samples_stored)
{
	/*
	 * If there are less samples available than the sample size, the sample
	 * weight must be 1.
	 */
	float8 sample_weight = Max(num_procs, yb_ash_sample_size) * 1.0 / yb_ash_sample_size;

	copy_pgproc_sample_fields(proc);
	copy_non_pgproc_sample_fields(sample_weight, sample_time);

	YbAshIncrementCircularBufferIndex();

	if (++(*samples_stored) == yb_ash_sample_size)
		return false;

	return true;
}

static void
copy_pgproc_sample_fields(PGPROC *proc)
{
	YBCAshSample *cb_sample = &yb_ash->circular_buffer[yb_ash->index];

	LWLockAcquire(&proc->yb_ash_metadata_lock, LW_SHARED);
	memcpy(&cb_sample->metadata, &proc->yb_ash_metadata, sizeof(YBCAshMetadata));
	LWLockRelease(&proc->yb_ash_metadata_lock);

	cb_sample->encoded_wait_event_code = proc->wait_event_info;
}

static void
copy_non_pgproc_sample_fields(float8 sample_weight, TimestampTz sample_time)
{
	YBCAshSample *cb_sample = &yb_ash->circular_buffer[yb_ash->index];

	/* yql_endpoint_tserver_uuid is constant for all PG samples */
	if (get_yql_endpoint_tserver_uuid())
		memcpy(cb_sample->yql_endpoint_tserver_uuid,
			   get_yql_endpoint_tserver_uuid(),
			   sizeof(cb_sample->yql_endpoint_tserver_uuid));

	/* rpc_request_id is 0 for PG samples */
	cb_sample->rpc_request_id = 0;
	/* TODO(asaha): Add aux info to circular buffer once it's available */
	cb_sample->aux_info[0] = '\0';
	cb_sample->sample_weight = sample_weight;
	cb_sample->sample_time = sample_time;
}

/*
 * Returns a pointer to the circular buffer slot where the sample should be
 * inserted and increments the index.
 */
static YBCAshSample *
YbAshGetNextCircularBufferSlot(void)
{
	YBCAshSample *slot = &yb_ash->circular_buffer[yb_ash->index];
	YbAshIncrementCircularBufferIndex();
	return slot;
}

Datum
yb_active_session_history(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int			i;
	static int  ncols = 0;

	if (ncols < ACTIVE_SESSION_HISTORY_COLS_V3)
		ncols = YbGetNumberOfFunctionOutputColumns(F_YB_ACTIVE_SESSION_HISTORY);

	/* ASH must be loaded first */
	if (!yb_ash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("ysql_yb_ash_enable_infra gflag must be enabled")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errmsg_internal("return type must be a row type")));

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	YbAshAcquireBufferLock(false /* exclusive */);

	for (i = 0; i < yb_ash->max_entries; ++i)
	{
		Datum		values[ncols];
		bool		nulls[ncols];
		int			j = 0;
		pg_uuid_t	root_request_id;
		pg_uuid_t	yql_endpoint_tserver_uuid;
		/* 22 bytes required for ipv4 and 48 for ipv6 (including null character) */
		char		client_node_ip[48];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		YBCAshSample *sample = &yb_ash->circular_buffer[i];
		YBCAshMetadata *metadata = &sample->metadata;

		if (sample->sample_time != 0)
			values[j++] = TimestampTzGetDatum(sample->sample_time);
		else
			break; /* The circular buffer is not fully filled yet */

		uchar_to_uuid(metadata->root_request_id, &root_request_id);
		values[j++] = UUIDPGetDatum(&root_request_id);

		if (sample->rpc_request_id != 0)
			values[j++] = Int64GetDatum(sample->rpc_request_id);
		else
			nulls[j++] = true;

		values[j++] = CStringGetTextDatum(
			YBCGetWaitEventComponent(sample->encoded_wait_event_code));
		values[j++] = CStringGetTextDatum(
			YBCGetWaitEventClass(sample->encoded_wait_event_code));
		values[j++] = CStringGetTextDatum(
			pgstat_get_wait_event(sample->encoded_wait_event_code));

		uchar_to_uuid(sample->yql_endpoint_tserver_uuid, &yql_endpoint_tserver_uuid);
		values[j++] = UUIDPGetDatum(&yql_endpoint_tserver_uuid);

		values[j++] = UInt64GetDatum(metadata->query_id);
		values[j++] = UInt64GetDatum(metadata->session_id);

		if (metadata->addr_family == AF_INET || metadata->addr_family == AF_INET6)
		{
			client_ip_to_string(metadata->client_addr, metadata->client_port, metadata->addr_family,
								client_node_ip);
			values[j++] = CStringGetTextDatum(client_node_ip);
		}
		else
		{
			/*
			 * internal operations such as flushes and compactions are not tied to any client
			 * and they might have the addr_family as AF_UNSPEC
			 */
			Assert(metadata->addr_family == AF_UNIX || metadata->addr_family == AF_UNSPEC);
			nulls[j++] = true;
		}

		if (sample->aux_info[0] != '\0')
			values[j++] = CStringGetTextDatum(sample->aux_info);
		else
			nulls[j++] = true;

		values[j++] = Float4GetDatum(sample->sample_weight);

		if (ncols >= ACTIVE_SESSION_HISTORY_COLS_V2)
			values[j++] = CStringGetTextDatum(
				pgstat_get_wait_event_type(sample->encoded_wait_event_code));

		if (ncols >= ACTIVE_SESSION_HISTORY_COLS_V3)
			values[j++] = ObjectIdGetDatum(metadata->database_id);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	YbAshReleaseBufferLock();

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

static void
uchar_to_uuid(unsigned char *in, pg_uuid_t *out)
{
	memcpy(out->data, in, UUID_LEN);
}

static void
client_ip_to_string(unsigned char *client_addr, uint16 client_port,
					uint8_t addr_family, char *client_ip)
{
	if (addr_family == AF_INET)
	{
		sprintf(client_ip, "%d.%d.%d.%d:%d",
				client_addr[0], client_addr[1], client_addr[2], client_addr[3],
				client_port);
	}
	else
	{
		sprintf(client_ip,
				"[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%d",
				client_addr[0], client_addr[1], client_addr[2], client_addr[3],
				client_addr[4], client_addr[5], client_addr[6], client_addr[7],
				client_addr[8], client_addr[9], client_addr[10], client_addr[11],
				client_addr[12], client_addr[13], client_addr[14], client_addr[15],
				client_port);
	}
}