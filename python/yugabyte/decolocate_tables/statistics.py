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

from decolocate_tables.copy_data import qualified_regclass

logger = logging.getLogger(__name__)

TABLE_HAS_STATISTICS_SQL = """
SELECT EXISTS (
    SELECT 1
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    JOIN pg_statistic s ON s.starelid = c.oid
    WHERE n.nspname = %(schema)s
      AND c.relname = %(table)s
);
"""


class StatisticsError(Exception):
    pass


def table_has_statistics(cur, schema: str, table: str) -> bool:
    """Return true if pg_statistic has rows for the table (ANALYZE was run)."""
    cur.execute(TABLE_HAS_STATISTICS_SQL, {"schema": schema, "table": table})
    row = cur.fetchone()
    if row is None:
        raise StatisticsError(f"Could not check statistics for {schema}.{table}")
    return bool(row[0])


def run_analyze(conn, schema: str, table: str) -> None:
    reg = qualified_regclass(schema, table)
    logger.info("Running ANALYZE on %s", reg)
    prev_autocommit = conn.autocommit
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            cur.execute(f"ANALYZE {reg}")
    finally:
        conn.autocommit = prev_autocommit
