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

import re
from dataclasses import dataclass, field
from typing import List, Optional, Sequence


@dataclass(frozen=True)
class QualifiedName:
    schema: str
    name: str

    @classmethod
    def parse(cls, text: str) -> "QualifiedName":
        text = text.strip()
        if "." in text:
            schema, name = text.split(".", 1)
            return cls(schema=schema, name=name)
        return cls(schema="public", name=text)

    def __str__(self) -> str:
        return f"{self.schema}.{self.name}"

    def regclass(self) -> str:
        return f'"{self.schema}"."{self.name}"'


def parse_table_list(table_args: Sequence[str]) -> List[QualifiedName]:
    """
    Parse --table CLI values. Each argument may list multiple tables separated
    by commas or semicolons. Repeatable --table flags are merged (deduplicated).
    """
    result: List[QualifiedName] = []
    seen: set[str] = set()
    for arg in table_args:
        for piece in re.split(r"[,;]+", arg):
            piece = piece.strip()
            if not piece:
                continue
            qn = QualifiedName.parse(piece)
            key = str(qn)
            if key in seen:
                continue
            seen.add(key)
            result.append(qn)
    return result


@dataclass
class TableInfo:
    qualified: QualifiedName
    oid: int
    relkind: str
    is_colocated: bool
    # OID used for dependency discovery (pg_depend).  After Phase 1 rename this
    # is the backup table's OID, which still matches the pre-migration table OID.
    source_oid: Optional[int] = None
    # True when Phase 1 already committed (target uncollocated, backup present).
    resuming: bool = False
    create_table_sql_path: Optional[str] = None
    post_create_sql_path: Optional[str] = None
    backup_name: Optional[str] = None
    row_count: Optional[int] = None
    data_copy_method: Optional[str] = None
    had_statistics: Optional[bool] = None
    will_analyze: Optional[bool] = None


@dataclass
class ViewInfo:
    qualified: QualifiedName
    oid: int
    depends_on: List[str] = field(default_factory=list)
    ddl_file: Optional[str] = None


@dataclass
class MigrationPlan:
    tables: List[TableInfo]
    views_create_order: List[ViewInfo]
    views_drop_order: List[ViewInfo]
    work_dir: str
    dry_run: bool = True
    copy_threads: int = 4
    analyze_if_had_stats: bool = True
