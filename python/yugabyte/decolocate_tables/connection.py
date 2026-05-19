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
from dataclasses import dataclass
from typing import List, Optional

logger = logging.getLogger(__name__)

YB_SERVERS_SQL = """
SELECT DISTINCT host, port::int AS port
FROM yb_servers()
WHERE COALESCE(node_type, 'primary') = 'primary'
ORDER BY host, port;
"""


@dataclass(frozen=True)
class YbServer:
    host: str
    port: int


def fetch_yb_servers(cur) -> List[YbServer]:
    """
    Return primary YSQL nodes from yb_servers().

    Raises nothing on failure; callers should fall back to the CLI host.
    """
    cur.execute(YB_SERVERS_SQL)
    rows = cur.fetchall()
    servers: List[YbServer] = []
    seen: set[tuple[str, int]] = set()
    for host, port in rows:
        if host is None or port is None:
            continue
        host_s = str(host).strip()
        port_i = int(port)
        key = (host_s, port_i)
        if key in seen:
            continue
        seen.add(key)
        servers.append(YbServer(host=host_s, port=port_i))
    return servers


def server_for_bucket(
    servers: List[YbServer],
    bucket: int,
    *,
    role: str = "default",
) -> YbServer:
    """
    Pick a node for a parallel COPY worker.

    * ``role='src'``  – COPY TO (read from backup)
    * ``role='dst'``  – COPY FROM (write to target); uses the next node when possible
    * ``role='default'`` – same as ``src`` (monitoring, single connection)
    """
    if not servers:
        raise ValueError("server_for_bucket requires a non-empty server list")
    n = len(servers)
    base = bucket % n
    if role == "dst" and n > 1:
        return servers[(base + 1) % n]
    return servers[base]


def connect_psycopg(conninfo: dict, host: str, port: int):
    try:
        import psycopg
    except ImportError:
        import psycopg2 as psycopg  # type: ignore

    dsn = (
        f"host={host} port={port} "
        f"dbname={conninfo['dbname']} user={conninfo['user']}"
    )
    if conninfo.get("password"):
        dsn += f" password={conninfo['password']}"
    return psycopg.connect(dsn)


class ConnectionFactory:
    """
    Build database connections, spreading parallel COPY workers across
    ``yb_servers()`` nodes when available.
    """

    def __init__(self, conninfo: dict) -> None:
        self._conninfo = dict(conninfo)
        self._default_host = str(conninfo["host"])
        self._default_port = int(conninfo["port"])
        self._servers: List[YbServer] = []

    @property
    def servers(self) -> List[YbServer]:
        return list(self._servers)

    def discover_nodes(self, conn) -> None:
        """Populate the server list from yb_servers(); fall back to CLI host."""
        try:
            with conn.cursor() as cur:
                servers = fetch_yb_servers(cur)
        except Exception as exc:
            logger.warning(
                "Could not query yb_servers() (%s); using --host %s:%s for all COPY workers",
                exc,
                self._default_host,
                self._default_port,
            )
            servers = []

        if not servers:
            servers = [YbServer(self._default_host, self._default_port)]
            logger.info(
                "No primary nodes from yb_servers(); using %s:%s for COPY",
                self._default_host,
                self._default_port,
            )
        else:
            logger.info(
                "Discovered %d primary node(s) from yb_servers(): %s",
                len(servers),
                ", ".join(f"{s.host}:{s.port}" for s in servers),
            )

        self._servers = servers

    def connect(
        self,
        bucket: Optional[int] = None,
        *,
        role: str = "default",
    ):
        if bucket is None or not self._servers:
            host, port = self._default_host, self._default_port
        else:
            srv = server_for_bucket(self._servers, bucket, role=role)
            host, port = srv.host, srv.port
            logger.debug(
                "COPY bucket %d %s connection -> %s:%s",
                bucket,
                role,
                host,
                port,
            )
        return connect_psycopg(self._conninfo, host, port)

    def __call__(
        self,
        bucket: Optional[int] = None,
        *,
        role: str = "default",
    ):
        return self.connect(bucket, role=role)

    def log_copy_node_plan(self, num_buckets: int) -> None:
        """Log which node each COPY worker bucket will use."""
        if not self._servers:
            return
        n = len(self._servers)
        for bucket in range(num_buckets):
            src = server_for_bucket(self._servers, bucket, role="src")
            dst = server_for_bucket(self._servers, bucket, role="dst")
            if src == dst:
                logger.info(
                    "COPY bucket %d/%d -> %s:%s (read + write)",
                    bucket + 1,
                    num_buckets,
                    src.host,
                    src.port,
                )
            else:
                logger.info(
                    "COPY bucket %d/%d -> read %s:%s, write %s:%s",
                    bucket + 1,
                    num_buckets,
                    src.host,
                    src.port,
                    dst.host,
                    dst.port,
                )
        if num_buckets > n:
            logger.info(
                "More COPY workers (%d) than nodes (%d); nodes are reused round-robin",
                num_buckets,
                n,
            )
