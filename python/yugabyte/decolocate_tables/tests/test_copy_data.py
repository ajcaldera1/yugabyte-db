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

import unittest

from decolocate_tables.copy_data import (
    COPY_ROW_THRESHOLD,
    build_copy_in_sql,
    build_copy_out_sql,
    data_copy_method,
    hash_bucket_predicate,
    yb_hash_code_expr,
)


class TestDataCopyMethod(unittest.TestCase):
    def test_skip_empty(self):
        self.assertEqual(data_copy_method(0), "skip")

    def test_insert_below_threshold(self):
        self.assertEqual(data_copy_method(1), "insert")
        self.assertEqual(data_copy_method(COPY_ROW_THRESHOLD - 1), "insert")

    def test_copy_at_threshold(self):
        self.assertEqual(data_copy_method(COPY_ROW_THRESHOLD), "copy")
        self.assertEqual(data_copy_method(COPY_ROW_THRESHOLD + 1), "copy")


class TestHashPredicates(unittest.TestCase):
    def test_single_column_hash(self):
        self.assertEqual(yb_hash_code_expr(["id"]), 'yb_hash_code("id")')

    def test_mod_bucket(self):
        pred = hash_bucket_predicate(["id"], 8, 3)
        self.assertEqual(pred, 'mod(yb_hash_code("id"), 8) = 3')

    def test_single_thread_predicate(self):
        self.assertEqual(hash_bucket_predicate(["id"], 1, 0), "TRUE")


class TestCopySql(unittest.TestCase):
    def test_copy_out_uses_subquery_and_hash(self):
        sql = build_copy_out_sql(
            "public",
            "t_backup",
            ["id", "v"],
            ["id"],
            4,
            2,
        )
        self.assertIn("COPY (SELECT", sql)
        self.assertIn('FROM "public"."t_backup"', sql)
        self.assertIn('mod(yb_hash_code("id"), 4) = 2', sql)
        self.assertIn("TO STDOUT", sql)

    def test_copy_in(self):
        sql = build_copy_in_sql("public", "t", ["id", "v"])
        self.assertEqual(
            sql,
            'COPY "public"."t" ("id", "v") FROM STDIN',
        )


if __name__ == "__main__":
    unittest.main()
