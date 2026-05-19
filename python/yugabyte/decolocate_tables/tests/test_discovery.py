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

from decolocate_tables.discovery import DiscoveryError, topo_sort_tables, topo_sort_views


class TestTopoSortViews(unittest.TestCase):
    def test_linear_chain(self):
        # v3 -> v2 -> v1
        oids = {1, 2, 3}
        edges = [(3, 2), (2, 1)]
        order = topo_sort_views(oids, edges)
        self.assertEqual(order, [1, 2, 3])

    def test_cycle_raises(self):
        with self.assertRaises(DiscoveryError):
            topo_sort_views({1, 2}, [(1, 2), (2, 1)])


class TestTopoSortTables(unittest.TestCase):
    def test_parent_first(self):
        # child=10 references parent=20
        order = topo_sort_tables([10, 20], [(10, 20)])
        self.assertEqual(order, [20, 10])


if __name__ == "__main__":
    unittest.main()
