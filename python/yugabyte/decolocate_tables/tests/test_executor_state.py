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

"""Unit tests for executor state detection and restart-safety helpers."""

from __future__ import annotations

import unittest
from unittest.mock import MagicMock, call, patch

from decolocate_tables.executor import (
    ExecutorError,
    _STATE_COMPLETE,
    _STATE_PHASE1_DONE,
    _STATE_READY,
    _batch_state,
    _detect_table_state,
    _execute_sql_idempotent,
    _pgcode,
)


class TestPgcode(unittest.TestCase):
    def test_psycopg2_pgcode(self):
        exc = Exception()
        exc.pgcode = "42710"
        self.assertEqual(_pgcode(exc), "42710")

    def test_psycopg3_sqlstate(self):
        exc = Exception()
        exc.sqlstate = "42P07"
        self.assertEqual(_pgcode(exc), "42P07")

    def test_psycopg3_diag(self):
        diag = MagicMock()
        diag.sqlstate = "23505"
        exc = Exception()
        exc.diag = diag
        self.assertEqual(_pgcode(exc), "23505")

    def test_no_code(self):
        self.assertIsNone(_pgcode(ValueError("boom")))


class TestDetectTableState(unittest.TestCase):
    def _make_cur(self, rows):
        cur = MagicMock()
        cur.fetchall.return_value = rows
        return cur

    def test_ready(self):
        cur = self._make_cur([("orders", True)])
        state = _detect_table_state(cur, "public", "orders", "_bak")
        self.assertEqual(state, _STATE_READY)

    def test_phase1_done(self):
        cur = self._make_cur([("orders", False), ("orders_bak", True)])
        state = _detect_table_state(cur, "public", "orders", "_bak")
        self.assertEqual(state, _STATE_PHASE1_DONE)

    def test_complete(self):
        cur = self._make_cur([("orders", False)])
        state = _detect_table_state(cur, "public", "orders", "_bak")
        self.assertEqual(state, _STATE_COMPLETE)

    def test_corrupt_backup_only(self):
        cur = self._make_cur([("orders_bak", True)])
        with self.assertRaises(ExecutorError):
            _detect_table_state(cur, "public", "orders", "_bak")

    def test_neither_exists_raises(self):
        cur = self._make_cur([])
        with self.assertRaises(ExecutorError):
            _detect_table_state(cur, "public", "orders", "_bak")


class TestBatchState(unittest.TestCase):
    def test_all_ready(self):
        self.assertEqual(_batch_state({"a": _STATE_READY, "b": _STATE_READY}), _STATE_READY)

    def test_all_phase1(self):
        self.assertEqual(
            _batch_state({"a": _STATE_PHASE1_DONE, "b": _STATE_PHASE1_DONE}),
            _STATE_PHASE1_DONE,
        )

    def test_all_complete(self):
        self.assertEqual(
            _batch_state({"a": _STATE_COMPLETE, "b": _STATE_COMPLETE}),
            _STATE_COMPLETE,
        )

    def test_mixed_phase1_complete_returns_phase1(self):
        result = _batch_state({"a": _STATE_PHASE1_DONE, "b": _STATE_COMPLETE})
        self.assertEqual(result, _STATE_PHASE1_DONE)

    def test_mixed_ready_and_phase1_raises(self):
        with self.assertRaises(ExecutorError):
            _batch_state({"a": _STATE_READY, "b": _STATE_PHASE1_DONE})

    def test_mixed_ready_and_complete_raises(self):
        with self.assertRaises(ExecutorError):
            _batch_state({"a": _STATE_READY, "b": _STATE_COMPLETE})


class TestExecuteSqlIdempotent(unittest.TestCase):
    def _make_cur(self):
        return MagicMock()

    def test_executes_each_statement(self):
        cur = self._make_cur()
        _execute_sql_idempotent(cur, "CREATE INDEX i ON t(a); CREATE INDEX j ON t(b);", "test")
        executed = [c.args[0] for c in cur.execute.call_args_list if c.args[0] not in
                    ("SAVEPOINT _decolocate_idem", "RELEASE SAVEPOINT _decolocate_idem")]
        self.assertEqual(len(executed), 2)

    def test_skips_duplicate_object(self):
        cur = MagicMock()
        dup_exc = Exception("already exists")
        dup_exc.pgcode = "42710"

        call_count = 0

        def execute_side_effect(sql, *args, **kwargs):
            nonlocal call_count
            if sql in ("SAVEPOINT _decolocate_idem",
                       "ROLLBACK TO SAVEPOINT _decolocate_idem",
                       "RELEASE SAVEPOINT _decolocate_idem"):
                return
            call_count += 1
            if "CREATE INDEX" in sql:
                raise dup_exc

        cur.execute.side_effect = execute_side_effect
        # Should NOT raise even though CREATE INDEX fails with duplicate error.
        _execute_sql_idempotent(cur, "CREATE INDEX i ON t(a);", "test")

    def test_reraises_non_duplicate_error(self):
        cur = MagicMock()
        fatal_exc = Exception("permission denied")
        fatal_exc.pgcode = "42501"

        def execute_side_effect(sql, *args, **kwargs):
            if sql in ("SAVEPOINT _decolocate_idem",
                       "ROLLBACK TO SAVEPOINT _decolocate_idem",
                       "RELEASE SAVEPOINT _decolocate_idem"):
                return
            if "CREATE INDEX" in sql:
                raise fatal_exc

        cur.execute.side_effect = execute_side_effect
        with self.assertRaises(Exception) as ctx:
            _execute_sql_idempotent(cur, "CREATE INDEX i ON t(a);", "test")
        self.assertIs(ctx.exception, fatal_exc)


if __name__ == "__main__":
    unittest.main()
