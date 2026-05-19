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

import io
import unittest

from decolocate_tables.progress import (
    PhaseReporter,
    estimate_bucket_rows,
    extract_index_display_name,
    format_table_progress,
    is_create_index_statement,
)


class TestEstimateBucketRows(unittest.TestCase):
    def test_even_split(self):
        self.assertEqual(estimate_bucket_rows(100, 4, 0), 25)
        self.assertEqual(estimate_bucket_rows(100, 4, 3), 25)

    def test_remainder(self):
        self.assertEqual(estimate_bucket_rows(10, 3, 0), 4)
        self.assertEqual(estimate_bucket_rows(10, 3, 1), 3)
        self.assertEqual(estimate_bucket_rows(10, 3, 2), 3)
        self.assertEqual(sum(estimate_bucket_rows(10, 3, b) for b in range(3)), 10)


class TestFormatTableProgress(unittest.TestCase):
    def test_includes_position(self):
        msg = format_table_progress(2, 10, "finalize public.orders")
        self.assertEqual(msg, "Table 2 of 10: finalize public.orders")


class TestCreateIndexParsing(unittest.TestCase):
    def test_detects_create_index(self):
        self.assertTrue(
            is_create_index_statement(
                'CREATE INDEX idx_orders_customer ON public.orders (customer_id);'
            )
        )
        self.assertTrue(
            is_create_index_statement(
                "CREATE UNIQUE INDEX IF NOT EXISTS \"idx\" ON t (id);"
            )
        )
        self.assertFalse(
            is_create_index_statement("ALTER TABLE t ADD CONSTRAINT c PRIMARY KEY (id);")
        )

    def test_extracts_index_name(self):
        stmt = 'CREATE UNIQUE INDEX idx_orders_customer ON public.orders (id);'
        self.assertEqual(extract_index_display_name(stmt), "idx_orders_customer")
        stmt2 = 'CREATE INDEX "MyIdx" ON public.t (a);'
        self.assertEqual(extract_index_display_name(stmt2), "MyIdx")


class TestPhaseReporter(unittest.TestCase):
    def test_prints_phases_when_enabled(self):
        buf = io.StringIO()
        reporter = PhaseReporter(enabled=True, stream=buf)
        reporter.start(1, "Data migration", "2 tables")
        reporter.step("public.t1: copy")
        reporter.complete("done")
        out = buf.getvalue()
        self.assertIn("Phase 1: Data migration", out)
        self.assertIn("public.t1: copy", out)
        self.assertIn("complete", out)

    def test_silent_when_disabled(self):
        buf = io.StringIO()
        reporter = PhaseReporter(enabled=False, stream=buf)
        reporter.start(1, "Data migration")
        reporter.step("hidden")
        reporter.complete()
        self.assertEqual(buf.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
