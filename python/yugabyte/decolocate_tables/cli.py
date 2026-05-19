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

import argparse
import logging
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import List, Optional

from decolocate_tables.connection import ConnectionFactory
from decolocate_tables.copy_data import COPY_ROW_THRESHOLD, data_copy_method, get_row_count
from decolocate_tables.discovery import DiscoveryError, discover
from decolocate_tables.statistics import table_has_statistics
from decolocate_tables.dump import (
    capture_table_ddl,
    capture_table_ddl_resume,
    capture_view_ddl,
)
from decolocate_tables.executor import ExecutorError, execute_plan
from decolocate_tables.manifest import (
    load_manifest_views,
    merge_view_lists,
    write_manifest,
)
from decolocate_tables.models import MigrationPlan, parse_table_list


def _find_ysql_dump(explicit: Optional[str]) -> str:
    if explicit:
        if not os.path.isfile(explicit) or not os.access(explicit, os.X_OK):
            raise SystemExit(f"ysql_dump not found or not executable: {explicit}")
        return explicit
    found = shutil.which("ysql_dump")
    if not found:
        raise SystemExit(
            "ysql_dump not found on PATH; use --ysql-dump to specify the binary"
        )
    return found


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Migrate colocated YSQL tables to uncollocated (COLOCATION=false) "
            "and recreate dependent views."
        ),
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5433)
    parser.add_argument("--dbname", required=True)
    parser.add_argument("--user", default="yugabyte")
    parser.add_argument("--password", default=os.environ.get("PGPASSWORD", ""))
    parser.add_argument(
        "--table",
        action="append",
        dest="tables",
        required=True,
        metavar="SCHEMA.TABLE",
        help=(
            "Colocated table to decolocate (repeatable). Each value may be a "
            "comma- or semicolon-separated list, e.g. "
            "--table public.a,public.b"
        ),
    )
    parser.add_argument(
        "--ysql-dump",
        default=None,
        help="Path to ysql_dump binary (default: search PATH)",
    )
    parser.add_argument(
        "--work-dir",
        default=None,
        help="Directory for captured DDL and manifest (default: temp dir)",
    )
    parser.add_argument(
        "--backup-suffix",
        default="_colocated_bak",
        help="Suffix for renamed backup tables during migration",
    )
    parser.add_argument(
        "--split-into-tablets",
        type=int,
        default=1,
        metavar="N",
        help=(
            "SPLIT INTO N TABLETS on new uncollocated CREATE TABLE statements "
            "(default: 1)"
        ),
    )
    parser.add_argument(
        "--lock-timeout",
        default="30s",
        help="SET lock_timeout for the migration transaction",
    )
    parser.add_argument(
        "--statement-timeout",
        default=None,
        help="SET statement_timeout for the migration transaction",
    )
    parser.add_argument(
        "--copy-threads",
        type=int,
        default=4,
        metavar="N",
        help=(
            "Number of parallel COPY workers per table, each copying rows where "
            "mod(yb_hash_code(<pk>), N) equals the worker bucket (default: 4)"
        ),
    )
    parser.add_argument(
        "--no-analyze-if-had-stats",
        action="store_true",
        help=(
            "Do not run ANALYZE on migrated tables even when the source had "
            "column statistics (use when auto-analyze is enabled)"
        ),
    )
    parser.add_argument(
        "--no-progress",
        action="store_true",
        help=(
            "Disable progress bars for parallel COPY (pg_stat_progress_copy) "
            "and CREATE INDEX (pg_stat_progress_create_index). "
            "Phase summaries are still printed during --execute"
        ),
    )
    parser.add_argument(
        "--execute",
        action="store_true",
        help="Run the migration (default is dry-run: plan and capture DDL only)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable debug logging",
    )
    return parser


def _print_plan_summary(plan: MigrationPlan, manifest_path: Path) -> None:
    print(f"Work directory: {plan.work_dir}")
    print(f"Manifest: {manifest_path}")
    print(f"Mode: {'DRY RUN' if plan.dry_run else 'EXECUTE'}")
    print(f"COPY threads: {plan.copy_threads} (tables with >={COPY_ROW_THRESHOLD} rows)")
    if plan.analyze_if_had_stats:
        print("Post-migrate ANALYZE: enabled when source has pg_statistic rows")
    else:
        print("Post-migrate ANALYZE: disabled")
    print()
    print("Tables (migration order):")
    for t in plan.tables:
        row_info = ""
        if t.row_count is not None:
            row_info = f", {t.row_count} rows -> {t.data_copy_method or data_copy_method(t.row_count)}"
        stats_info = ""
        if t.had_statistics is not None:
            if t.will_analyze:
                stats_info = ", ANALYZE after migrate"
            else:
                stats_info = ", no source stats"
        elif not plan.analyze_if_had_stats:
            stats_info = ", ANALYZE skipped (disabled)"
        resume_info = ", resuming Phase 1" if t.resuming else ""
        print(f"  - {t.qualified} (oid={t.oid}{resume_info}{row_info}{stats_info})")
        if t.create_table_sql_path:
            print(f"      create: {t.create_table_sql_path}")
        if t.post_create_sql_path:
            print(f"      post:   {t.post_create_sql_path}")
    print()
    if plan.views_drop_order:
        print("Views (drop order):")
        for v in plan.views_drop_order:
            deps = ", ".join(v.depends_on) if v.depends_on else "(base)"
            print(f"  - {v.qualified}  depends_on: {deps}")
        print()
        print("Views (create order):")
        for v in plan.views_create_order:
            print(f"  - {v.qualified}")
    else:
        print("No dependent views found.")
    print()
    if plan.dry_run:
        print("Re-run with --execute to apply changes.")


def run(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s: %(message)s",
    )

    dry_run = not args.execute  # default: dry-run
    table_names = parse_table_list(args.tables)
    if not table_names:
        raise SystemExit("At least one table is required (--table SCHEMA.TABLE)")

    conninfo = {
        "host": args.host,
        "port": args.port,
        "dbname": args.dbname,
        "user": args.user,
        "password": args.password,
    }

    ysql_dump = _find_ysql_dump(args.ysql_dump)

    work_dir_obj: Optional[tempfile.TemporaryDirectory] = None
    if args.work_dir:
        work_dir = Path(args.work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        if args.execute:
            logging.warning(
                "No --work-dir specified: DDL files will be written to a "
                "temporary directory that is deleted when the process exits.  "
                "If the migration is interrupted and needs to be restarted, "
                "re-run with --execute (the script detects in-progress state "
                "automatically) but note that the DDL files will be re-captured "
                "from scratch.  Use --work-dir for a persistent, inspectable "
                "working directory."
            )
        work_dir_obj = tempfile.TemporaryDirectory(prefix="decolocate_tables_")
        work_dir = Path(work_dir_obj.name)

    conn_factory = ConnectionFactory(conninfo)
    conn = conn_factory.connect()
    try:
        analyze_if_had_stats = not args.no_analyze_if_had_stats

        tables, views_create, views_drop = discover(
            conn, table_names, backup_suffix=args.backup_suffix
        )

        resuming_any = any(t.resuming for t in tables)
        if resuming_any:
            manifest_views_create, _manifest_views_drop = load_manifest_views(
                work_dir
            )
            views_create, views_drop = merge_view_lists(
                views_create, views_drop, manifest_views_create
            )
            if not views_create:
                logging.warning(
                    "Resuming migration after Phase 1: dependent views were "
                    "already dropped and no manifest.json with view DDL was "
                    "found in the work directory.  Views will not be recreated.  "
                    "Re-run with the same --work-dir used for the initial attempt."
                )
            elif manifest_views_create:
                logging.info(
                    "Loaded %d view(s) from prior manifest for resume",
                    len(manifest_views_create),
                )

        with conn.cursor() as cur:
            for table in tables:
                qn = table.qualified
                data_source = (
                    table.backup_name if table.resuming and table.backup_name else qn.name
                )
                table.row_count = get_row_count(cur, qn.schema, data_source)
                table.data_copy_method = data_copy_method(table.row_count)
                if analyze_if_had_stats:
                    table.had_statistics = table_has_statistics(
                        cur, qn.schema, data_source
                    )
                    table.will_analyze = bool(table.had_statistics)
                else:
                    table.had_statistics = None
                    table.will_analyze = False

        for table in tables:
            if table.resuming:
                if not table.backup_name:
                    raise DiscoveryError(
                        f"Resuming table {table.qualified} is missing backup_name"
                    )
                capture_table_ddl_resume(
                    table,
                    work_dir,
                    ysql_dump,
                    conninfo,
                    table.backup_name,
                )
            else:
                capture_table_ddl(
                    table,
                    work_dir,
                    ysql_dump,
                    conninfo,
                    split_into_tablets=args.split_into_tablets,
                )

        for view in views_create:
            capture_view_ddl(view, work_dir, ysql_dump, conninfo)

        plan = MigrationPlan(
            tables=tables,
            views_create_order=views_create,
            views_drop_order=views_drop,
            work_dir=str(work_dir.resolve()),
            dry_run=dry_run,
            copy_threads=args.copy_threads,
            analyze_if_had_stats=analyze_if_had_stats,
        )

        manifest_path = work_dir / "manifest.json"
        write_manifest(plan, manifest_path)
        _print_plan_summary(plan, manifest_path)

        if args.copy_threads < 1:
            raise SystemExit("--copy-threads must be >= 1")

        if args.execute:
            conn_factory.discover_nodes(conn)

        show_copy_progress = (
            args.execute
            and not args.no_progress
            and sys.stderr.isatty()
        )
        execute_plan(
            conn,
            plan,
            backup_suffix=args.backup_suffix,
            connect_fn=conn_factory,
            copy_threads=args.copy_threads,
            lock_timeout=args.lock_timeout,
            statement_timeout=args.statement_timeout,
            show_phase_progress=args.execute,
            show_copy_progress=show_copy_progress,
        )
    except (DiscoveryError, ExecutorError) as exc:
        logging.error("%s", exc)
        return 1
    except Exception as exc:
        logging.exception("Unexpected error: %s", exc)
        return 1
    finally:
        conn.close()
        if work_dir_obj is not None and dry_run:
            logging.info("Dry-run work dir preserved at %s", work_dir)

    return 0


def main() -> None:
    sys.exit(run())
