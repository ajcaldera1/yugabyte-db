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

from decolocate_tables.dump import (
    convert_primary_key_to_hash_sharding,
    inject_colocation_false,
    rewrite_table_name_in_sql,
    split_schema_dump,
    strip_view_statements,
)


class TestInjectColocation(unittest.TestCase):
    def test_adds_with_clause(self):
        sql = "CREATE TABLE t (id int PRIMARY KEY, v int);"
        out = inject_colocation_false(sql)
        self.assertIn("COLOCATION = false", out)
        self.assertIn("SPLIT INTO 1 TABLETS", out)
        self.assertRegex(out, r"(?i)WITH\s*\(\s*COLOCATION\s*=\s*false")

    def test_replaces_existing_split_with_default_one(self):
        sql = (
            "CREATE TABLE t (id int PRIMARY KEY) "
            "WITH (COLOCATION = true, SPLIT INTO 8 TABLETS);"
        )
        out = inject_colocation_false(sql)
        self.assertIn("SPLIT INTO 1 TABLETS", out)
        self.assertNotRegex(out, r"(?i)SPLIT\s+INTO\s+8\s+TABLETS")

    def test_replaces_true(self):
        sql = "CREATE TABLE t (id int) WITH (COLOCATION = true);"
        out = inject_colocation_false(sql)
        self.assertIn("COLOCATION = false", out)
        self.assertNotRegex(out, r"(?i)COLOCATION\s*=\s*true")

    def test_partition_of(self):
        sql = (
            "CREATE TABLE parent (k int, PRIMARY KEY (k)) PARTITION BY HASH (k);\n"
            "CREATE TABLE parent_p0 PARTITION OF parent "
            "FOR VALUES WITH (modulus 2, remainder 0) WITH (COLOCATION = true);"
        )
        out = inject_colocation_false(sql)
        self.assertEqual(out.lower().count("colocation = false"), 2)

    def test_split_into_tablets(self):
        sql = "CREATE TABLE t (id int PRIMARY KEY);"
        out = inject_colocation_false(sql, split_into_tablets=3)
        self.assertIn("SPLIT INTO 3 TABLETS", out)

    def test_pk_asc_becomes_hash(self):
        sql = "CREATE TABLE t (a int, PRIMARY KEY (a ASC));"
        out = inject_colocation_false(sql)
        self.assertIn("PRIMARY KEY (a HASH)", out)
        self.assertNotRegex(out, r"(?i)PRIMARY\s+KEY\s*\(\s*a\s+ASC")

    def test_pk_multi_column_leading_asc(self):
        sql = "CREATE TABLE t (a int, b int, PRIMARY KEY (a ASC, b DESC));"
        out = inject_colocation_false(sql)
        self.assertIn("PRIMARY KEY (a HASH, b DESC)", out)

    def test_pk_plain_leading_gets_hash(self):
        sql = "CREATE TABLE t (a int, b int, PRIMARY KEY (a, b DESC));"
        out = inject_colocation_false(sql)
        self.assertIn("PRIMARY KEY (a HASH, b DESC)", out)


class TestConvertPrimaryKeyToHash(unittest.TestCase):
    def test_composite_key(self):
        sql = "CREATE TABLE t (r1 int, r2 int, PRIMARY KEY ((r1, r2) ASC));"
        out = convert_primary_key_to_hash_sharding(sql)
        self.assertIn("PRIMARY KEY ((r1, r2) HASH)", out)

    def test_index_asc_unchanged(self):
        sql = "CREATE INDEX idx ON t (a ASC);"
        out = convert_primary_key_to_hash_sharding(sql)
        self.assertIn("(a ASC)", out)


class TestSplitSchemaDump(unittest.TestCase):
    def test_splits_create_from_index(self):
        sql = (
            "CREATE TABLE t (id int PRIMARY KEY);\n"
            "CREATE INDEX t_idx ON t (id);\n"
            "ALTER TABLE t ADD CONSTRAINT t_chk CHECK (id > 0);"
        )
        create, post = split_schema_dump(sql)
        self.assertIn("CREATE TABLE", create)
        self.assertNotIn("CREATE INDEX", create)
        self.assertIn("CREATE INDEX", post)
        self.assertIn("ALTER TABLE", post)


class TestStripViews(unittest.TestCase):
    def test_removes_create_view(self):
        sql = (
            "CREATE TABLE t (id int);\n"
            "CREATE VIEW v AS SELECT 1;\n"
            "CREATE INDEX t_i ON t (id);"
        )
        out = strip_view_statements(sql)
        self.assertIn("CREATE TABLE", out)
        self.assertNotIn("CREATE VIEW", out)
        self.assertIn("CREATE INDEX", out)


class TestRewriteTableName(unittest.TestCase):
    def test_rewrites_quoted_qualified_name(self):
        sql = 'CREATE INDEX ON "public"."orders_bak" (id);'
        out = rewrite_table_name_in_sql(sql, "public", "orders_bak", "orders")
        self.assertIn('"public"."orders"', out)
        self.assertNotIn("orders_bak", out)

    def test_rewrites_unquoted_qualified_name(self):
        sql = "ALTER TABLE public.orders_bak ADD CONSTRAINT c PRIMARY KEY (id);"
        out = rewrite_table_name_in_sql(sql, "public", "orders_bak", "orders")
        self.assertIn("public.orders", out)
        self.assertNotIn("orders_bak", out)


if __name__ == "__main__":
    unittest.main()
