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

import unittest

from decolocate_tables.models import parse_table_list


class TestParseTableList(unittest.TestCase):
    def test_single_table(self):
        names = parse_table_list(["public.orders"])
        self.assertEqual([str(n) for n in names], ["public.orders"])

    def test_comma_separated(self):
        names = parse_table_list(["public.orders, public.order_items"])
        self.assertEqual(
            [str(n) for n in names],
            ["public.orders", "public.order_items"],
        )

    def test_semicolon_separated(self):
        names = parse_table_list(["public.a;public.b"])
        self.assertEqual([str(n) for n in names], ["public.a", "public.b"])

    def test_repeatable_flags_merged(self):
        names = parse_table_list(
            ["public.a,public.b", "public.c"]
        )
        self.assertEqual(
            [str(n) for n in names],
            ["public.a", "public.b", "public.c"],
        )

    def test_deduplicates(self):
        names = parse_table_list(["public.a, public.a", "public.a"])
        self.assertEqual([str(n) for n in names], ["public.a"])

    def test_bare_table_defaults_to_public(self):
        names = parse_table_list(["orders, line_items"])
        self.assertEqual([str(n) for n in names], ["public.orders", "public.line_items"])


if __name__ == "__main__":
    unittest.main()
