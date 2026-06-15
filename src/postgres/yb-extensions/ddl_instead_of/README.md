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

-- From 1.4 to 1.5  (adds index storage estimation functions)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.5';

-- From any earlier version directly to 1.5
-- (PostgreSQL chains upgrade scripts automatically)
ALTER EXTENSION ddl_instead_of UPDATE TO '1.5';
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

**Example — automatically set `SPLIT INTO` tablets based on `estimate_index_size`:**

```sql
-- A CREATE INDEX handler that uses estimate_index_size to compute an
-- appropriate SPLIT INTO clause before the index is created.
CREATE OR REPLACE FUNCTION public.autosplit_index(
    command_tag text,
    stmt        text
) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    est     record;
    tablets integer;
BEGIN
    -- Skip if the statement already specifies SPLIT INTO
    IF stmt ~* 'SPLIT\s+INTO' THEN
        RETURN NULL;
    END IF;

    SELECT * INTO est
    FROM ddl_instead_of.estimate_index_size(stmt)
    LIMIT 1;

    -- Recommend at least 1 tablet; apply ceiling for the SQL clause
    tablets := GREATEST(1, ceil(est.recommended_tablets));

    -- Only add SPLIT INTO when more than one tablet is warranted
    IF tablets <= 1 THEN
        RETURN NULL;
    END IF;

    RETURN stmt || format(' SPLIT INTO %s TABLETS', tablets);
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

### `ddl_instead_of.parse_index_columns`

```sql
ddl_instead_of.parse_index_columns(statement text)
RETURNS TABLE (
    is_key       boolean,
    ordinal      integer,
    column_name  text,
    expression   text,
    ordering     text,
    nulls_first  boolean,
    where_clause text
)
```

Parses a raw `CREATE INDEX` statement and returns one row per key column
followed by one row per `INCLUDE` column.  Operates entirely on raw SQL text;
no catalog lookups or OID resolution are performed.

Granted to `PUBLIC`.

**Return columns:**

| Column | Description |
|---|---|
| `is_key` | `true` for key columns, `false` for `INCLUDE` columns |
| `ordinal` | 1-based position within the key or include list |
| `column_name` | Column name, or `NULL` for expression indexes |
| `expression` | Formatted expression for expression indexes (e.g. `lower(email)`), or `NULL` for plain column indexes |
| `ordering` | `'HASH'`, `'ASC'`, `'DESC'`, or `NULL` (database default) |
| `nulls_first` | `true` / `false` / `NULL` (database default) |
| `where_clause` | The `WHERE` predicate rendered as text (same on every row), or `NULL` when there is no `WHERE` clause |

**Notes:**

- The `ordering` column reflects the YugabyteDB-specific `HASH` sort direction
  as well as the standard `ASC` / `DESC`.  A value of `NULL` means no ordering
  was written explicitly.
- Returns zero rows for statements that are not `CREATE INDEX`.

**Example:**

```sql
SELECT is_key, ordinal, column_name, ordering, where_clause
FROM ddl_instead_of.parse_index_columns($$
    CREATE INDEX ON orders (tenant_id HASH, created_at ASC)
    INCLUDE (total_amount)
    WHERE status = 'active'
$$);

--  is_key | ordinal | column_name  | ordering | where_clause
-- --------+---------+--------------+----------+------------------
--  t      |       1 | tenant_id    | HASH     | (status = 'active')
--  t      |       2 | created_at   | ASC      | (status = 'active')
--  f      |       1 | total_amount |          | (status = 'active')
```

---

### `ddl_instead_of.parse_index_predicates`

```sql
ddl_instead_of.parse_index_predicates(statement text)
RETURNS TABLE (
    conjunction  text,
    column_name  text,
    operator     text,
    literal      text,
    literal_list text,
    negated      boolean
)
```

Parses the `WHERE` clause of a `CREATE INDEX` statement and returns one row
per **leaf condition**.  Boolean combinators (`AND`, `OR`, `NOT`) are not
emitted as rows; instead, each leaf is tagged with the conjunction of its
**parent** node so that PL/pgSQL can reconstruct the correct combination
formula.

Returns zero rows when there is no `WHERE` clause.  Granted to `PUBLIC`.

**Return columns:**

| Column | Description |
|---|---|
| `conjunction` | `'AND'` / `'OR'` / `'NONE'` (top-level single predicate) — indicates how this leaf combines with its siblings |
| `column_name` | Referenced column name, or `NULL` for expression predicates |
| `operator` | `'='`, `'<'`, `'>'`, `'<='`, `'>='`, `'<>'`, `'IS NULL'`, `'IS NOT NULL'`, `'IN'`, `'NOT IN'`, `'BETWEEN'`, `'NOT BETWEEN'`, `'LIKE'`, `'ILIKE'`, `'IS TRUE'`, `'IS FALSE'`, or `'UNKNOWN'` for unrecognised predicates |
| `literal` | Scalar constant value (e.g. `'active'`, `42`), or `NULL` for `IS NULL` / `IS NOT NULL` |
| `literal_list` | Comma-separated values for `IN` / `BETWEEN`, or `NULL` |
| `negated` | `true` when the predicate is inside a `NOT` expression |

**Supported predicate forms:**

| Pattern | `operator` | `literal` | `literal_list` |
|---|---|---|---|
| `col = 'x'` | `=` | `'x'` | `NULL` |
| `col IS NULL` | `IS NULL` | `NULL` | `NULL` |
| `col IS NOT NULL` | `IS NOT NULL` | `NULL` | `NULL` |
| `col IN ('a', 'b')` | `IN` | `NULL` | `'a', 'b'` |
| `col BETWEEN 1 AND 10` | `BETWEEN` | `NULL` | `1 AND 10` |
| `col LIKE 'x%'` | `LIKE` | `'x%'` | `NULL` |
| `col IS TRUE` | `IS TRUE` | `NULL` | `NULL` |
| `NOT col = 'x'` | `=` | `'x'` | `NULL` (with `negated = true`) |
| Complex expression | `UNKNOWN` | formatted text | `NULL` |

**Example:**

```sql
SELECT conjunction, column_name, operator, literal, negated
FROM ddl_instead_of.parse_index_predicates($$
    CREATE INDEX ON events (user_id)
    WHERE status = 'active' AND deleted_at IS NULL
$$);

--  conjunction | column_name | operator   | literal    | negated
-- -------------+-------------+------------+------------+---------
--  AND         | status      | =          | 'active'   | f
--  AND         | deleted_at  | IS NULL    |            | f
```

---

### `ddl_instead_of.estimate_column_selectivity`

```sql
ddl_instead_of.estimate_column_selectivity(
    p_table_oid    oid,
    p_col_name     text,
    p_operator     text,
    p_literal      text      DEFAULT NULL,
    p_literal_list text      DEFAULT NULL,
    p_negated      boolean   DEFAULT false
) RETURNS float8
```

Estimates the fraction of rows in a table that satisfy a single column
predicate using `pg_stats` Most Common Values data (Approach B — no `EXPLAIN`
on the live cluster).

Returns a value in the range `[0.0001, 1.0]`.  Returns `0.333` (PostgreSQL's
built-in default for unknown predicates) when statistics are unavailable or
the operator cannot be estimated.

Granted to `PUBLIC`.

**Parameters:**

| Parameter | Description |
|---|---|
| `p_table_oid` | OID of the base table (from `pg_class`) |
| `p_col_name` | Column name as it appears in `pg_stats` |
| `p_operator` | One of the operators returned by `parse_index_predicates` |
| `p_literal` | Formatted literal from `parse_index_predicates.literal` |
| `p_literal_list` | Formatted literal list from `parse_index_predicates.literal_list` |
| `p_negated` | Whether the predicate is inside a `NOT` |

**Estimation strategy by operator:**

| Operator | Strategy |
|---|---|
| `IS NULL` | `pg_stats.null_frac` |
| `IS NOT NULL` | `1 - null_frac` |
| `=` | MCV lookup; falls back to `(non_null_frac - MCV_total) / (n_distinct - MCV_count)` |
| `<>` | `non_null_frac` minus the MCV frequency of the matched value |
| `IN` | Sum of per-element MCV frequencies; falls back to `count / n_distinct` |
| `IS TRUE` / `IS FALSE` | MCV lookup for `'t'` or `'f'` |
| `<`, `<=`, `>`, `>=` | PostgreSQL's default `0.333` (histogram not yet used) |
| `BETWEEN` | Default `0.333` |
| `LIKE` / `ILIKE` | Fixed `0.2` |
| `UNKNOWN` | Default `0.333` |

**Note:** The literal values produced by `parse_index_predicates` are
automatically normalized (quotes and type casts stripped) before comparison
with `pg_stats.most_common_vals` elements.

---

### `ddl_instead_of.estimate_index_where_selectivity`

```sql
ddl_instead_of.estimate_index_where_selectivity(
    p_table_oid  oid,
    p_statement  text
) RETURNS float8
```

Combines the per-predicate selectivities for the `WHERE` clause of a
`CREATE INDEX` statement into a single combined selectivity value.

Returns `1.0` when the statement has no `WHERE` clause.  Granted to `PUBLIC`.

**Combination rules:**

- **`AND` leaves**: individual selectivities are multiplied.
- **`OR` leaves**: combined using inclusion-exclusion: `1 - Π(1 - sᵢ)`.
  After the OR block is resolved, its combined selectivity is multiplied into
  the AND product.
- **`UNKNOWN` or expression predicates**: use `0.333`.
- The result is clipped to `[0.0001, 1.0]`.

**Example:**

```sql
-- Table with 1,000,000 rows; status = 'active' has MCV frequency 0.35,
-- deleted_at IS NULL has null_frac = 0.02 → IS NOT NULL selectivity = 0.98.
SELECT ddl_instead_of.estimate_index_where_selectivity(
    'orders'::regclass,
    $$CREATE INDEX ON orders(user_id) WHERE status = 'active' AND deleted_at IS NOT NULL$$
);
-- → 0.35 × 0.98 = 0.343
```

---

### `ddl_instead_of.estimate_index_size`

```sql
ddl_instead_of.estimate_index_size(
    p_statement           text,
    p_target_tablet_bytes bigint  DEFAULT 10737418240,  -- 10 GiB
    p_ybctid_bytes        integer DEFAULT 24
) RETURNS TABLE (
    table_name           text,
    index_key_cols       text,
    index_include_cols   text,
    where_clause         text,
    base_rows            float8,
    where_selectivity    float8,
    effective_rows       float8,
    entry_bytes_raw      float8,
    entry_bytes_disk     float8,
    total_bytes          float8,
    recommended_tablets  float8,
    notes                text
)
```

Estimates the DocDB storage footprint and recommended tablet count for a
`CREATE INDEX` statement before it is executed.  Uses `pg_stats` heuristics
(Approach B) for `WHERE` predicate selectivity — no `EXPLAIN` is run on the
live cluster.  The tablet count is not rounded; callers apply their own
ceiling or threshold.

Granted to `PUBLIC`.

**DocDB storage model**

Each index entry is stored as a key-value pair in DocDB (RocksDB-based).
The size formula per entry:

```
Key  = Σ(key_col_avg_width + 1 type byte)
      + ybctid_bytes           ← base-table row identifier
      + 12 bytes HLC           ← hybrid logical clock timestamp
      + 8 bytes RKV metadata   ← RocksDB key overhead

Value = Σ(include_col_avg_width + 1 type byte)
       + 4 bytes value prefix  ← DocDB value header

Raw entry  = key + value
Disk entry = raw entry × 1.15  ← SST block / filter / bloom overhead
```

Average column widths come from `pg_stats.avg_width`, with fallback to a
type-family heuristic (e.g. `integer` → 4 bytes, `text` → 32 bytes,
expression columns → 32 bytes).

**Tablet recommendation**

```
effective_rows      = pg_class.reltuples × where_selectivity
total_bytes         = effective_rows × disk_entry_bytes
recommended_tablets = total_bytes / p_target_tablet_bytes
```

**Parameters:**

| Parameter | Description |
|---|---|
| `p_statement` | Raw `CREATE INDEX` SQL text |
| `p_target_tablet_bytes` | Target tablet size in bytes (default 10 GiB) |
| `p_ybctid_bytes` | Expected ybctid size; default 24 bytes (covers most primary key shapes) |

**Return columns:**

| Column | Description |
|---|---|
| `table_name` | Schema-qualified base table name |
| `index_key_cols` | Comma-separated key column list |
| `index_include_cols` | Comma-separated `INCLUDE` column list, or empty string |
| `where_clause` | `WHERE` predicate text, or empty string |
| `base_rows` | `pg_class.reltuples`; may be `-1` or `0` before first `ANALYZE` |
| `where_selectivity` | Estimated fraction of rows matching the `WHERE` clause |
| `effective_rows` | `base_rows × where_selectivity` |
| `entry_bytes_raw` | Estimated uncompressed bytes per index entry |
| `entry_bytes_disk` | `entry_bytes_raw × 1.15` |
| `total_bytes` | `effective_rows × entry_bytes_disk` |
| `recommended_tablets` | `total_bytes / p_target_tablet_bytes` (not rounded) |
| `notes` | Semicolon-separated diagnostic messages (e.g. missing statistics) |

**Notes:**

- `base_rows` is taken directly from `pg_class.reltuples`.  Run `ANALYZE` on
  the base table before calling this function for accurate results.
- For expression index columns (e.g. `lower(email)`), `avg_width` defaults to
  32 bytes and a note is added to the `notes` column.
- The function raises an exception if the base table does not exist.
- `recommended_tablets` is a continuous value.  A typical caller applies
  `GREATEST(1, ceil(recommended_tablets))` before constructing a `SPLIT INTO`
  clause.

**Example:**

```sql
SELECT
    table_name,
    index_key_cols,
    index_include_cols,
    where_clause,
    base_rows,
    round(where_selectivity::numeric, 4) AS selectivity,
    round(effective_rows::numeric)        AS eff_rows,
    round(entry_bytes_disk::numeric)      AS bytes_per_entry,
    round(total_bytes::numeric / 1e9, 2)  AS total_gb,
    recommended_tablets,
    notes
FROM ddl_instead_of.estimate_index_size($$
    CREATE INDEX ON orders (tenant_id HASH, created_at ASC)
    INCLUDE (total_amount)
    WHERE status = 'active' AND deleted_at IS NULL
$$);

--  table_name | index_key_cols               | index_include_cols | where_clause
-- ------------+------------------------------+--------------------+----------------------------------------
--  orders     | tenant_id, created_at        | total_amount       | (status = 'active' AND deleted_at IS NULL)
--
--  base_rows | selectivity | eff_rows | bytes_per_entry | total_gb | recommended_tablets | notes
-- -----------+-------------+----------+-----------------+----------+---------------------+-------
--  2000000   |      0.3430 |   686000 |              83 |     0.06 |            0.005461 |
```

**Typical use cases:**

- Pre-flight sizing: evaluate the storage impact of a proposed index before
  creating it on a production cluster.
- Automated `SPLIT INTO` selection: call from a `ddl_instead_of` handler on
  `CREATE INDEX` to inject an appropriate `SPLIT INTO N TABLETS` clause based
  on the estimated index size.
- Capacity planning: estimate total index storage across a set of planned
  migrations.

---

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
| 1.5 | Added index storage estimation suite: `parse_index_columns(statement)` (C SRF) decomposes a `CREATE INDEX` into key/include column rows with ordering and WHERE clause text; `parse_index_predicates(statement)` (C SRF) walks the WHERE clause AST and returns one row per leaf condition tagged with conjunction and negation; `estimate_column_selectivity(table_oid, col, op, …)` estimates per-predicate selectivity from `pg_stats` MCV lists (Approach B, no EXPLAIN); `estimate_index_where_selectivity(table_oid, statement)` combines AND/OR leaves using multiplication / inclusion-exclusion; `estimate_index_size(statement[, target_tablet_bytes[, ybctid_bytes]])` returns full DocDB storage estimates and an unrounded tablet count recommendation; all functions granted to PUBLIC |
| 1.4 | Added `table_signature(statement)` — computes an MD5 fingerprint of a `CREATE TABLE` structural definition; constraint names are excluded so tables with identical structure but different constraint names produce the same hash; constraints are sorted before hashing for order-independence; granted to PUBLIC |
| 1.3 | Added `parse_table_columns(statement)` returning per-column detail (ordinal position, name, type, nullability, default expression) and `parse_table_constraints(statement)` returning per-constraint detail (type, key columns with YugabyteDB ordering, check predicate, FK referenced table/columns, FK referential actions); type name formatter maps pg_catalog internal names to SQL standard names; expression formatter covers literals, type casts, function calls, operators, IN/BETWEEN, IS NULL, AND/OR/NOT; both granted to PUBLIC |
| 1.2 | Added `parse_command_info(command_tag, statement)` set-returning function that extracts `object_type`, `schema_name`, `object_name`, and `object_identity` from raw DDL text; covers 20+ statement types including multi-object DROP/TRUNCATE; granted to PUBLIC |
| 1.1 | Fixed `intercept_rule_lookup` index column order for range-sharding compatibility; fixed `add_rule` ON CONFLICT ambiguity; added GUC debug logging (`ddl_instead_of.debug`); fixed `tag_datum` use-after-free in `load_handler_oids`; added `pg_analyze_and_rewrite` step in `apply_rewrite` to handle `CALL` and other statements requiring semantic analysis; pass rewritten SQL text as `queryString` to downstream hooks |
| 1.0 | Initial release |
