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

from decolocate_tables.connection import (
    YbServer,
    server_for_bucket,
)


class TestServerForBucket(unittest.TestCase):
    def setUp(self) -> None:
        self.servers = [
            YbServer("10.0.0.1", 5433),
            YbServer("10.0.0.2", 5433),
            YbServer("10.0.0.3", 5433),
        ]

    def test_src_round_robin(self):
        self.assertEqual(server_for_bucket(self.servers, 0, role="src").host, "10.0.0.1")
        self.assertEqual(server_for_bucket(self.servers, 1, role="src").host, "10.0.0.2")
        self.assertEqual(server_for_bucket(self.servers, 3, role="src").host, "10.0.0.1")

    def test_dst_uses_next_node_when_multiple(self):
        self.assertEqual(server_for_bucket(self.servers, 0, role="dst").host, "10.0.0.2")
        self.assertEqual(server_for_bucket(self.servers, 2, role="dst").host, "10.0.0.1")

    def test_single_node_src_and_dst_same(self):
        one = [YbServer("127.0.0.1", 5433)]
        self.assertEqual(server_for_bucket(one, 0, role="src"), one[0])
        self.assertEqual(server_for_bucket(one, 0, role="dst"), one[0])


if __name__ == "__main__":
    unittest.main()
