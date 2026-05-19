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
import os
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Callable, List, Optional, Sequence, Tuple

# connect_fn(bucket, role='src'|'dst'|'default') or connect_fn() for default host
ConnectFn = Callable[..., object]

from decolocate_tables.progress import CopyProgressMonitor, backend_pid

logger = logging.getLogger(__name__)

# Tables with fewer rows use INSERT ... SELECT; larger tables use parallel COPY.
COPY_ROW_THRESHOLD = 100_000

TABLE_COLUMNS_SQL = """
SELECT a.attname
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
JOIN pg_attribute a ON a.attrelid = c.oid
WHERE n.nspname = %(schema)s
  AND c.relname = %(table)s
  AND a.attnum > 0
  AND NOT a.attisdropped
  AND COALESCE(a.attgenerated, '') = ''
ORDER BY a.attnum;
"""

HASH_KEY_COLUMNS_SQL = """
SELECT a.attname
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
JOIN pg_index idx ON idx.indrelid = c.oid AND idx.indisprimary
CROSS JOIN LATERAL (
    SELECT num_hash_key_columns::int AS nhash
    FROM yb_table_properties(c.oid)
) ytp
JOIN LATERAL unnest(idx.indkey) WITH ORDINALITY AS k(attnum, ord) ON true
JOIN pg_attribute a ON a.attrelid = c.oid
    AND a.attnum = k.attnum
    AND NOT a.attisdropped
WHERE n.nspname = %(schema)s
  AND c.relname = %(table)s
  AND k.ord <= CASE
        WHEN ytp.nhash > 0 THEN ytp.nhash
        ELSE cardinality(idx.indkey)
      END
ORDER BY k.ord;
"""


class CopyDataError(Exception):
    pass


def quote_ident(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def qualified_regclass(schema: str, name: str) -> str:
    return f"{quote_ident(schema)}.{quote_ident(name)}"


def get_row_count(cur, schema: str, table: str) -> int:
    reg = qualified_regclass(schema, table)
    cur.execute(f"SELECT count(*)::bigint FROM {reg}")
    row = cur.fetchone()
    if row is None:
        raise CopyDataError(f"Could not count rows for {schema}.{table}")
    return int(row[0])


def data_copy_method(row_count: int) -> str:
    """Return 'skip', 'insert', or 'copy' for the given row count."""
    if row_count == 0:
        return "skip"
    if row_count < COPY_ROW_THRESHOLD:
        return "insert"
    return "copy"


def get_table_columns(cur, schema: str, table: str) -> List[str]:
    cur.execute(TABLE_COLUMNS_SQL, {"schema": schema, "table": table})
    rows = cur.fetchall()
    if not rows:
        raise CopyDataError(f"No copyable columns found for {schema}.{table}")
    return [r[0] for r in rows]


def get_hash_key_columns(cur, schema: str, table: str) -> List[str]:
    cur.execute(HASH_KEY_COLUMNS_SQL, {"schema": schema, "table": table})
    rows = cur.fetchall()
    if not rows:
        raise CopyDataError(
            f"No primary-key / hash-key columns found for {schema}.{table}"
        )
    return [r[0] for r in rows]


def yb_hash_code_expr(columns: Sequence[str]) -> str:
    args = ", ".join(quote_ident(c) for c in columns)
    return f"yb_hash_code({args})"


def hash_bucket_predicate(
    hash_columns: Sequence[str],
    num_threads: int,
    bucket: int,
) -> str:
    if num_threads <= 1:
        return "TRUE"
    expr = yb_hash_code_expr(hash_columns)
    return f"mod({expr}, {num_threads}) = {bucket}"


def _column_list_sql(columns: Sequence[str]) -> str:
    return ", ".join(quote_ident(c) for c in columns)


def build_copy_out_sql(
    schema: str,
    table: str,
    columns: Sequence[str],
    hash_columns: Sequence[str],
    num_threads: int,
    bucket: int,
) -> str:
    reg = qualified_regclass(schema, table)
    cols = _column_list_sql(columns)
    where = hash_bucket_predicate(hash_columns, num_threads, bucket)
    return f"COPY (SELECT {cols} FROM {reg} WHERE {where}) TO STDOUT"


def build_copy_in_sql(schema: str, table: str, columns: Sequence[str]) -> str:
    reg = qualified_regclass(schema, table)
    cols = _column_list_sql(columns)
    return f"COPY {reg} ({cols}) FROM STDIN"


def _is_psycopg3(conn) -> bool:
    module = type(conn).__module__
    return module.startswith("psycopg.") and not module.startswith("psycopg2")


def _pipe_copy_psycopg3(
    connect_fn: ConnectFn,
    copy_out_sql: str,
    copy_in_sql: str,
    bucket: int,
    monitor: Optional[CopyProgressMonitor] = None,
) -> int:
    src_conn = connect_fn(bucket, role="src")
    dst_conn = connect_fn(bucket, role="dst")
    rows = 0
    try:
        if monitor is not None:
            monitor.register(
                bucket,
                src_pid=backend_pid(src_conn),
                dst_pid=backend_pid(dst_conn),
            )
        with src_conn.cursor() as src_cur, dst_conn.cursor() as dst_cur:
            with src_cur.copy(copy_out_sql) as copy_out:
                with dst_cur.copy(copy_in_sql) as copy_in:
                    for chunk in copy_out:
                        if chunk:
                            copy_in.write(chunk)
                            rows += chunk.count(b"\n")
        dst_conn.commit()
    except Exception:
        dst_conn.rollback()
        raise
    finally:
        if monitor is not None:
            monitor.complete_bucket(bucket, final_rows=rows if rows > 0 else None)
        src_conn.close()
        dst_conn.close()
    logger.debug("Bucket %d copied (~%d data rows)", bucket, rows)
    return rows


def _pipe_copy_psycopg2(
    connect_fn: ConnectFn,
    copy_out_sql: str,
    copy_in_sql: str,
    bucket: int,
    monitor: Optional[CopyProgressMonitor] = None,
) -> int:
    read_fd, write_fd = os.pipe()
    errors: List[BaseException] = []
    pids_lock = threading.Lock()
    src_pid: List[Optional[int]] = [None]
    dst_pid: List[Optional[int]] = [None]

    def producer() -> None:
        conn = connect_fn(bucket, role="src")
        try:
            with pids_lock:
                src_pid[0] = backend_pid(conn)
                if monitor is not None:
                    monitor.register(bucket, src_pid=src_pid[0])
            with os.fdopen(write_fd, "wb", closefd=True) as sink:
                with conn.cursor() as cur:
                    cur.copy_expert(copy_out_sql, sink)
            conn.commit()
        except BaseException as exc:
            errors.append(exc)
        finally:
            conn.close()

    def consumer() -> None:
        conn = None
        fd_opened = False
        try:
            conn = connect_fn(bucket, role="dst")
            with pids_lock:
                dst_pid[0] = backend_pid(conn)
                if monitor is not None:
                    monitor.register(bucket, dst_pid=dst_pid[0])
            with os.fdopen(read_fd, "rb", closefd=True) as source:
                fd_opened = True
                with conn.cursor() as cur:
                    cur.copy_expert(copy_in_sql, source)
            conn.commit()
        except BaseException as exc:
            errors.append(exc)
            if not fd_opened:
                try:
                    os.close(read_fd)
                except OSError:
                    pass
        finally:
            if conn is not None:
                conn.close()

    t_out = threading.Thread(target=producer, name=f"copy-out-{bucket}")
    t_in = threading.Thread(target=consumer, name=f"copy-in-{bucket}")
    t_out.start()
    t_in.start()
    t_out.join()
    t_in.join()
    try:
        if errors:
            if len(errors) > 1:
                logger.debug(
                    "Multiple errors in bucket %d; reporting first. Second: %s",
                    bucket,
                    errors[1],
                )
            raise errors[0]
        return 0
    finally:
        if monitor is not None:
            monitor.complete_bucket(bucket)


def copy_bucket(
    connect_fn: ConnectFn,
    src_schema: str,
    src_table: str,
    dst_schema: str,
    dst_table: str,
    columns: Sequence[str],
    hash_columns: Sequence[str],
    num_threads: int,
    bucket: int,
    monitor: Optional[CopyProgressMonitor] = None,
) -> None:
    copy_out_sql = build_copy_out_sql(
        src_schema, src_table, columns, hash_columns, num_threads, bucket
    )
    copy_in_sql = build_copy_in_sql(dst_schema, dst_table, columns)
    logger.info(
        "COPY bucket %d/%d for %s.%s -> %s.%s",
        bucket + 1,
        num_threads,
        src_schema,
        src_table,
        dst_schema,
        dst_table,
    )

    # Probe connection type from a throwaway connection (CLI host).
    probe = connect_fn()
    try:
        use_pg3 = _is_psycopg3(probe)
    finally:
        probe.close()

    if use_pg3:
        _pipe_copy_psycopg3(
            connect_fn, copy_out_sql, copy_in_sql, bucket, monitor=monitor
        )
    else:
        _pipe_copy_psycopg2(
            connect_fn, copy_out_sql, copy_in_sql, bucket, monitor=monitor
        )


def insert_select_table(
    connect_fn: Callable[[], object],
    src_schema: str,
    src_table: str,
    dst_schema: str,
    dst_table: str,
    row_count: int,
) -> None:
    src_reg = qualified_regclass(src_schema, src_table)
    dst_reg = qualified_regclass(dst_schema, dst_table)
    logger.info(
        "INSERT INTO %s SELECT FROM %s (%d rows)",
        dst_reg,
        src_reg,
        row_count,
    )
    conn = connect_fn()
    prev_autocommit = conn.autocommit
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            cur.execute(f"INSERT INTO {dst_reg} SELECT * FROM {src_reg}")
    finally:
        conn.autocommit = prev_autocommit
        conn.close()


def migrate_table_data(
    connect_fn: Callable[[], object],
    cur,
    src_schema: str,
    src_table: str,
    dst_schema: str,
    dst_table: str,
    num_threads: int,
    row_count: int,
    show_progress: bool = False,
) -> str:
    """
    Copy rows from src to dst. Returns the method used: 'skip', 'insert', or 'copy'.
    """
    method = data_copy_method(row_count)
    if method == "skip":
        logger.info(
            "Skipping data copy for %s.%s (0 rows)",
            src_schema,
            src_table,
        )
        return method
    if method == "insert":
        insert_select_table(
            connect_fn, src_schema, src_table, dst_schema, dst_table, row_count
        )
        return method
    parallel_copy_table(
        connect_fn,
        cur,
        src_schema,
        src_table,
        dst_schema,
        dst_table,
        num_threads,
        row_count,
        show_progress=show_progress,
    )
    return method


def parallel_copy_table(
    connect_fn: Callable[[], object],
    cur,
    src_schema: str,
    src_table: str,
    dst_schema: str,
    dst_table: str,
    num_threads: int,
    row_count: int,
    show_progress: bool = False,
) -> None:
    if num_threads < 1:
        raise CopyDataError("--copy-threads must be >= 1")

    columns = get_table_columns(cur, src_schema, src_table)
    hash_columns = get_hash_key_columns(cur, src_schema, src_table)

    table_label = f"{src_schema}.{src_table} -> {dst_schema}.{dst_table}"
    logger.info(
        "Starting parallel piped COPY with %d threads (yb_hash_code mod %d) "
        "for %s (%d rows)",
        num_threads,
        num_threads,
        table_label,
        row_count,
    )
    if hasattr(connect_fn, "log_copy_node_plan"):
        connect_fn.log_copy_node_plan(num_threads)  # type: ignore[attr-defined]

    errors: List[Tuple[int, BaseException]] = []

    with CopyProgressMonitor(
        connect_fn,
        num_threads,
        row_count,
        table_label,
        enabled=show_progress,
    ) as monitor:
        with ThreadPoolExecutor(max_workers=num_threads) as pool:
            futures = {
                pool.submit(
                    copy_bucket,
                    connect_fn,
                    src_schema,
                    src_table,
                    dst_schema,
                    dst_table,
                    columns,
                    hash_columns,
                    num_threads,
                    bucket,
                    monitor if show_progress else None,
                ): bucket
                for bucket in range(num_threads)
            }
            for future in as_completed(futures):
                bucket = futures[future]
                try:
                    future.result()
                except BaseException as exc:
                    errors.append((bucket, exc))

        if monitor.failed:
            raise CopyDataError(monitor.error_message or "COPY reported ERROR status")

    if errors:
        details = ", ".join(f"bucket {b}: {e}" for b, e in sorted(errors))
        raise CopyDataError(f"Parallel COPY failed: {details}")
