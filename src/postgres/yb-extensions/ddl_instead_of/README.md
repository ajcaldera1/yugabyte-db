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

-- From 1.0 directly to 1.2  (PostgreSQL chains the upgrade scripts automatically)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.2';
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
| 1.2 | Added `parse_command_info(command_tag, statement)` set-returning function that extracts `object_type`, `schema_name`, `object_name`, and `object_identity` from raw DDL text; covers 20+ statement types including multi-object DROP/TRUNCATE; granted to PUBLIC |
| 1.1 | Fixed `intercept_rule_lookup` index column order for range-sharding compatibility; fixed `add_rule` ON CONFLICT ambiguity; added GUC debug logging (`ddl_instead_of.debug`); fixed `tag_datum` use-after-free in `load_handler_oids`; added `pg_analyze_and_rewrite` step in `apply_rewrite` to handle `CALL` and other statements requiring semantic analysis; pass rewritten SQL text as `queryString` to downstream hooks |
| 1.0 | Initial release |
