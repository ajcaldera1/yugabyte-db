#!/usr/bin/env bash
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

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
YB_PYTHON="$(cd "${PKG_DIR}/../.." && pwd)"

export PYTHONPATH="${YB_PYTHON}:${PYTHONPATH:-}"

echo "Running unit tests..."
python3 -m unittest discover -s "${SCRIPT_DIR}" -p 'test_*.py' -v

if [[ "${DECOLOCATE_INTEGRATION:-}" != "1" ]]; then
  echo "Skipping integration tests (set DECOLOCATE_INTEGRATION=1 to enable)."
  exit 0
fi

HOST="${DECOLOCATE_HOST:-127.0.0.1}"
PORT="${DECOLOCATE_PORT:-5433}"
USER="${DECOLOCATE_USER:-yugabyte}"
DB="${DECOLOCATE_DB:-decolocate_test}"
YSQL="${DECOLOCATE_YSQL:-ysqlsh}"
YSQL_DUMP="${DECOLOCATE_YSQL_DUMP:-ysql_dump}"

echo "Running integration tests against ${HOST}:${PORT}/${DB}..."

"${YSQL}" -h "${HOST}" -p "${PORT}" -U "${USER}" -d postgres -c \
  "SELECT 1 FROM pg_database WHERE datname = '${DB}'" | grep -q 1 || \
  "${YSQL}" -h "${HOST}" -p "${PORT}" -U "${USER}" -d postgres -c \
    "CREATE DATABASE ${DB} WITH COLOCATION = true"

"${YSQL}" -h "${HOST}" -p "${PORT}" -U "${USER}" -d "${DB}" -f \
  "${SCRIPT_DIR}/fixtures/integration_setup.sql"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

python3 -m decolocate_tables \
  --host "${HOST}" --port "${PORT}" --dbname "${DB}" --user "${USER}" \
  --ysql-dump "${YSQL_DUMP}" \
  --work-dir "${WORK_DIR}" \
  --table public.decolocate_base

test -f "${WORK_DIR}/manifest.json"

python3 -m decolocate_tables \
  --host "${HOST}" --port "${PORT}" --dbname "${DB}" --user "${USER}" \
  --ysql-dump "${YSQL_DUMP}" \
  --work-dir "${WORK_DIR}" \
  --table public.decolocate_base \
  --execute

IS_COLO=$("${YSQL}" -h "${HOST}" -p "${PORT}" -U "${USER}" -d "${DB}" -t -c \
  "SELECT is_colocated FROM yb_table_properties('public.decolocate_base'::regclass)")

if echo "${IS_COLO}" | grep -qi f; then
  echo "decolocate_base is uncollocated."
else
  echo "ERROR: decolocate_base is still colocated: ${IS_COLO}"
  exit 1
fi

ROWS=$("${YSQL}" -h "${HOST}" -p "${PORT}" -U "${USER}" -d "${DB}" -t -c \
  "SELECT count(*) FROM decolocate_v2")

if [[ "${ROWS}" -eq 3 ]]; then
  echo "decolocate_v2 row count OK (3 rows)."
else
  echo "ERROR: decolocate_v2 expected 3 rows, got ${ROWS}"
  exit 1
fi

echo "Integration tests passed."
