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
import logging
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from decolocate_tables.models import MigrationPlan, QualifiedName, TableInfo, ViewInfo

logger = logging.getLogger(__name__)


def plan_to_dict(plan: MigrationPlan) -> Dict[str, Any]:
    return {
        "dry_run": plan.dry_run,
        "work_dir": plan.work_dir,
        "copy_threads": plan.copy_threads,
        "analyze_if_had_stats": plan.analyze_if_had_stats,
        "tables": [
            {
                "schema": t.qualified.schema,
                "name": t.qualified.name,
                "oid": t.oid,
                "relkind": t.relkind,
                "is_colocated": t.is_colocated,
                "resuming": t.resuming,
                "source_oid": t.source_oid,
                "backup_name": t.backup_name,
                "row_count": t.row_count,
                "data_copy_method": t.data_copy_method,
                "had_statistics": t.had_statistics,
                "will_analyze": t.will_analyze,
                "create_table_sql": t.create_table_sql_path,
                "post_create_sql": t.post_create_sql_path,
            }
            for t in plan.tables
        ],
        "views_drop_order": [
            {
                "schema": v.qualified.schema,
                "name": v.qualified.name,
                "oid": v.oid,
                "depends_on": v.depends_on,
                "ddl_file": v.ddl_file,
            }
            for v in plan.views_drop_order
        ],
        "views_create_order": [
            {
                "schema": v.qualified.schema,
                "name": v.qualified.name,
                "oid": v.oid,
                "depends_on": v.depends_on,
                "ddl_file": v.ddl_file,
            }
            for v in plan.views_create_order
        ],
    }


def write_manifest(plan: MigrationPlan, path: Path) -> None:
    path.write_text(json.dumps(plan_to_dict(plan), indent=2) + "\n", encoding="utf-8")


def _resolve_ddl_path(work_dir: Path, ddl_file: Optional[str]) -> Optional[str]:
    if not ddl_file:
        return None
    path = Path(ddl_file)
    if path.is_file():
        return str(path.resolve())
    alt = work_dir / path.name
    if alt.is_file():
        return str(alt.resolve())
    return None


def _views_from_manifest_data(
    work_dir: Path, entries: List[Dict[str, Any]]
) -> List[ViewInfo]:
    views: List[ViewInfo] = []
    for v in entries:
        qn = QualifiedName(schema=v["schema"], name=v["name"])
        ddl_path = _resolve_ddl_path(work_dir, v.get("ddl_file"))
        if ddl_path is None:
            logger.warning(
                "Manifest lists view %s but DDL file is missing under %s",
                qn,
                work_dir,
            )
            continue
        views.append(
            ViewInfo(
                qualified=qn,
                oid=v.get("oid", 0),
                depends_on=list(v.get("depends_on", [])),
                ddl_file=ddl_path,
            )
        )
    return views


def load_manifest_views(
    work_dir: Path,
) -> Tuple[List[ViewInfo], List[ViewInfo]]:
    """
    Load view recreate order from a prior manifest.json in work_dir.

    Returns (views_create_order, views_drop_order).  Empty lists when no
    manifest exists or it contains no views with resolvable DDL files.
    """
    path = work_dir / "manifest.json"
    if not path.is_file():
        return [], []

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        logger.warning("Could not read manifest %s: %s", path, exc)
        return [], []

    views_create = _views_from_manifest_data(
        work_dir, data.get("views_create_order", [])
    )
    views_drop = list(reversed(views_create))
    return views_create, views_drop


def merge_view_lists(
    discovered_create: List[ViewInfo],
    discovered_drop: List[ViewInfo],
    manifest_create: List[ViewInfo],
) -> Tuple[List[ViewInfo], List[ViewInfo]]:
    """
    Merge catalog-discovered views with views loaded from a prior manifest.

    Manifest entries win on name collision (they carry persisted DDL paths).
    Preserves manifest create order, then appends any newly discovered views.
    """
    if not manifest_create:
        return discovered_create, discovered_drop

    merged: List[ViewInfo] = []
    seen: set[str] = set()
    for v in manifest_create:
        key = str(v.qualified)
        if key not in seen:
            merged.append(v)
            seen.add(key)
    for v in discovered_create:
        key = str(v.qualified)
        if key not in seen:
            merged.append(v)
            seen.add(key)
    return merged, list(reversed(merged))
