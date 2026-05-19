# Copyright (c) YugabyteDB, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.

from __future__ import annotations

import logging
import re
import sys
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

from decolocate_tables.copy_data import (
    CopyDataError,
    data_copy_method,
    get_row_count,
    migrate_table_data,
)
from decolocate_tables.dump import _split_sql_statements
from decolocate_tables.models import MigrationPlan, TableInfo
from decolocate_tables.progress import (
    CreateIndexProgressBar,
    PhaseReporter,
    cursor_backend_pid,
    extract_index_display_name,
    format_table_progress,
    is_create_index_statement,
)
from decolocate_tables.statistics import StatisticsError, run_analyze, table_has_statistics

logger = logging.getLogger(__name__)

# ── Migration state constants ──────────────────────────────────────────────────
#
# Each target table moves through these states as the migration progresses:
#
#   ready       – original colocated table exists; no backup present
#   phase1_done – phase 1 transaction committed: source renamed to backup,
#                 empty uncollocated shell created; backup still present
#   complete    – phase 3 committed: backup dropped, constraints/views recreated
#
# The values are surfaced in log messages to aid diagnosis.
_STATE_READY = "ready"
_STATE_PHASE1_DONE = "phase1_done"
_STATE_COMPLETE = "complete"

# PostgreSQL/YSQL SQLSTATE codes for "object already exists" conditions:
#   42P07 – duplicate_table  (CREATE INDEX, CREATE TABLE, etc.)
#   42710 – duplicate_object (constraints, triggers, policies, rules, etc.)
_DUPLICATE_SQLSTATES = frozenset({"42710", "42P07"})

_DETECT_STATE_SQL = """
SELECT c.relname,
       (SELECT is_colocated FROM yb_table_properties(c.oid)) AS is_colocated
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = %(schema)s
  AND c.relname = ANY(%(names)s)
  AND c.relkind = 'r';
"""


class ExecutorError(Exception):
    pass


# ── Helpers ────────────────────────────────────────────────────────────────────

def _pgcode(exc) -> Optional[str]:
    """Extract SQLSTATE portably from both psycopg2 and psycopg3 exceptions."""
    return (
        getattr(exc, "pgcode", None)
        or getattr(exc, "sqlstate", None)
        or getattr(getattr(exc, "diag", None), "sqlstate", None)
    )


def _read_sql(path: Optional[str]) -> str:
    if not path:
        return ""
    return Path(path).read_text(encoding="utf-8").strip()


def _execute_sql(cur, sql: str, label: str) -> None:
    sql = sql.strip()
    if not sql:
        return
    logger.info("Executing: %s", label)
    cur.execute(sql)


def _execute_one_idempotent(cur, stmt: str, label: str) -> None:
    """Run a single DDL statement in a savepoint (idempotent on duplicates)."""
    cur.execute("SAVEPOINT _decolocate_idem")
    try:
        cur.execute(stmt)
        cur.execute("RELEASE SAVEPOINT _decolocate_idem")
    except Exception as exc:
        code = _pgcode(exc)
        cur.execute("ROLLBACK TO SAVEPOINT _decolocate_idem")
        cur.execute("RELEASE SAVEPOINT _decolocate_idem")
        if code in _DUPLICATE_SQLSTATES:
            logger.debug(
                "Skipping already-existing object in %s: %.80s", label, stmt
            )
        else:
            raise


def _execute_sql_idempotent(cur, sql: str, label: str) -> None:
    """
    Execute each statement in *sql* inside a savepoint.  Statements that fail
    with a duplicate-object SQLSTATE (42710, 42P07) are silently skipped so the
    method is safe to call a second time when some objects already exist.
    """
    for stmt in _split_sql_statements(sql):
        stmt = stmt.strip()
        if not stmt or stmt == ";":
            continue
        _execute_one_idempotent(cur, stmt, label)


def _execute_post_create_sql(
    cur,
    connect_fn: Callable[[], object],
    sql: str,
    label: str,
    table_label: str,
    estimated_rows: int,
    show_index_progress: bool,
) -> None:
    """
    Execute post-create DDL, showing a pg_stat_progress_create_index bar for
    each CREATE INDEX statement when progress reporting is enabled.
    """
    statements: List[str] = []
    for stmt in _split_sql_statements(sql):
        stmt = stmt.strip()
        if stmt and stmt != ";":
            statements.append(stmt)

    index_statements = [s for s in statements if is_create_index_statement(s)]
    index_total = len(index_statements)
    index_num = 0

    if show_index_progress and index_total > 0:
        print(
            f"CREATE INDEX progress ({table_label}): {index_total} index(es)",
            file=sys.stderr,
            flush=True,
        )

    for stmt in statements:
        if is_create_index_statement(stmt) and show_index_progress:
            index_num += 1
            index_name = extract_index_display_name(stmt)
            pid = cursor_backend_pid(cur)
            cur.execute("SAVEPOINT _decolocate_idem")
            try:
                with CreateIndexProgressBar(
                    connect_fn,
                    pid,
                    index_name,
                    index_num,
                    index_total,
                    estimated_rows,
                    enabled=True,
                ):
                    cur.execute(stmt)
                cur.execute("RELEASE SAVEPOINT _decolocate_idem")
            except Exception as exc:
                code = _pgcode(exc)
                cur.execute("ROLLBACK TO SAVEPOINT _decolocate_idem")
                cur.execute("RELEASE SAVEPOINT _decolocate_idem")
                if code in _DUPLICATE_SQLSTATES:
                    logger.debug(
                        "Skipping already-existing index in %s: %.80s",
                        label,
                        stmt,
                    )
                else:
                    raise
        else:
            _execute_one_idempotent(cur, stmt, label)


def _run_in_transaction(
    conn,
    lock_timeout: Optional[str],
    statement_timeout: Optional[str],
    body,
) -> None:
    """
    Run *body(cur)* inside an explicit BEGIN/COMMIT.  Uses autocommit=True so
    the explicit BEGIN is unambiguous for both psycopg2 and psycopg3.
    Timeouts are applied with SET LOCAL so they only affect this transaction.
    """
    prev_autocommit = conn.autocommit
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            cur.execute("BEGIN")
            try:
                if lock_timeout:
                    cur.execute(f"SET LOCAL lock_timeout = '{lock_timeout}'")
                if statement_timeout:
                    cur.execute(f"SET LOCAL statement_timeout = '{statement_timeout}'")
                body(cur)
                cur.execute("COMMIT")
            except Exception:
                try:
                    cur.execute("ROLLBACK")
                except Exception:
                    pass
                raise
    finally:
        conn.autocommit = prev_autocommit


# ── State detection ────────────────────────────────────────────────────────────

def _detect_table_state(
    cur, schema: str, name: str, backup_suffix: str
) -> str:
    """
    Determine the migration state of a single table by inspecting pg_class.

    Returns one of _STATE_READY, _STATE_PHASE1_DONE, or _STATE_COMPLETE.
    Raises ExecutorError for unrecognised / corrupt states.
    """
    backup = f"{name}{backup_suffix}"
    cur.execute(
        _DETECT_STATE_SQL,
        {"schema": schema, "names": [name, backup]},
    )
    rows: Dict[str, bool] = {r[0]: r[1] for r in cur.fetchall()}

    has_target = name in rows
    has_backup = backup in rows

    if has_target and not has_backup:
        if rows[name] is True:
            return _STATE_READY
        # Table exists and is already uncollocated with no backup → done.
        return _STATE_COMPLETE

    if has_backup and has_target:
        return _STATE_PHASE1_DONE

    if has_backup and not has_target:
        raise ExecutorError(
            f"Corrupt state for {schema}.{name}: backup '{backup}' exists but "
            f"the target table does not. Manual intervention required."
        )

    raise ExecutorError(
        f"Table {schema}.{name} does not exist and no backup found. "
        "The migration may already be complete, or the table was dropped externally."
    )


def _batch_state(states: Dict[str, str]) -> str:
    """
    Return the effective batch-level state from per-table states, validating
    that they are internally consistent.

    Tables are migrated one at a time (each runs phases 1-4 before the next).
    A mixed 'ready' plus in-progress state is invalid. A mix of 'phase1_done'
    and 'complete' is allowed when resuming a partially finished run.
    """
    distinct = set(states.values())
    if len(distinct) == 1:
        return next(iter(distinct))

    if _STATE_READY in distinct and len(distinct) > 1:
        raise ExecutorError(
            "Inconsistent table states detected — some tables are in 'ready' "
            "state while others have already been partially migrated:\n"
            + "\n".join(f"  {name}: {state}" for name, state in sorted(states.items()))
            + "\nManual inspection is required before proceeding."
        )

    # Only phase1_done / complete mix: treat as phase1_done so we re-copy
    # tables that haven't been finalized yet.
    return _STATE_PHASE1_DONE


# ── Statistics helpers (restart-aware) ────────────────────────────────────────

def _record_source_statistics(
    cur,
    plan: MigrationPlan,
    states: Dict[str, str],
) -> None:
    """
    Check pg_statistic for each table and decide whether ANALYZE should run
    after migration.  Uses the backup table name when the original has already
    been renamed (phase1_done state), so the decision survives a restart.
    """
    for table in plan.tables:
        qn = table.qualified
        state = states.get(qn.name, _STATE_READY)

        if not plan.analyze_if_had_stats or state == _STATE_COMPLETE:
            table.had_statistics = None
            table.will_analyze = False
            continue

        # After phase 1 the original name refers to the empty new shell; read
        # statistics from the backup (which preserves the original data).
        check_name = table.backup_name if state == _STATE_PHASE1_DONE else qn.name
        table.had_statistics = table_has_statistics(cur, qn.schema, check_name)
        table.will_analyze = bool(table.had_statistics)


# ── Phase helpers ──────────────────────────────────────────────────────────────

def _create_empty_replacement(cur, table: TableInfo, backup_suffix: str) -> None:
    """Rename source to backup and create an uncollocated shell using captured DDL."""
    qn = table.qualified
    backup_name = f"{qn.name}{backup_suffix}"
    table.backup_name = backup_name

    cur.execute(
        f'ALTER TABLE "{qn.schema}"."{qn.name}" RENAME TO "{backup_name}"'
    )
    logger.info("Renamed %s -> %s", qn, backup_name)

    create_sql = _read_sql(table.create_table_sql_path)
    _execute_sql(cur, create_sql, f"create uncollocated table {qn}")


def _truncate_table(
    conn,
    table: TableInfo,
    lock_timeout: Optional[str],
    statement_timeout: Optional[str],
) -> None:
    """Clear one target table before re-running COPY after a restart."""

    def body(cur) -> None:
        reg = table.qualified.regclass()
        cur.execute(f"TRUNCATE {reg}")
        logger.info(
            "Truncated %s to remove partial COPY data from prior run",
            table.qualified,
        )

    _run_in_transaction(conn, lock_timeout, statement_timeout, body)


def _drop_views_if_needed(
    cur,
    plan: MigrationPlan,
    dropped_views: set[str],
    phases: Optional[PhaseReporter],
) -> None:
    for view in plan.views_drop_order:
        key = str(view.qualified)
        if key in dropped_views:
            continue
        qn = view.qualified
        cur.execute(f'DROP VIEW IF EXISTS "{qn.schema}"."{qn.name}"')
        dropped_views.add(key)
        logger.info("Dropped view %s", qn)
        if phases is not None:
            phases.step(f"Dropped view {qn}")


def _migrate_one_table_data(
    conn,
    table: TableInfo,
    connect_fn: Callable[[], object],
    copy_threads: int,
    show_progress: bool = False,
    phases: Optional[PhaseReporter] = None,
    table_num: int = 1,
    table_total: int = 1,
) -> None:
    if not table.backup_name:
        raise ExecutorError(f"Missing backup name for {table.qualified}")
    qn = table.qualified
    with conn.cursor() as cur:
        row_count = get_row_count(cur, qn.schema, table.backup_name)
        table.row_count = row_count
        if phases is not None:
            phases.step(
                format_table_progress(
                    table_num,
                    table_total,
                    f"{qn}: {row_count:,} rows in backup -> "
                    f"{data_copy_method(row_count)}",
                )
            )
        method = migrate_table_data(
            connect_fn,
            cur,
            qn.schema,
            table.backup_name,
            qn.schema,
            qn.name,
            copy_threads,
            row_count,
            show_progress=show_progress,
        )
        table.data_copy_method = method
        logger.info(
            "Data migration for %s: %d rows via %s",
            qn,
            row_count,
            method,
        )


def _finalize_table(
    cur,
    table: TableInfo,
    connect_fn: Callable[[], object],
    show_index_progress: bool = False,
) -> None:
    """Apply post-create DDL, verify row counts and colocation, drop backup."""
    qn = table.qualified
    backup_name = table.backup_name
    if not backup_name:
        raise ExecutorError(f"Missing backup name for {table.qualified}")

    post_sql = _read_sql(table.post_create_sql_path)
    estimated_rows = int(table.row_count or 0)
    _execute_post_create_sql(
        cur,
        connect_fn,
        post_sql,
        f"post-create DDL for {qn}",
        str(qn),
        estimated_rows,
        show_index_progress,
    )

    _verify_row_counts(cur, table, backup_name)
    _verify_uncollocated(cur, table)

    # IF EXISTS guards against the rare case where backup was already dropped
    # (e.g. a prior Phase 3 that committed just the DROP before failing).
    backup_reg = f'"{qn.schema}"."{backup_name}"'
    cur.execute(f"DROP TABLE IF EXISTS {backup_reg}")
    logger.info("Dropped backup table %s.%s", qn.schema, backup_name)


def _recreate_views(cur, plan: MigrationPlan) -> None:
    """
    Recreate dependent views in topological order.  Uses CREATE OR REPLACE VIEW
    so the step is safe to re-run if it previously completed only partially.
    """
    for view in plan.views_create_order:
        ddl = _read_sql(view.ddl_file)
        # Make idempotent: CREATE OR REPLACE VIEW works even if the view exists.
        ddl = re.sub(
            r"\bCREATE\s+VIEW\b",
            "CREATE OR REPLACE VIEW",
            ddl,
            flags=re.IGNORECASE,
        )
        _execute_sql(cur, ddl, f"recreate view {view.qualified}")


def _run_post_migrate_analyze(
    conn,
    plan: MigrationPlan,
    phases: Optional[PhaseReporter] = None,
) -> None:
    if not plan.analyze_if_had_stats:
        return
    to_analyze = [t for t in plan.tables if t.will_analyze]
    total = len(to_analyze)
    for table_num, table in enumerate(to_analyze, 1):
        qn = table.qualified
        if phases is not None:
            phases.step(
                format_table_progress(table_num, total, f"ANALYZE {qn}")
            )
        run_analyze(conn, qn.schema, qn.name)


# ── Main entry point ───────────────────────────────────────────────────────────

def execute_plan(
    conn,
    plan: MigrationPlan,
    backup_suffix: str,
    connect_fn: Callable[[], object],
    copy_threads: int = 4,
    lock_timeout: Optional[str] = None,
    statement_timeout: Optional[str] = None,
    show_phase_progress: bool = True,
    show_copy_progress: bool = False,
) -> None:
    if plan.dry_run:
        logger.info("Dry run: skipping execution")
        return

    start = time.monotonic()
    phases = PhaseReporter(enabled=show_phase_progress)

    # ── Step 0: Detect current state and set backup names ──────────────────────
    phases.start(0, "Pre-flight", "Detect migration state and column statistics")
    with conn.cursor() as cur:
        states: Dict[str, str] = {}
        for table in plan.tables:
            qn = table.qualified
            # Set backup_name unconditionally so later phases can use it even
            # when the rename already happened on a prior run.
            table.backup_name = f"{qn.name}{backup_suffix}"
            state = _detect_table_state(cur, qn.schema, qn.name, backup_suffix)
            states[qn.name] = state
            logger.info("Table %s: migration state = %s", qn, state)

        effective_state = _batch_state(states)
        logger.info("Effective batch state: %s", effective_state)

        # Read statistics from the correct table for each state.
        _record_source_statistics(cur, plan, states)

    phases.complete(f"batch state is {effective_state}")

    # All tables already complete: optional view repair pass only.
    if effective_state == _STATE_COMPLETE:
        if plan.views_create_order:
            phases.start(3, "Finalize", "Recreate views only")
            def _views_only(cur) -> None:
                for view in plan.views_create_order:
                    phases.step(f"Recreate view {view.qualified}")
                _recreate_views(cur, plan)
            _run_in_transaction(conn, lock_timeout, statement_timeout, _views_only)
            phases.complete(f"{len(plan.views_create_order)} view(s)")
        if plan.analyze_if_had_stats and any(t.will_analyze for t in plan.tables):
            phases.start(4, "Post-migrate ANALYZE", "Refresh planner statistics")
            try:
                _run_post_migrate_analyze(conn, plan, phases=phases)
            except StatisticsError as exc:
                raise ExecutorError(str(exc)) from exc
            phases.complete("statistics updated")
        logger.info("Migration already complete; nothing further to do.")
        return

    table_total = len(plan.tables)
    dropped_views: set[str] = set()

    for table_num, table in enumerate(plan.tables, 1):
        qn = table.qualified
        state_at_start = states[qn.name]

        if state_at_start == _STATE_COMPLETE:
            logger.info("Skipping %s: already migrated", qn)
            if show_phase_progress:
                phases.table_header(table_num, table_total, str(qn))
                phases.step("already complete; skipping")
            continue

        phases.table_header(table_num, table_total, str(qn))

        # ── Phase 1: DDL for this table ──────────────────────────────────────
        if state_at_start == _STATE_READY:
            phases.start(
                1,
                "DDL preparation",
                "Drop dependent views (first table only), recreate uncollocated shell",
            )

            def _phase1(cur) -> None:
                _drop_views_if_needed(cur, plan, dropped_views, phases)
                phases.step("recreate as uncollocated")
                _create_empty_replacement(cur, table, backup_suffix)

            _run_in_transaction(conn, lock_timeout, statement_timeout, _phase1)
            phases.complete("uncollocated shell created")
            states[qn.name] = _STATE_PHASE1_DONE

        # ── Phase 2: Data migration for this table ───────────────────────────
        if states[qn.name] == _STATE_PHASE1_DONE:
            phases.start(
                2,
                "Data migration",
                "COPY workers spread across yb_servers() nodes",
            )
            if state_at_start == _STATE_PHASE1_DONE:
                phases.step("truncate target (clear partial COPY from prior run)")
                _truncate_table(conn, table, lock_timeout, statement_timeout)
            try:
                _migrate_one_table_data(
                    conn,
                    table,
                    connect_fn,
                    copy_threads,
                    show_progress=show_copy_progress,
                    phases=phases,
                    table_num=table_num,
                    table_total=table_total,
                )
            except CopyDataError as exc:
                raise ExecutorError(str(exc)) from exc
            phases.complete("data copied")

            # ── Phase 3: Finalize this table ─────────────────────────────────
            phases.start(
                3,
                "Finalize",
                "Indexes, constraints, triggers, verify, drop backup",
            )

            def _phase3(cur) -> None:
                phases.step("apply post-create DDL and verify")
                _finalize_table(
                    cur,
                    table,
                    connect_fn,
                    show_index_progress=show_copy_progress,
                )

            _run_in_transaction(conn, lock_timeout, statement_timeout, _phase3)
            phases.complete("table finalized")
            states[qn.name] = _STATE_COMPLETE

            # ── Phase 4: ANALYZE this table ──────────────────────────────────
            if table.will_analyze:
                phases.start(4, "Post-migrate ANALYZE", "Refresh planner statistics")
                try:
                    phases.step(f"ANALYZE {qn}")
                    run_analyze(conn, qn.schema, qn.name)
                except StatisticsError as exc:
                    raise ExecutorError(str(exc)) from exc
                phases.complete("statistics updated")

    # ── Recreate dependent views after all tables are migrated ───────────────
    if plan.views_create_order:
        phases.start(3, "Dependent views", "Recreate views in dependency order")
        def _recreate_all_views(cur) -> None:
            for view in plan.views_create_order:
                phases.step(f"Recreate view {view.qualified}")
            _recreate_views(cur, plan)
        _run_in_transaction(conn, lock_timeout, statement_timeout, _recreate_all_views)
        phases.complete(f"{len(plan.views_create_order)} view(s)")

    elapsed = time.monotonic() - start
    if show_phase_progress:
        print(
            f"\nMigration finished in {elapsed:.1f}s: "
            f"{len(plan.tables)} table(s), {len(plan.views_create_order)} view(s)",
            flush=True,
        )
    logger.info(
        "Migration completed in %.1fs: %d table(s), %d view(s), copy_threads=%d",
        elapsed,
        len(plan.tables),
        len(plan.views_create_order),
        copy_threads,
    )
