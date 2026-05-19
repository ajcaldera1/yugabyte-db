# decolocate_tables

Migrate colocated YSQL tables to uncollocated (`WITH (COLOCATION = false)`) and
recreate dependent regular views.

## Prerequisites

- YugabyteDB cluster with a **colocated database**
- `ysql_dump` on `PATH` (or pass `--ysql-dump`)
- Python 3.8+
- `psycopg` (v3): `pip install -r requirements.txt`

## Usage

Set `PYTHONPATH` to `python/yugabyte`, then run:

```bash
export PYTHONPATH=/path/to/yugabyte-db/python/yugabyte

# Dry run (default): discover dependencies, capture DDL, write manifest
python -m decolocate_tables \
  --host 127.0.0.1 --port 5433 --dbname mydb --user yugabyte \
  --table public.orders,public.order_items

# Apply migration
python -m decolocate_tables \
  --host 127.0.0.1 --port 5433 --dbname mydb --user yugabyte \
  --table public.orders \
  --execute
```

Or invoke the package module:

```bash
python -m decolocate_tables --dbname mydb --table public.t1
```

### Options

| Flag | Description |
|------|-------------|
| `--table SCHEMA.TABLE` | Table(s) to decolocate; comma/semicolon-separated lists and repeatable flags are supported |
| `--execute` | Apply changes (default is dry-run) |
| `--work-dir DIR` | Keep captured SQL and `manifest.json` |
| `--backup-suffix SUFFIX` | Suffix for renamed backup tables (default `_colocated_bak`) |
| `--split-into-tablets N` | `SPLIT INTO N TABLETS` on new uncollocated tables (default: `1`) |
| `--ysql-dump PATH` | Path to `ysql_dump` binary |
| `--lock-timeout` | `SET lock_timeout` during migration (default `30s`) |
| `--copy-threads N` | Parallel piped `COPY` workers per table (default `4`) |
| `--no-analyze-if-had-stats` | Skip post-migrate `ANALYZE` (for clusters with auto-analyze) |
| `--no-progress` | Disable per-thread COPY progress bars (phase summaries still print) |

## What it does

1. Validates tables are colocated and the database is colocated
2. Aborts on inbound FKs from tables outside the set or dependent materialized views
3. Discovers all dependent **regular views** (including view-on-view chains)
4. Captures full table DDL via `ysql_dump --schema-only --include-yb-metadata`
5. Injects `COLOCATION = false` into `CREATE TABLE` statements
6. On `--execute`: drops views, recreates each table uncollocated, copies data in parallel via piped `COPY` (partitioned by `mod(yb_hash_code(<pk>), N)`), recreates views

## Data copy

Before copying, the tool counts rows in the source table:

| Row count | Method |
|-----------|--------|
| 0 | No data copy (empty table) |
| 1 – 99,999 | `INSERT INTO ... SELECT * FROM ...` |
| 100,000+ | Parallel piped `COPY` via `--copy-threads` workers |

For large tables, each worker `i` copies rows where `mod(yb_hash_code(<hash-key columns>), N) = i`, using separate database connections so scans can run in parallel across tablets. Data streams in memory between `COPY TO STDOUT` and `COPY FROM STDIN` (no files on disk).

On `--execute`, the tool queries `yb_servers()` and spreads COPY workers across primary nodes when possible: worker `i` reads from one node and writes to the next (round-robin when there are more workers than nodes). DDL and other phases use the `--host` you pass on the command line.

During `--execute`, the tool prints a summary for each migration phase. When stderr is a TTY and `--no-progress` is not set:

- **COPY** (large tables): one bar per worker thread, polling `pg_stat_progress_copy` (`tuples_processed` on `COPY FROM`).
- **CREATE INDEX** (finalize phase): one bar per index, polling `pg_stat_progress_create_index` (`tuples_done` / `tuples_total` during the `backfilling` phase).

For each table in migration order (respecting foreign-key dependencies), the tool runs **all phases before starting the next table**:

Captured `CREATE TABLE` DDL is adjusted for uncollocated tables: `COLOCATION = false`, `SPLIT INTO 1 TABLETS` (by default), and the primary-key sharding column is changed from `ASC` to `HASH`.

1. **DDL** (transaction): drop dependent views on the first table that needs it, rename source, create empty uncollocated shell
2. **Data** (parallel piped `COPY` or `INSERT` when small): copy rows from backup to the new table
3. **Finalize** (transaction): indexes, constraints, triggers, verify row counts, drop backup
4. **ANALYZE** (optional): refresh statistics when the source had `pg_statistic` rows

After every table is finished, dependent views are recreated in one transaction.

By default, step 4 runs when the source table has statistics (a prior `ANALYZE`). Disable with `--no-analyze-if-had-stats` on releases where auto-analyze already maintains stats.

## Resuming after interruption

If the process fails after Phase 1 (source renamed to `{table}{backup_suffix}`, empty
uncollocated shell created), re-run with the same flags:

```bash
python -m decolocate_tables \
  --dbname mydb --table public.orders \
  --work-dir /path/to/prior/work \
  --execute
```

The tool detects the in-progress state automatically: it skips Phase 1, truncates
any partial COPY data, copies from the backup table, then finishes indexes and views.

**Use the same `--work-dir` and `--backup-suffix` as the original run.** The work
directory's `manifest.json` and captured view DDL files are required to recreate
views (they are dropped in Phase 1 and no longer appear in the catalog).

## Downtime

Table recreation requires quiescing writes. Duration depends on table size. See
[Colocated tables documentation](../../../docs/content/stable/additional-features/colocation.md).

## Tests

```bash
cd python/yugabyte/decolocate_tables
pip install -r requirements.txt
./tests/run_tests.sh
```

Integration tests run only when `DECOLOCATE_INTEGRATION=1` and a cluster is reachable.
