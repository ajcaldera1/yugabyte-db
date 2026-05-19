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

import logging
from collections import defaultdict, deque
from typing import Dict, List, Set, Tuple

from decolocate_tables.models import QualifiedName, TableInfo, ViewInfo

logger = logging.getLogger(__name__)

DEPENDENT_VIEWS_SQL = """
WITH RECURSIVE dependent_views AS (
    SELECT DISTINCT c.oid AS view_oid
    FROM unnest(%(target_oids)s::oid[]) AS t(oid)
    JOIN pg_depend d ON d.refobjid = t.oid
        AND d.refclassid = 'pg_class'::regclass
        AND d.classid = 'pg_rewrite'::regclass
        AND d.deptype = 'n'
    JOIN pg_rewrite r ON r.oid = d.objid
    JOIN pg_class c ON c.oid = r.ev_class AND c.relkind = 'v'

    UNION

    SELECT DISTINCT c2.oid AS view_oid
    FROM dependent_views dv
    JOIN pg_depend d2 ON d2.refobjid = dv.view_oid
        AND d2.refclassid = 'pg_class'::regclass
        AND d2.classid = 'pg_rewrite'::regclass
        AND d2.deptype = 'n'
    JOIN pg_rewrite r2 ON r2.oid = d2.objid
    JOIN pg_class c2 ON c2.oid = r2.ev_class AND c2.relkind = 'v'
)
SELECT dv.view_oid,
       n.nspname AS schema_name,
       c.relname AS view_name
FROM dependent_views dv
JOIN pg_class c ON c.oid = dv.view_oid
JOIN pg_namespace n ON n.oid = c.relnamespace
ORDER BY n.nspname, c.relname;
"""

VIEW_VIEW_EDGES_SQL = """
WITH RECURSIVE dependent_views AS (
    SELECT DISTINCT c.oid AS view_oid
    FROM unnest(%(target_oids)s::oid[]) AS t(oid)
    JOIN pg_depend d ON d.refobjid = t.oid
        AND d.refclassid = 'pg_class'::regclass
        AND d.classid = 'pg_rewrite'::regclass
        AND d.deptype = 'n'
    JOIN pg_rewrite r ON r.oid = d.objid
    JOIN pg_class c ON c.oid = r.ev_class AND c.relkind = 'v'

    UNION

    SELECT DISTINCT c2.oid AS view_oid
    FROM dependent_views dv
    JOIN pg_depend d2 ON d2.refobjid = dv.view_oid
        AND d2.refclassid = 'pg_class'::regclass
        AND d2.classid = 'pg_rewrite'::regclass
        AND d2.deptype = 'n'
    JOIN pg_rewrite r2 ON r2.oid = d2.objid
    JOIN pg_class c2 ON c2.oid = r2.ev_class AND c2.relkind = 'v'
)
SELECT dv.view_oid AS dependent_oid,
       c_ref.oid AS ref_oid,
       nr.nspname AS ref_schema,
       c_ref.relname AS ref_name
FROM dependent_views dv
JOIN pg_rewrite r ON r.ev_class = dv.view_oid
JOIN pg_depend d ON d.objid = r.oid
    AND d.classid = 'pg_rewrite'::regclass
    AND d.deptype = 'n'
    AND d.refclassid = 'pg_class'::regclass
JOIN pg_class c_ref ON c_ref.oid = d.refobjid AND c_ref.relkind = 'v'
JOIN pg_namespace nr ON nr.oid = c_ref.relnamespace
WHERE c_ref.oid IN (SELECT view_oid FROM dependent_views)
  AND c_ref.oid <> dv.view_oid;
"""

BLOCKING_MATVIEWS_SQL = """
SELECT DISTINCT n.nspname, c.relname
FROM unnest(%(target_oids)s::oid[]) AS t(oid)
JOIN pg_depend d ON d.refobjid = t.oid
    AND d.refclassid = 'pg_class'::regclass
    AND d.classid = 'pg_rewrite'::regclass
    AND d.deptype = 'n'
JOIN pg_rewrite r ON r.oid = d.objid
JOIN pg_class c ON c.oid = r.ev_class AND c.relkind = 'm'
JOIN pg_namespace n ON n.oid = c.relnamespace
ORDER BY 1, 2;
"""

INBOUND_FK_SQL = """
SELECT conrelid::regclass::text AS referencing_table, conname
FROM pg_constraint
WHERE contype = 'f'
  AND confrelid = ANY(%(target_oids)s::oid[])
  AND NOT (conrelid = ANY(%(target_oids)s::oid[]))
ORDER BY 1, 2;
"""

TABLE_FK_ORDER_SQL = """
SELECT conrelid AS child_oid, confrelid AS parent_oid
FROM pg_constraint
WHERE contype = 'f'
  AND conrelid = ANY(%(target_oids)s::oid[])
  AND confrelid = ANY(%(target_oids)s::oid[])
  AND conrelid <> confrelid;
"""

TABLE_LOOKUP_SQL = """
SELECT c.oid, c.relkind,
       ytp.is_colocated
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
LEFT JOIN LATERAL (
    SELECT is_colocated FROM yb_table_properties(c.oid)
) ytp ON true
WHERE n.nspname = %(schema)s AND c.relname = %(name)s;
"""

DATABASE_COLOCATED_SQL = "SELECT yb_is_database_colocated() AS colocated;"

BACKUP_EXISTS_SQL = """
SELECT EXISTS (
    SELECT 1
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = %(schema)s AND c.relname = %(backup)s AND c.relkind = 'r'
);
"""

BACKUP_LOOKUP_SQL = """
SELECT c.oid
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = %(schema)s AND c.relname = %(backup)s AND c.relkind = 'r';
"""


class DiscoveryError(Exception):
    pass


def topo_sort_views(view_oids: Set[int], edges: List[Tuple[int, int]]) -> List[int]:
    """
    Topological sort for view creation order.
    Edge (dependent, dependency) means dependency must be created before dependent.
    """
    deps: Dict[int, Set[int]] = defaultdict(set)
    dependents: Dict[int, Set[int]] = defaultdict(set)
    nodes = set(view_oids)

    for dependent, dependency in edges:
        if dependent not in nodes or dependency not in nodes:
            continue
        if dependent == dependency:
            continue
        deps[dependent].add(dependency)
        dependents[dependency].add(dependent)

    in_degree = {n: len(deps[n]) for n in nodes}
    queue = deque(sorted(n for n in nodes if in_degree[n] == 0))
    result: List[int] = []

    while queue:
        node = queue.popleft()
        result.append(node)
        for child in sorted(dependents[node]):
            in_degree[child] -= 1
            if in_degree[child] == 0:
                queue.append(child)

    if len(result) != len(nodes):
        remaining = nodes - set(result)
        raise DiscoveryError(
            f"View dependency cycle detected among OIDs: {sorted(remaining)}"
        )
    return result


def topo_sort_tables(table_oids: List[int], fk_edges: List[Tuple[int, int]]) -> List[int]:
    """
    Sort tables so referenced tables (parents) are migrated first.
    Edge (child_oid, parent_oid) from FK: child references parent.
    """
    oid_set = set(table_oids)
    deps: Dict[int, Set[int]] = defaultdict(set)
    dependents: Dict[int, Set[int]] = defaultdict(set)

    for child, parent in fk_edges:
        if child not in oid_set or parent not in oid_set:
            continue
        deps[child].add(parent)
        dependents[parent].add(child)

    in_degree = {n: len(deps[n]) for n in oid_set}
    queue = deque(sorted(n for n in oid_set if in_degree[n] == 0))
    result: List[int] = []

    while queue:
        node = queue.popleft()
        result.append(node)
        for child in sorted(dependents[node]):
            in_degree[child] -= 1
            if in_degree[child] == 0:
                queue.append(child)

    if len(result) != len(oid_set):
        raise DiscoveryError("Foreign-key cycle among target tables")
    return result


def discover(
    conn,
    table_names: List[QualifiedName],
    backup_suffix: str = "_colocated_bak",
) -> Tuple[List[TableInfo], List[ViewInfo], List[ViewInfo]]:
    with conn.cursor() as cur:
        cur.execute(DATABASE_COLOCATED_SQL)
        row = cur.fetchone()
        if not row or not row[0]:
            raise DiscoveryError(
                "Current database is not colocated (yb_is_database_colocated() is false)"
            )

        tables: List[TableInfo] = []
        target_oids: List[int] = []

        for qn in table_names:
            cur.execute(
                TABLE_LOOKUP_SQL,
                {"schema": qn.schema, "name": qn.name},
            )
            row = cur.fetchone()
            if row is None:
                raise DiscoveryError(f"Table not found: {qn}")
            oid, relkind, is_colocated = row
            if relkind != "r":
                raise DiscoveryError(
                    f"{qn} is not an ordinary table (relkind={relkind!r})"
                )
            if is_colocated is False:
                backup_name = f"{qn.name}{backup_suffix}"
                cur.execute(
                    BACKUP_EXISTS_SQL,
                    {"schema": qn.schema, "backup": backup_name},
                )
                backup_exists = cur.fetchone()[0]
                if backup_exists:
                    cur.execute(
                        BACKUP_LOOKUP_SQL,
                        {"schema": qn.schema, "backup": backup_name},
                    )
                    backup_row = cur.fetchone()
                    if backup_row is None:
                        raise DiscoveryError(
                            f"Backup table {qn.schema}.{backup_name} "
                            "disappeared during discovery"
                        )
                    backup_oid = backup_row[0]
                    info = TableInfo(
                        qualified=qn,
                        oid=oid,
                        relkind=relkind,
                        is_colocated=False,
                        source_oid=backup_oid,
                        resuming=True,
                        backup_name=backup_name,
                    )
                    tables.append(info)
                    target_oids.append(backup_oid)
                    logger.info(
                        "Resuming interrupted migration for %s "
                        "(backup %s.%s, oid=%d)",
                        qn,
                        qn.schema,
                        backup_name,
                        backup_oid,
                    )
                    continue
                logger.warning(
                    "Skipping %s: already uncollocated and no backup found", qn
                )
                continue
            if is_colocated is None:
                raise DiscoveryError(
                    f"Could not read colocation status for {qn} "
                    "(yb_table_properties unavailable?)"
                )
            info = TableInfo(
                qualified=qn,
                oid=oid,
                relkind=relkind,
                is_colocated=True,
                source_oid=oid,
            )
            tables.append(info)
            target_oids.append(oid)

        if not tables:
            raise DiscoveryError("No colocated tables to migrate")

        cur.execute(INBOUND_FK_SQL, {"target_oids": target_oids})
        inbound_fks = cur.fetchall()
        if inbound_fks:
            lines = [f"  {ref} ({conname})" for ref, conname in inbound_fks]
            raise DiscoveryError(
                "Inbound foreign keys from tables outside the migration set:\n"
                + "\n".join(lines)
            )

        cur.execute(BLOCKING_MATVIEWS_SQL, {"target_oids": target_oids})
        matviews = cur.fetchall()
        if matviews:
            lines = [f"  {s}.{n}" for s, n in matviews]
            raise DiscoveryError(
                "Materialized views depend on target table(s); not supported:\n"
                + "\n".join(lines)
            )

        cur.execute(DEPENDENT_VIEWS_SQL, {"target_oids": target_oids})
        view_rows = cur.fetchall()

        cur.execute(VIEW_VIEW_EDGES_SQL, {"target_oids": target_oids})
        edge_rows = cur.fetchall()

        cur.execute(TABLE_FK_ORDER_SQL, {"target_oids": target_oids})
        fk_rows = cur.fetchall()

    view_by_oid: Dict[int, ViewInfo] = {}
    for oid, schema, name in view_rows:
        view_by_oid[oid] = ViewInfo(
            qualified=QualifiedName(schema=schema, name=name),
            oid=oid,
        )

    edges: List[Tuple[int, int]] = []
    depends_map: Dict[int, Set[str]] = defaultdict(set)
    for dependent_oid, ref_oid, ref_schema, ref_name in edge_rows:
        edges.append((dependent_oid, ref_oid))
        depends_map[dependent_oid].add(f"{ref_schema}.{ref_name}")

    for v in view_by_oid.values():
        v.depends_on = sorted(depends_map.get(v.oid, []))

    create_order_oids = topo_sort_views(set(view_by_oid.keys()), edges)
    views_create = [view_by_oid[oid] for oid in create_order_oids]
    views_drop = list(reversed(views_create))

    # FK metadata may reference backup OIDs after Phase 1; map to target table OIDs.
    oid_to_table: Dict[int, TableInfo] = {}
    for t in tables:
        oid_to_table[t.oid] = t
        dep_oid = t.source_oid if t.source_oid is not None else t.oid
        oid_to_table[dep_oid] = t

    fk_edges_for_sort: List[Tuple[int, int]] = []
    for child, parent in fk_rows:
        child_t = oid_to_table.get(child)
        parent_t = oid_to_table.get(parent)
        if child_t is not None and parent_t is not None:
            fk_edges_for_sort.append((child_t.oid, parent_t.oid))

    table_order_oids = topo_sort_tables(
        [t.oid for t in tables],
        fk_edges_for_sort,
    )
    table_by_oid = {t.oid: t for t in tables}
    tables_ordered = [table_by_oid[oid] for oid in table_order_oids]

    logger.info(
        "Discovered %d table(s), %d dependent view(s)",
        len(tables_ordered),
        len(views_create),
    )
    return tables_ordered, views_create, views_drop
