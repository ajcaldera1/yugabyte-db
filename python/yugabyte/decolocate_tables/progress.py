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
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Set, TextIO

logger = logging.getLogger(__name__)

try:
    from tqdm import tqdm
except ImportError:  # pragma: no cover - optional at runtime
    tqdm = None  # type: ignore

COPY_PROGRESS_SQL = """
SELECT pid,
       command,
       tuples_processed,
       bytes_processed,
       bytes_total,
       yb_status
FROM pg_stat_progress_copy
WHERE pid = ANY(%(pids)s);
"""

CREATE_INDEX_PROGRESS_SQL = """
SELECT phase,
       tuples_total,
       tuples_done,
       partitions_total,
       partitions_done,
       command
FROM pg_stat_progress_create_index
WHERE pid = %(pid)s;
"""

CREATE_INDEX_STMT_RE = re.compile(
    r"^\s*CREATE\s+(?:UNIQUE\s+)?INDEX\b",
    re.IGNORECASE,
)

CREATE_INDEX_NAME_RE = re.compile(
    r"CREATE\s+(?:UNIQUE\s+)?INDEX\s+(?:CONCURRENTLY\s+)?"
    r"(?:IF\s+NOT\s+EXISTS\s+)?"
    r'(?:"([^"]+)"|\'([^\']+)\'|([\w.]+))',
    re.IGNORECASE,
)


def estimate_bucket_rows(total_rows: int, num_threads: int, bucket: int) -> int:
    """Approximate row count for a hash bucket (even split of total_rows)."""
    if num_threads <= 0:
        return total_rows
    base, remainder = divmod(total_rows, num_threads)
    return base + (1 if bucket < remainder else 0)


def backend_pid(conn) -> int:
    with conn.cursor() as cur:
        cur.execute("SELECT pg_backend_pid()")
        row = cur.fetchone()
    if row is None:
        raise RuntimeError("pg_backend_pid() returned no row")
    return int(row[0])


def cursor_backend_pid(cur) -> int:
    cur.execute("SELECT pg_backend_pid()")
    row = cur.fetchone()
    if row is None:
        raise RuntimeError("pg_backend_pid() returned no row")
    return int(row[0])


def is_create_index_statement(stmt: str) -> bool:
    return bool(CREATE_INDEX_STMT_RE.match(stmt.strip()))


def extract_index_display_name(stmt: str) -> str:
    match = CREATE_INDEX_NAME_RE.search(stmt)
    if match:
        return match.group(1) or match.group(2) or match.group(3) or "index"
    return "index"


def format_table_progress(index: int, total: int, message: str) -> str:
    """Format a step message with 1-based table position (e.g. 'Table 2 of 10: ...')."""
    return f"Table {index} of {total}: {message}"


class PhaseReporter:
    """Print high-level migration phase boundaries to stdout."""

    def __init__(self, enabled: bool = True, stream: TextIO | None = None) -> None:
        self.enabled = enabled
        self._stream = stream or sys.stdout
        self._phase_start: float = 0.0
        self._current_phase: Optional[int] = None

    def start(self, phase: int, title: str, detail: str = "") -> None:
        if not self.enabled:
            return
        self._current_phase = phase
        self._phase_start = time.monotonic()
        line = f"\n=== Phase {phase}: {title} ==="
        if detail:
            line += f"\n    {detail}"
        print(line, file=self._stream, flush=True)

    def step(self, message: str) -> None:
        if not self.enabled:
            return
        print(f"  {message}", file=self._stream, flush=True)

    def complete(self, message: str = "done") -> None:
        if not self.enabled:
            return
        elapsed = time.monotonic() - self._phase_start
        phase = self._current_phase if self._current_phase is not None else "?"
        print(
            f"=== Phase {phase} complete ({elapsed:.1f}s): {message} ===\n",
            file=self._stream,
            flush=True,
        )
        self._current_phase = None

    def table_header(self, table_num: int, total: int, qualified: str) -> None:
        if not self.enabled:
            return
        print(
            f"\n=== Table {table_num} of {total}: {qualified} ===",
            file=self._stream,
            flush=True,
        )


@dataclass
class _BucketState:
    bucket: int
    estimated_total: int
    dst_pid: Optional[int] = None
    src_pid: Optional[int] = None
    bar: Any = None
    done: bool = False
    last_tuples: int = 0
    last_status: str = ""


class CopyProgressMonitor:
    """
    Poll pg_stat_progress_copy and update one tqdm bar per parallel COPY worker.

    Tracks the COPY FROM backend (destination load) as the primary row counter.
    COPY TO progress is shown in the bar postfix when available.
    """

    def __init__(
        self,
        connect_fn: Callable[..., object],
        num_buckets: int,
        total_rows: int,
        table_label: str,
        *,
        enabled: bool = True,
        poll_interval: float = 0.5,
    ) -> None:
        # Node-aware factory: connect_fn(bucket, role='src'|'dst')
        self._connect_fn = connect_fn
        self._num_buckets = num_buckets
        self._total_rows = total_rows
        self._table_label = table_label
        self._enabled = enabled and tqdm is not None
        self._poll_interval = poll_interval
        self._lock = threading.Lock()
        self._buckets: Dict[int, _BucketState] = {}
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._had_error = False
        self._error_message: Optional[str] = None

        if enabled and tqdm is None:
            logger.warning(
                "tqdm is not installed; COPY progress bars disabled. "
                "Install with: pip install tqdm"
            )

        for bucket in range(num_buckets):
            est = estimate_bucket_rows(total_rows, num_buckets, bucket)
            bar = None
            if self._enabled:
                bar = tqdm(
                    total=max(est, 1),
                    desc=f"  bucket {bucket + 1}/{num_buckets}",
                    unit="rows",
                    position=bucket,
                    leave=True,
                    file=sys.stderr,
                    dynamic_ncols=True,
                )
            self._buckets[bucket] = _BucketState(
                bucket=bucket,
                estimated_total=est,
                bar=bar,
            )

    @property
    def enabled(self) -> bool:
        return self._enabled

    def __enter__(self) -> CopyProgressMonitor:
        if self._enabled:
            print(
                f"COPY progress ({self._table_label}): "
                f"{self._total_rows:,} rows, {self._num_buckets} worker(s)",
                file=sys.stderr,
                flush=True,
            )
            self._thread = threading.Thread(
                target=self._poll_loop,
                name="copy-progress-monitor",
                daemon=True,
            )
            self._thread.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=self._poll_interval + 2.0)
        self._close_bars()

    @property
    def failed(self) -> bool:
        return self._had_error

    @property
    def error_message(self) -> Optional[str]:
        return self._error_message

    def register(
        self,
        bucket: int,
        *,
        dst_pid: Optional[int] = None,
        src_pid: Optional[int] = None,
    ) -> None:
        with self._lock:
            state = self._buckets[bucket]
            if dst_pid is not None:
                state.dst_pid = dst_pid
            if src_pid is not None:
                state.src_pid = src_pid

    def complete_bucket(self, bucket: int, final_rows: Optional[int] = None) -> None:
        with self._lock:
            state = self._buckets[bucket]
            state.done = True
            if state.bar is not None:
                n = final_rows if final_rows is not None else state.last_tuples
                if n > 0:
                    if n > state.bar.total:
                        state.bar.total = n
                    state.bar.n = n
                state.bar.set_postfix_str("done", refresh=True)
                state.bar.refresh()

    def _poll_loop(self) -> None:
        while not self._stop.wait(self._poll_interval):
            self._poll_once()
        self._poll_once()

    def _poll_progress_on_node(
        self, bucket: int, role: str, pids: Set[int]
    ) -> Dict[int, tuple]:
        if not pids:
            return {}
        try:
            conn = self._connect_fn(bucket, role=role)
        except TypeError:
            conn = self._connect_fn()
        try:
            with conn.cursor() as cur:
                cur.execute(COPY_PROGRESS_SQL, {"pids": list(pids)})
                rows = cur.fetchall()
            return {int(r[0]): r for r in rows}
        except Exception as exc:
            logger.debug(
                "pg_stat_progress_copy poll failed (bucket=%d role=%s): %s",
                bucket,
                role,
                exc,
            )
            return {}
        finally:
            conn.close()

    def _poll_once(self) -> None:
        with self._lock:
            buckets = {
                b: s
                for b, s in self._buckets.items()
                if not s.done and s.bar is not None
            }

        for bucket, state in buckets.items():
            by_pid: Dict[int, tuple] = {}
            if state.dst_pid is not None:
                by_pid.update(
                    self._poll_progress_on_node(bucket, "dst", {state.dst_pid})
                )
            if state.src_pid is not None and state.src_pid != state.dst_pid:
                by_pid.update(
                    self._poll_progress_on_node(bucket, "src", {state.src_pid})
                )
            if not by_pid:
                continue
            with self._lock:
                if state.done or state.bar is None:
                    continue
                self._update_bucket_from_stats(state, by_pid)

    def _update_bucket_from_stats(
        self,
        state: _BucketState,
        by_pid: Dict[int, tuple],
    ) -> None:
        dst_row = by_pid.get(state.dst_pid) if state.dst_pid else None
        src_row = by_pid.get(state.src_pid) if state.src_pid else None

        primary = dst_row or src_row
        if primary is None:
            return

        _pid, command, tuples_processed, bytes_processed, bytes_total, yb_status = (
            primary
        )
        tuples_processed = int(tuples_processed or 0)
        bytes_processed = int(bytes_processed or 0)
        bytes_total = int(bytes_total or 0)
        status = str(yb_status or "IN PROGRESS")

        state.last_tuples = tuples_processed
        state.last_status = status

        if status == "ERROR":
            self._had_error = True
            self._error_message = (
                f"COPY failed for bucket {state.bucket + 1} "
                f"(pid {state.dst_pid}, status ERROR)"
            )
            if state.bar is not None:
                state.bar.set_postfix_str("ERROR", refresh=True)
            return

        if state.bar is not None:
            if tuples_processed > state.bar.total:
                state.bar.total = tuples_processed
            state.bar.n = tuples_processed
            parts: List[str] = []
            if command:
                parts.append(str(command).strip())
            if src_row and dst_row and src_row is not dst_row:
                src_tuples = int(src_row[2] or 0)
                parts.append(f"read {src_tuples:,}")
            if bytes_total > 0:
                pct = 100.0 * bytes_processed / bytes_total
                parts.append(f"bytes {pct:.0f}%")
            elif bytes_processed > 0:
                parts.append(f"bytes {bytes_processed:,}")
            if status != "IN PROGRESS":
                parts.append(status)
            if parts:
                state.bar.set_postfix(", ".join(parts), refresh=False)
            state.bar.refresh()

        if status == "SUCCESS":
            state.done = True
            if state.bar is not None:
                if tuples_processed > state.bar.total:
                    state.bar.total = tuples_processed
                state.bar.n = tuples_processed
                state.bar.set_postfix_str("SUCCESS", refresh=True)

    def _close_bars(self) -> None:
        with self._lock:
            for state in self._buckets.values():
                if state.bar is not None:
                    state.bar.close()


class CreateIndexProgressBar:
    """
    Poll pg_stat_progress_create_index while a single CREATE INDEX runs.

    Intended for sequential index creation during the finalize phase.  Uses
    tuples_done / tuples_total from the view (YugabyteDB backfill progress).
    """

    def __init__(
        self,
        connect_fn: Callable[[], object],
        pid: int,
        index_name: str,
        index_num: int,
        index_total: int,
        estimated_tuples: int,
        *,
        enabled: bool = True,
        poll_interval: float = 0.5,
    ) -> None:
        self._connect_fn = connect_fn
        self._pid = pid
        self._index_name = index_name
        self._index_num = index_num
        self._index_total = index_total
        self._estimated_tuples = max(estimated_tuples, 1)
        self._enabled = enabled and tqdm is not None
        self._poll_interval = poll_interval
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._bar: Any = None
        self._last_tuples_done = 0

    def __enter__(self) -> CreateIndexProgressBar:
        if self._enabled:
            self._bar = tqdm(
                total=self._estimated_tuples,
                desc=(
                    f"  [{self._index_num}/{self._index_total}] "
                    f"{self._index_name}"
                ),
                unit="tuples",
                position=0,
                leave=True,
                file=sys.stderr,
                dynamic_ncols=True,
            )
            self._thread = threading.Thread(
                target=self._poll_loop,
                name=f"create-index-progress-{self._pid}",
                daemon=True,
            )
            self._thread.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=self._poll_interval + 2.0)
        if self._bar is not None:
            if self._last_tuples_done > self._bar.total:
                self._bar.total = self._last_tuples_done
            self._bar.n = max(self._bar.n, self._last_tuples_done)
            if exc_type is None:
                self._bar.set_postfix_str("done", refresh=True)
            self._bar.close()

    def _poll_loop(self) -> None:
        conn = self._connect_fn()
        try:
            while not self._stop.wait(self._poll_interval):
                self._poll_once(conn)
            self._poll_once(conn)
        finally:
            conn.close()

    def _poll_once(self, conn) -> None:
        if self._bar is None:
            return
        try:
            with conn.cursor() as cur:
                cur.execute(CREATE_INDEX_PROGRESS_SQL, {"pid": self._pid})
                row = cur.fetchone()
        except Exception as exc:
            logger.debug("pg_stat_progress_create_index poll failed: %s", exc)
            return

        if row is None:
            return

        phase, tuples_total, tuples_done, parts_total, parts_done, command = row
        tuples_total = int(tuples_total or 0)
        tuples_done = int(tuples_done or 0)
        parts_total = int(parts_total or 0)
        parts_done = int(parts_done or 0)
        self._last_tuples_done = tuples_done

        total = tuples_total if tuples_total > 0 else self._estimated_tuples
        if total > self._bar.total:
            self._bar.total = total
        self._bar.n = min(tuples_done, total) if total > 0 else tuples_done

        parts: List[str] = []
        if phase:
            parts.append(str(phase))
        if parts_total > 0:
            parts.append(f"partitions {parts_done}/{parts_total}")
        if command:
            parts.append(str(command).strip())
        if parts:
            self._bar.set_postfix(", ".join(parts), refresh=False)
        self._bar.refresh()
