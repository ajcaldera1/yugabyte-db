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

from decolocate_tables.models import MigrationPlan, QualifiedName, TableInfo
from decolocate_tables.statistics import TABLE_HAS_STATISTICS_SQL


class TestStatisticsSql(unittest.TestCase):
    def test_query_uses_pg_statistic(self):
        self.assertIn("pg_statistic", TABLE_HAS_STATISTICS_SQL)
        self.assertIn("starelid", TABLE_HAS_STATISTICS_SQL)


class TestWillAnalyzePlan(unittest.TestCase):
    def test_analyze_when_enabled_and_had_stats(self):
        table = TableInfo(
            qualified=QualifiedName("public", "t"),
            oid=1,
            relkind="r",
            is_colocated=True,
            had_statistics=True,
            will_analyze=True,
        )
        plan = MigrationPlan(
            tables=[table],
            views_create_order=[],
            views_drop_order=[],
            work_dir="/tmp",
            analyze_if_had_stats=True,
        )
        self.assertTrue(plan.analyze_if_had_stats and table.will_analyze)

    def test_no_analyze_when_disabled(self):
        table = TableInfo(
            qualified=QualifiedName("public", "t"),
            oid=1,
            relkind="r",
            is_colocated=True,
            had_statistics=True,
            will_analyze=False,
        )
        plan = MigrationPlan(
            tables=[table],
            views_create_order=[],
            views_drop_order=[],
            work_dir="/tmp",
            analyze_if_had_stats=False,
        )
        self.assertFalse(table.will_analyze)


if __name__ == "__main__":
    unittest.main()
