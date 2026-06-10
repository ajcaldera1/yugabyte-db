# ddl_instead_of

A PostgreSQL/YugabyteDB extension that intercepts top-level DDL statements
**before** any event trigger fires, allowing a handler function to rewrite or
replace the statement entirely.

## Overview

PostgreSQL event triggers (`ddl_command_start`, `ddl_command_end`, `sql_drop`,
`table_rewrite`) observe DDL but cannot change it.  `ddl_instead_of` adds an
earlier interception point: a `ProcessUtility_hook` that runs before the
statement reaches the event-trigger machinery.  Each registered rule maps a
command tag (e.g. `CREATE TABLE`) to a handler function.  The handler receives
the original SQL text and can:

- **Return `NULL`** — let the original statement execute unchanged.
- **Return a new SQL string** — replace the statement with the returned text.

The returned SQL is fully parsed and semantically analyzed before execution, so
any valid statement — including `CALL`, additional DDL, or a stored-procedure
call — is accepted.

### Relation to event triggers

| Trigger point | Fires | Can rewrite |
|---|---|---|
| `ddl_instead_of` rules | Before everything | Yes |
| `ddl_command_start` | After rewrite | No |
| `ddl_command_end` | After execution | No |
| `sql_drop` | After execution | No |
| `table_rewrite` | During execution | No |

Because `ddl_instead_of` fires first, downstream event triggers receive the
**rewritten** command tag and statement, not the original.

## Requirements

- YugabyteDB >= 2.x (tested) or PostgreSQL 14+ (compatible)
- Superuser access to install the extension and register rules
- The extension must be loaded via `shared_preload_libraries` **or** created
  with `CREATE EXTENSION` in a session where the library is already loaded.

## Installation

### 1. Add to `shared_preload_libraries`

In `postgresql.conf` (or `yugabyte.conf`):

```
shared_preload_libraries = 'ddl_instead_of'
```

Restart the server after this change.

### 2. Create the extension in each target database

```sql
CREATE EXTENSION ddl_instead_of;
```

This creates the `ddl_instead_of` schema and its catalog table and management
functions.

### Upgrading

```sql
-- From 1.0 to 1.1
ALTER EXTENSION ddl_instead_of UPDATE TO '1.1';

-- From 1.1 to 1.2  (adds parse_command_info)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.2';

-- From 1.2 to 1.3  (adds parse_table_columns, parse_table_constraints)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.3';

-- From 1.3 to 1.4  (adds table_signature)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.4';

-- From any earlier version directly to 1.4
-- (PostgreSQL chains upgrade scripts automatically)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.4';
```

## Usage

### Step 1 — Write a handler function

A handler must have the signature:

```sql
function_name(command_tag text, statement_text text) RETURNS text
```

| Parameter | Description |
|---|---|
| `command_tag` | PostgreSQL command tag, e.g. `'CREATE TABLE'` |
| `statement_text` | The original SQL text of the statement |

Return `NULL` (or an empty string) to execute the original statement unchanged.
Return any non-empty string to replace it.

**Example — add a `SPLIT INTO` clause to every `CREATE TABLE` in a specific schema:**

```sql
CREATE OR REPLACE FUNCTION public.add_split_tablets(
    command_tag text,
    stmt        text
) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    info record;
BEGIN
    SELECT * INTO info
    FROM ddl_instead_of.parse_command_info(command_tag, stmt)
    LIMIT 1;

    -- Only augment tables in 'myschema' that don't already have SPLIT INTO
    IF info.schema_name = 'myschema' AND stmt !~* 'SPLIT\s+INTO' THEN
        RETURN stmt || ' SPLIT INTO 8 TABLETS';
    END IF;
    RETURN NULL;  -- pass through unchanged
END;
$$;
```

**Example — enforce a structural schema contract using `table_signature`:**

```sql
-- Reject any CREATE TABLE whose column/constraint layout does not match
-- a pre-approved "canonical" definition for that table name.
CREATE OR REPLACE FUNCTION public.enforce_schema_contract(
    command_tag text,
    stmt        text
) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    info      record;
    approved  text;
    actual    text;
BEGIN
    SELECT * INTO info
    FROM ddl_instead_of.parse_command_info(command_tag, stmt)
    LIMIT 1;

    -- Only enforce contracts for tables in the 'app' schema
    IF info.schema_name IS DISTINCT FROM 'app' THEN
        RETURN NULL;
    END IF;

    -- Look up the pre-approved signature for this table
    SELECT signature INTO approved
    FROM schema_contracts
    WHERE table_name = info.object_name;

    IF NOT FOUND THEN
        RETURN NULL;  -- no contract registered, allow
    END IF;

    actual := ddl_instead_of.table_signature(stmt);
    IF actual <> approved THEN
        RAISE EXCEPTION
            'table % does not match the approved schema contract '
            '(got signature %, expected %)',
            info.object_identity, actual, approved;
    END IF;

    RETURN NULL;  -- passes structural check, execute as-is
END;
$$;
```

**Example — redirect `TRUNCATE` to a soft-delete procedure:**

```sql
CREATE OR REPLACE FUNCTION public.truncate_to_soft_delete(
    command_tag text,
    stmt        text
) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    info record;
BEGIN
    -- Use parse_command_info to reliably extract schema and table name
    SELECT * INTO info
    FROM ddl_instead_of.parse_command_info(command_tag, stmt)
    LIMIT 1;

    -- Return a CALL statement; the original TRUNCATE is suppressed
    RETURN format('CALL soft_delete.archive(%L, %L)',
                  info.schema_name, info.object_name);
END;
$$;
```

### Step 2 — Register a rule

```sql
SELECT ddl_instead_of.add_rule(
    rule_name   => 'add_split',           -- unique name for this rule
    command_tag => 'CREATE TABLE',        -- command tag to intercept
    handler     => 'public.add_split_tablets(text,text)'::regprocedure,
    priority    => 100                    -- lower numbers run first (default 100)
);
```

Use `'*'` as `command_tag` to match every DDL statement.

Multiple rules can match the same command tag.  They run in ascending
`priority` order, then alphabetically by `rule_name`.  **Only the first rule
that returns a non-NULL string wins**; subsequent rules in the same batch are
skipped.

### Step 3 — Test it

```sql
-- With the add_split rule registered:
CREATE TABLE orders (id bigint PRIMARY KEY, amount numeric);
-- Actual DDL executed:
-- CREATE TABLE orders (id bigint PRIMARY KEY, amount numeric) SPLIT INTO 8 TABLETS
```

## Reference

### `ddl_instead_of.add_rule`

```sql
ddl_instead_of.add_rule(
    rule_name   name,
    command_tag text,
    handler     regprocedure,
    priority    integer DEFAULT 100
) RETURNS void
```

Registers or replaces a rule.  If a rule with the same `rule_name` already
exists it is updated in place (upsert).  Superuser only.

`handler` is validated at registration time: the referenced function must have
the exact signature `(text, text) RETURNS text` or an error is raised.

Common command tags:

| Tag | Statement |
|---|---|
| `CREATE TABLE` | `CREATE TABLE` |
| `ALTER TABLE` | `ALTER TABLE` |
| `DROP TABLE` | `DROP TABLE` |
| `CREATE INDEX` | `CREATE INDEX` |
| `TRUNCATE` | `TRUNCATE` |
| `CREATE SCHEMA` | `CREATE SCHEMA` |
| `CREATE FUNCTION` | `CREATE FUNCTION` or `CREATE OR REPLACE FUNCTION` |
| `CALL` | `CALL procedure_name(...)` |
| `*` | Any utility statement |

Run `SELECT DISTINCT command_tag FROM pg_event_trigger_ddl_commands()` inside
an event trigger to discover the exact tag for a specific statement.

### `ddl_instead_of.drop_rule`

```sql
ddl_instead_of.drop_rule(rule_name name) RETURNS void
```

Removes a rule.  Raises an error if the rule does not exist.  Superuser only.

### `ddl_instead_of.set_rule_enabled`

```sql
ddl_instead_of.set_rule_enabled(rule_name name, enabled boolean) RETURNS void
```

Enables or disables a rule without removing it.  Useful for temporarily
suspending interception.  Superuser only.

### `ddl_instead_of.validate_handler`

```sql
ddl_instead_of.validate_handler(handler regprocedure) RETURNS void
```

Checks that the referenced function has the required signature.  Called
internally by `add_rule`; also available for manual testing.  Superuser only.

### `ddl_instead_of.parse_command_info`

```sql
ddl_instead_of.parse_command_info(
    command_tag text,
    statement   text
) RETURNS TABLE(
    object_type      text,
    schema_name      text,
    object_name      text,
    object_identity  text
)
```

Parses a raw DDL statement and returns structured information about the primary
object(s) it addresses.  Intended to be called from within a handler function
to extract `schema_name` and `object_name` without brittle regex-based parsing.

Works pre-execution on raw SQL text; no catalog lookups are performed and no
OIDs are resolved.  The names returned are exactly as they appeared in the SQL
text.

Granted to `PUBLIC` because it only parses SQL text and performs no data or
catalog access.

**Return columns:**

| Column | Description |
|---|---|
| `object_type` | Human-readable object type, e.g. `'table'`, `'index'`, `'function'`, `'schema'` |
| `schema_name` | Schema qualifier, or `NULL` when inapplicable (schemas, extensions, roles) or when no explicit schema was written |
| `object_name` | Unqualified object name |
| `object_identity` | `schema_name.object_name` when schema is known, otherwise `object_name` |

**Supported statements:**

| Statement | Notes |
|---|---|
| `CREATE TABLE`, `CREATE TABLE AS`, `CREATE MATERIALIZED VIEW` | `object_type` is `'table'` or `'materialized view'` |
| `ALTER TABLE` / `ALTER INDEX` / `ALTER VIEW` / `ALTER MATERIALIZED VIEW` | `object_type` reflects what is being altered |
| `CREATE INDEX` | `schema_name` from the indexed table; `object_name` is the index name |
| `CREATE VIEW`, `REFRESH MATERIALIZED VIEW` | |
| `CREATE SEQUENCE`, `ALTER SEQUENCE` | |
| `CREATE SCHEMA` | `schema_name` is `NULL`; `object_name` is the schema name |
| `CREATE FUNCTION`, `CREATE PROCEDURE` | |
| `CREATE TYPE` (composite, enum, range), `CREATE DOMAIN` | |
| `CREATE TRIGGER`, `CREATE POLICY` | `schema_name` from the parent table |
| `CREATE EXTENSION`, `ALTER EXTENSION ... UPDATE` | `schema_name` is `NULL` |
| `DROP TABLE`, `DROP INDEX`, `DROP VIEW`, etc. | One row per dropped object |
| `TRUNCATE` | One row per listed relation |
| `CALL procedure(...)` | `object_type` is `'procedure'` |
| `ALTER ... RENAME TO` | Reports the pre-rename name |
| `ALTER ... SET SCHEMA` | |
| `COMMENT ON` | |
| All others | One row with `object_type` from `command_tag`; `schema_name` and `object_name` are `NULL` |

**Example:**

```sql
SELECT * FROM ddl_instead_of.parse_command_info(
    'CREATE TABLE',
    'CREATE TABLE myschema.orders (id bigint PRIMARY KEY)'
);

--  object_type | schema_name | object_name | object_identity
-- -------------+-------------+-------------+-----------------
--  table       | myschema    | orders      | myschema.orders

SELECT * FROM ddl_instead_of.parse_command_info(
    'DROP TABLE',
    'DROP TABLE public.foo, public.bar CASCADE'
);

--  object_type | schema_name | object_name | object_identity
-- -------------+-------------+-------------+-----------------
--  table       | public      | foo         | public.foo
--  table       | public      | bar         | public.bar
```

### `ddl_instead_of.parse_table_columns`

```sql
ddl_instead_of.parse_table_columns(statement text)
RETURNS TABLE(
    ordinal_position  integer,
    column_name       text,
    type_name         text,
    not_null          boolean,
    has_default       boolean,
    default_expr      text
)
```

Parses a raw `CREATE TABLE` statement and returns one row per column with
full structural detail.  Operates entirely on raw SQL text; no catalog lookups
or OID resolution are performed.

Granted to `PUBLIC`.

**Return columns:**

| Column | Description |
|---|---|
| `ordinal_position` | 1-based position of the column in the column list |
| `column_name` | Column name as written in the statement |
| `type_name` | Full SQL type string, e.g. `character varying(100)`, `numeric(10, 2)`, `integer[]`, `myschema.status_enum` |
| `not_null` | `true` when `NOT NULL`, `PRIMARY KEY`, `IDENTITY`, or `GENERATED` is present on the column |
| `has_default` | `true` when a `DEFAULT` clause is present |
| `default_expr` | The default expression rendered as text (e.g. `nextval('seq')`, `'active'::status_type`, `NOW()`), or `NULL` when `has_default` is `false` |

**Notes:**

- `LIKE` clauses and `INHERITS` references in `tableElts` are skipped (they
  are not `ColumnDef` nodes in the raw parse tree).
- `NOT NULL` imposed by a table-level `PRIMARY KEY` that names the column is
  **not** reflected here; only column-level nullability constraints are
  captured.
- Returns zero rows for statements that are not `CREATE TABLE` (no error is
  raised).

**Example:**

```sql
SELECT ordinal_position, column_name, type_name, not_null, has_default, default_expr
FROM ddl_instead_of.parse_table_columns($$
    CREATE TABLE orders (
        id          bigint          NOT NULL GENERATED ALWAYS AS IDENTITY,
        customer_id integer         NOT NULL,
        status      text            NOT NULL DEFAULT 'pending',
        amount      numeric(12, 2)  NOT NULL,
        tags        text[]
    )
$$);

--  ordinal_position | column_name | type_name     | not_null | has_default | default_expr
-- ------------------+-------------+---------------+----------+-------------+--------------
--  1                | id          | bigint        | t        | f           |
--  2                | customer_id | integer       | t        | f           |
--  3                | status      | text          | t        | t           | 'pending'
--  4                | amount      | numeric(12,2) | t        | f           |
--  5                | tags        | text[]        | f        | f           |
```

### `ddl_instead_of.parse_table_constraints`

```sql
ddl_instead_of.parse_table_constraints(statement text)
RETURNS TABLE(
    constraint_name  text,
    constraint_type  text,
    column_names     text,
    check_expr       text,
    ref_table        text,
    ref_columns      text,
    fk_on_delete     text,
    fk_on_update     text
)
```

Parses a raw `CREATE TABLE` statement and returns one row per constraint.
`NOT NULL` and `DEFAULT` are intentionally omitted; use `parse_table_columns`
for those.

Operates entirely on raw SQL text; no catalog lookups are performed.

Granted to `PUBLIC`.

**Return columns:**

| Column | Description |
|---|---|
| `constraint_name` | User-given constraint name, or `NULL` for unnamed constraints |
| `constraint_type` | `'PRIMARY KEY'`, `'UNIQUE'`, `'CHECK'`, `'FOREIGN KEY'`, or `'EXCLUDE'` |
| `column_names` | Comma-separated key column list.  For YugabyteDB `PRIMARY KEY` and `UNIQUE` constraints, ordering suffixes are included: `id HASH, created_at ASC` |
| `check_expr` | The `CHECK` predicate rendered as text (e.g. `(amount > 0)`), or `NULL` |
| `ref_table` | `[schema.]table` for `FOREIGN KEY`, or `NULL` |
| `ref_columns` | Referenced column list for `FOREIGN KEY`, or `NULL` (meaning: use the primary key of `ref_table`) |
| `fk_on_delete` | `FOREIGN KEY` `ON DELETE` action: `'NO ACTION'`, `'RESTRICT'`, `'CASCADE'`, `'SET NULL'`, `'SET DEFAULT'`, or `NULL` for non-FK constraints |
| `fk_on_update` | `FOREIGN KEY` `ON UPDATE` action (same values as `fk_on_delete`), or `NULL` |

**Notes:**

- Table-level constraints (written after the last column definition) are
  returned before column-level key constraints.
- Column-level `NOT NULL` and `DEFAULT` are not emitted as constraint rows;
  they appear in `parse_table_columns` instead.
- For a column-level `REFERENCES` clause, `column_names` is the declaring
  column name and `fk_attrs` is `NULL` in the raw parse tree (filled in later
  by the analyzer); the function reports the declaring column correctly.
- Returns zero rows for statements that are not `CREATE TABLE`.

**Example:**

```sql
SELECT constraint_name, constraint_type, column_names,
       check_expr, ref_table, ref_columns, fk_on_delete, fk_on_update
FROM ddl_instead_of.parse_table_constraints($$
    CREATE TABLE orders (
        id          bigint PRIMARY KEY,
        customer_id integer NOT NULL
            REFERENCES customers(id) ON DELETE CASCADE,
        amount      numeric(12, 2) NOT NULL,
        status      text NOT NULL,
        CONSTRAINT chk_amount   CHECK (amount > 0),
        CONSTRAINT uq_cust_stat UNIQUE (customer_id, status)
    )
$$);

--  constraint_name | constraint_type | column_names          | check_expr   | ref_table | ref_columns | fk_on_delete | fk_on_update
-- -----------------+-----------------+-----------------------+--------------+-----------+-------------+--------------+--------------
--                  | PRIMARY KEY     | id                    |              |           |             |              |
--  chk_amount      | CHECK           |                       | (amount > 0) |           |             |              |
--  uq_cust_stat    | UNIQUE          | customer_id, status   |              |           |             |              |
--                  | FOREIGN KEY     | customer_id           |              | customers | id          | CASCADE      | NO ACTION
```

### `ddl_instead_of.table_signature`

```sql
ddl_instead_of.table_signature(statement text) RETURNS text
```

Computes an MD5 fingerprint of the **structural definition** of a
`CREATE TABLE` statement.  The signature captures every aspect of the
table's shape that matters for compatibility: column positions, names,
types, nullability, defaults, and constraint definitions.

**Constraint names are deliberately excluded** so that two tables with
identical structure but differently-named constraints produce the same
signature.  This makes the function suitable for comparing tables across
environments where constraint names were generated differently (e.g.
`idx_12345_pkey` vs. `orders_pkey`).

Returns `NULL` only when the statement is invalid or contains no columns.
Granted to `PUBLIC`.

**Canonical format**

The MD5 is computed over a deterministic canonical text built as:

```
[column section]  GS  [constraint section]
```

where `GS` is ASCII 29 (group separator).  Within each section, rows are
separated by ASCII 30 (record separator) and fields by ASCII 31 (unit
separator).  These control characters cannot appear in SQL identifiers,
type names, or expressions, so the canonical text is unambiguous.

- **Columns** — ordered by `ordinal_position`.  Each row contains:
  `ordinal_position`, `column_name`, `type_name`, `not_null`, `has_default`,
  `default_expr`.
- **Constraints** — sorted by `(constraint_type, column_names, check_expr,
  ref_table, ref_columns)` so that declaration order does not affect the
  result.  Each row contains: `constraint_type`, `column_names`,
  `check_expr`, `ref_table`, `ref_columns`, `fk_on_delete`, `fk_on_update`.

**Example:**

```sql
-- Two tables with identical structure but different constraint names
-- produce the same signature.
SELECT
    ddl_instead_of.table_signature($$
        CREATE TABLE orders (
            id     bigint CONSTRAINT pk_orders  PRIMARY KEY,
            email  text   CONSTRAINT uq_email   UNIQUE,
            amount numeric(12,2) CONSTRAINT chk_amt CHECK (amount > 0)
        )
    $$)
    =
    ddl_instead_of.table_signature($$
        CREATE TABLE orders_replica (
            id     bigint PRIMARY KEY,
            email  text   UNIQUE,
            amount numeric(12,2) CHECK (amount > 0)
        )
    $$) AS signatures_match;
-- → true

-- Changing a column type breaks the signature.
SELECT
    ddl_instead_of.table_signature($$
        CREATE TABLE t (id bigint PRIMARY KEY)
    $$)
    <>
    ddl_instead_of.table_signature($$
        CREATE TABLE t (id integer PRIMARY KEY)
    $$) AS signatures_differ;
-- → true
```

**Typical use cases:**

- Schema drift detection: store the expected signature at deploy time and
  compare against the incoming `CREATE TABLE` statement in a handler.
- CI/CD schema validation: assert that migrated table DDL matches the
  reference definition, ignoring environment-specific constraint names.
- Audit logging: record the structural fingerprint alongside every
  `CREATE TABLE` event for forensic comparison.

### Catalog table

```sql
SELECT * FROM ddl_instead_of.intercept_rule;
```

| Column | Type | Description |
|---|---|---|
| `rule_name` | `name` | Primary key; unique rule identifier |
| `command_tag` | `text` | Command tag or `'*'` |
| `handler` | `regprocedure` | Handler function OID |
| `priority` | `integer` | Execution order (ascending) |
| `enabled` | `boolean` | Whether the rule is active |

## GUC parameters

| Parameter | Type | Default | Scope |
|---|---|---|---|
| `ddl_instead_of.debug` | `boolean` | `off` | `USERSET` |

Enable per-session to log rule matching and rewrite decisions at `LOG` level:

```sql
SET ddl_instead_of.debug = on;
```

Log messages report:
- Which command tag and statement fragment was intercepted
- How many handlers matched
- What each handler returned (`NULL` = pass-through, or the rewrite text)
- Whether a rewrite was applied or the original executed unchanged

## Security

- `add_rule`, `drop_rule`, `set_rule_enabled`, and `validate_handler` all
  require superuser privileges.
- The `intercept_rule` catalog table has all privileges revoked from `PUBLIC`.
- Handler functions are validated for the correct signature at registration
  time.
- The hook is skipped entirely during binary upgrades, YSQL upgrades, and
  `CREATE EXTENSION` execution to avoid interference with system operations.
- Interception is limited to `PROCESS_UTILITY_TOPLEVEL` context, so statements
  issued inside functions or SPI calls are not intercepted.
- A recursion guard prevents a rewritten statement from triggering the hook
  again (enforced via a depth counter).

## Behavior details

### Execution order with multiple matching rules

If rules `A` (priority 50) and `B` (priority 100) both match `CREATE TABLE`:

1. Rule A's handler runs.  If it returns a non-NULL string, that string becomes
   the new statement and rule B is **not** called.
2. If rule A returns `NULL`, rule B's handler runs.
3. If all handlers return `NULL`, the original statement executes unchanged.

### Rewritten statement requirements

The string returned by a handler must be a single, complete, syntactically
valid SQL statement.  Returning multiple statements separated by `;` raises an
error.  The statement is parsed and semantically analyzed (including function
lookup for `CALL`) before being passed to the execution engine.

### Interaction with `pg_stat_statements`

Downstream hooks such as `pg_stat_statements` receive the **rewritten** SQL
text as the query string, so the rewritten form appears in `pg_stat_statements`
rather than the original.

### `queryString` vs. statement text

The handler receives the statement text extracted from the original query string
using the statement location and length stored in `PlannedStmt`.  For
multi-statement query strings this is the individual statement, not the full
string.

## Limitations

- Only top-level utility statements are intercepted (`PROCESS_UTILITY_TOPLEVEL`
  context).  Statements called from within PL/pgSQL functions, `DO` blocks, or
  SPI are not intercepted.
- The handler cannot suppress execution entirely: returning `NULL` always passes
  the original statement through.  To suppress a statement, the handler must
  return a no-op replacement (e.g. `SELECT 1`).
- Rules are stored per-database.  Installing the extension in multiple databases
  requires registering rules in each one separately.
- The extension must be in `shared_preload_libraries`; loading it with
  `LOAD 'ddl_instead_of'` at runtime after the server has started will not
  intercept statements issued before the load.

## Building from source

The extension uses the standard PGXS build system.

```bash
# From the YugabyteDB source tree, after building the server:
cd src/postgres/yb-extensions/ddl_instead_of
source /path/to/build/postgres_build/yb-extensions/ddl_instead_of/env.sh
make install
```

Or via the YugabyteDB build wrapper (builds the full server plus the extension):

```bash
./yb_build.sh release daemons initdb --sj --skip-pg-parquet --no-odyssey --no-ybc
```

## Version history

| Version | Changes |
|---|---|
| 1.4 | Added `table_signature(statement)` — computes an MD5 fingerprint of a `CREATE TABLE` structural definition; constraint names are excluded so tables with identical structure but different constraint names produce the same hash; constraints are sorted before hashing for order-independence; granted to PUBLIC |
| 1.3 | Added `parse_table_columns(statement)` returning per-column detail (ordinal position, name, type, nullability, default expression) and `parse_table_constraints(statement)` returning per-constraint detail (type, key columns with YugabyteDB ordering, check predicate, FK referenced table/columns, FK referential actions); type name formatter maps pg_catalog internal names to SQL standard names; expression formatter covers literals, type casts, function calls, operators, IN/BETWEEN, IS NULL, AND/OR/NOT; both granted to PUBLIC |
| 1.2 | Added `parse_command_info(command_tag, statement)` set-returning function that extracts `object_type`, `schema_name`, `object_name`, and `object_identity` from raw DDL text; covers 20+ statement types including multi-object DROP/TRUNCATE; granted to PUBLIC |
| 1.1 | Fixed `intercept_rule_lookup` index column order for range-sharding compatibility; fixed `add_rule` ON CONFLICT ambiguity; added GUC debug logging (`ddl_instead_of.debug`); fixed `tag_datum` use-after-free in `load_handler_oids`; added `pg_analyze_and_rewrite` step in `apply_rewrite` to handle `CALL` and other statements requiring semantic analysis; pass rewritten SQL text as `queryString` to downstream hooks |
| 1.0 | Initial release |
