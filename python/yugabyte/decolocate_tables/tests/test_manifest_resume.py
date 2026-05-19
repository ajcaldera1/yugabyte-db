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

import json
import tempfile
import unittest
from pathlib import Path

from decolocate_tables.manifest import load_manifest_views, merge_view_lists
from decolocate_tables.models import QualifiedName, ViewInfo


class TestMergeViewLists(unittest.TestCase):
    def test_manifest_views_used_when_present(self):
        discovered = [
            ViewInfo(qualified=QualifiedName("public", "v_new"), oid=1),
        ]
        manifest = [
            ViewInfo(
                qualified=QualifiedName("public", "v_old"),
                oid=2,
                ddl_file="/tmp/v_old.sql",
            ),
        ]
        create, drop = merge_view_lists(discovered, [], manifest)
        self.assertEqual(len(create), 2)
        self.assertEqual(str(create[0].qualified), "public.v_old")
        self.assertEqual(str(create[1].qualified), "public.v_new")
        self.assertEqual(str(drop[0].qualified), "public.v_new")

    def test_no_manifest_returns_discovered(self):
        discovered = [ViewInfo(qualified=QualifiedName("public", "v1"), oid=1)]
        create, drop = merge_view_lists(discovered, list(reversed(discovered)), [])
        self.assertEqual(create, discovered)


class TestLoadManifestViews(unittest.TestCase):
    def test_loads_views_with_relative_ddl_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            ddl = work / "view_public_v1.sql"
            ddl.write_text("CREATE VIEW v1 AS SELECT 1;\n", encoding="utf-8")
            manifest = {
                "views_create_order": [
                    {
                        "schema": "public",
                        "name": "v1",
                        "oid": 99,
                        "depends_on": [],
                        "ddl_file": str(ddl),
                    }
                ]
            }
            (work / "manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            create, drop = load_manifest_views(work)
            self.assertEqual(len(create), 1)
            self.assertEqual(str(create[0].qualified), "public.v1")
            self.assertTrue(Path(create[0].ddl_file).is_file())
            self.assertEqual(len(drop), 1)


if __name__ == "__main__":
    unittest.main()
