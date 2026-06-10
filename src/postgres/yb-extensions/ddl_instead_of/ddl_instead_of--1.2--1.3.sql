/* yb-extensions/ddl_instead_of/ddl_instead_of--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use 'ALTER EXTENSION ddl_instead_of UPDATE TO ''1.3''' to load this file. \quit

/*
 * parse_table_columns: return per-column metadata from a CREATE TABLE statement.
 *
 * Parses the raw SQL text and returns one row per column:
 *
 *   ordinal_position  -- 1-based column order
 *   column_name       -- column name as written
 *   type_name         -- SQL type string, e.g. "character varying(100)", "integer[]"
 *   not_null          -- true when a NOT NULL, PRIMARY KEY, IDENTITY, or GENERATED
 *                        constraint is present on the column
 *   has_default       -- true when a DEFAULT clause is present
 *   default_expr      -- the default expression as text, or NULL
 *
 * Works pre-execution on raw SQL text; no catalog lookups are performed.
 * Skips LIKE clauses and inheritance specifications (those are not ColumnDef nodes).
 * Granted to PUBLIC because this function only parses SQL text.
 */
CREATE FUNCTION ddl_instead_of.parse_table_columns(statement text)
RETURNS TABLE(
    ordinal_position  integer,
    column_name       text,
    type_name         text,
    not_null          boolean,
    has_default       boolean,
    default_expr      text
)
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'ddl_instead_of_parse_table_columns';

GRANT EXECUTE ON FUNCTION ddl_instead_of.parse_table_columns(text) TO PUBLIC;

/*
 * parse_table_constraints: return per-constraint metadata from a CREATE TABLE
 * statement.
 *
 * Returns one row per constraint (PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY,
 * EXCLUDE).  NOT NULL and DEFAULT are intentionally omitted — use
 * parse_table_columns for those.
 *
 *   constraint_name  -- user-given name, or NULL if the constraint is unnamed
 *   constraint_type  -- 'PRIMARY KEY', 'UNIQUE', 'CHECK', 'FOREIGN KEY', 'EXCLUDE'
 *   column_names     -- comma-separated key column list; for YugabyteDB PRIMARY KEY
 *                       and UNIQUE constraints the list includes HASH/ASC/DESC suffixes
 *   check_expr       -- CHECK predicate as text, or NULL
 *   ref_table        -- [schema.]table for FOREIGN KEY, or NULL
 *   ref_columns      -- referenced column list for FK, or NULL (means use PK of ref_table)
 *   fk_on_delete     -- FK ON DELETE action: 'NO ACTION', 'RESTRICT', 'CASCADE',
 *                       'SET NULL', 'SET DEFAULT', or NULL for non-FK constraints
 *   fk_on_update     -- FK ON UPDATE action (same values), or NULL
 *
 * Table-level constraints are returned before column-level constraints.
 * Works pre-execution on raw SQL text; no catalog lookups are performed.
 * Granted to PUBLIC because this function only parses SQL text.
 */
CREATE FUNCTION ddl_instead_of.parse_table_constraints(statement text)
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
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'ddl_instead_of_parse_table_constraints';

GRANT EXECUTE ON FUNCTION ddl_instead_of.parse_table_constraints(text) TO PUBLIC;
