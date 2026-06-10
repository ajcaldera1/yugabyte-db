/* yb-extensions/ddl_instead_of/ddl_instead_of--1.4.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use 'CREATE EXTENSION ddl_instead_of' to load ddl_instead_of. \quit

CREATE TABLE intercept_rule (
	rule_name	name		PRIMARY KEY,
	command_tag	text		NOT NULL,
	handler		regprocedure	NOT NULL,
	priority	integer		NOT NULL DEFAULT 100,
	enabled		boolean		NOT NULL DEFAULT true
);

-- Range index leading on command_tag (the equality predicate in
-- load_handler_oids), then enabled, priority, rule_name so the scan
-- returns enabled=true rows in priority order without a sort step.
-- Using ASC throughout keeps the syntax compatible with vanilla PostgreSQL.
CREATE INDEX intercept_rule_lookup
	ON intercept_rule (command_tag ASC, enabled ASC, priority ASC, rule_name ASC);

REVOKE ALL ON TABLE intercept_rule FROM PUBLIC;

/*
 * C-level signature validator, called by add_rule and available for manual
 * use.  Raises ERROR when the function does not have signature
 * (text, text) RETURNS text.  Restricted to superusers.
 */
CREATE FUNCTION validate_handler(handler regprocedure)
RETURNS void
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'ddl_instead_of_validate_handler_sql';

REVOKE ALL ON FUNCTION validate_handler(regprocedure) FROM PUBLIC;

/*
 * add_rule: register or replace an intercept rule.
 *
 * handler must be a function with signature (text, text) RETURNS text.
 * Restricted to superusers.
 */
CREATE FUNCTION add_rule(rule_name name, command_tag text, handler regprocedure,
	priority integer DEFAULT 100)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF current_setting('is_superuser') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.add_rule: superuser privileges required';
	END IF;

	-- Validate handler signature before storing.  Raises ERROR on mismatch.
	PERFORM ddl_instead_of.validate_handler(add_rule.handler);

	-- ON CONFLICT ON CONSTRAINT avoids PL/pgSQL ambiguity between the
	-- rule_name parameter and the primary key column of the same name.
	INSERT INTO intercept_rule AS r
		(rule_name, command_tag, handler, priority, enabled)
	VALUES
		(add_rule.rule_name, add_rule.command_tag,
		 add_rule.handler, add_rule.priority, true)
	ON CONFLICT ON CONSTRAINT intercept_rule_pkey DO UPDATE
		SET command_tag = EXCLUDED.command_tag,
			handler     = EXCLUDED.handler,
			priority    = EXCLUDED.priority,
			enabled     = true;
END;
$fn$;

/*
 * drop_rule: remove a named intercept rule.
 *
 * Raises ERROR when the rule does not exist.  Restricted to superusers.
 */
CREATE FUNCTION drop_rule(rule_name name)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF current_setting('is_superuser') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.drop_rule: superuser privileges required';
	END IF;

	DELETE FROM intercept_rule r WHERE r.rule_name = drop_rule.rule_name;

	IF NOT FOUND THEN
		RAISE EXCEPTION 'ddl_instead_of.drop_rule: rule % not found', rule_name;
	END IF;
END;
$fn$;

/*
 * set_rule_enabled: enable or disable a named intercept rule.
 *
 * Raises ERROR when the rule does not exist.  Restricted to superusers.
 */
CREATE FUNCTION set_rule_enabled(rule_name name, enabled boolean)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF current_setting('is_superuser') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.set_rule_enabled: superuser privileges required';
	END IF;

	UPDATE intercept_rule r
	SET enabled = set_rule_enabled.enabled
	WHERE r.rule_name = set_rule_enabled.rule_name;

	IF NOT FOUND THEN
		RAISE EXCEPTION 'ddl_instead_of.set_rule_enabled: rule % not found',
			rule_name;
	END IF;
END;
$fn$;

REVOKE ALL ON FUNCTION add_rule(name, text, regprocedure, integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION drop_rule(name) FROM PUBLIC;
REVOKE ALL ON FUNCTION set_rule_enabled(name, boolean) FROM PUBLIC;

/*
 * parse_command_info: extract structured object information from a raw DDL
 * statement without requiring regex-based name parsing in handler functions.
 *
 * Returns a set of rows with:
 *   object_type     -- e.g. 'table', 'index', 'function', 'schema'
 *   schema_name     -- schema qualifier, or NULL when inapplicable
 *   object_name     -- unqualified object name
 *   object_identity -- [schema.]name combined string for convenience
 *
 * Works pre-execution on raw SQL text, so it is safe to call from within a
 * ddl_instead_of handler function.  Most DDL statements produce a single row;
 * multi-object statements (TRUNCATE a, b; DROP TABLE x, y) produce one row
 * per object.
 *
 * schema_name is NULL for object types that have no parent schema (schemas,
 * extensions, roles, tablespaces) or when no explicit schema was written in
 * the statement.
 *
 * Granted to PUBLIC because this function only parses SQL text and performs
 * no catalog lookups or data access.
 */
CREATE FUNCTION parse_command_info(command_tag text, statement text)
RETURNS TABLE(
	object_type      text,
	schema_name      text,
	object_name      text,
	object_identity  text
)
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'ddl_instead_of_parse_command_info';

GRANT EXECUTE ON FUNCTION parse_command_info(text, text) TO PUBLIC;

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
CREATE FUNCTION parse_table_columns(statement text)
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

GRANT EXECUTE ON FUNCTION parse_table_columns(text) TO PUBLIC;

/*
 * parse_table_constraints: return per-constraint metadata from a CREATE TABLE
 * statement.
 *
 * Returns one row per constraint (PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY,
 * EXCLUDE).  NOT NULL and DEFAULT are intentionally omitted -- use
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
CREATE FUNCTION parse_table_constraints(statement text)
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

GRANT EXECUTE ON FUNCTION parse_table_constraints(text) TO PUBLIC;

/*
 * table_signature(statement text) -> text
 *
 * Compute an MD5 fingerprint of the structural definition of a CREATE TABLE
 * statement.  The signature captures every aspect of the table's shape that
 * matters for compatibility:
 *
 *   - Each column's ordinal position, name, type, nullability, and default
 *     expression.
 *   - Each constraint's type, key columns, check predicate, referenced table
 *     and columns, and FK referential actions.
 *
 * Constraint *names* are deliberately excluded so that two tables with
 * identical structure but differently-named constraints produce the same
 * signature.
 *
 * Implementation notes
 * --------------------
 * The canonical text is built from two sections separated by ASCII GS
 * (chr(29), group separator):
 *
 *   [column section]  chr(29)  [constraint section]
 *
 * Within each section, records are separated by ASCII RS (chr(30), record
 * separator) and fields within a record by ASCII US (chr(31), unit separator).
 * These control characters cannot appear in SQL identifiers, type names, or
 * expressions, so the canonical text is unambiguous.
 *
 * Columns are ordered by ordinal position (the natural parse order).
 * Constraints are sorted by (type, column_names, check_expr, ref_table,
 * ref_columns) so that two tables whose constraints are declared in a
 * different order still produce the same signature.
 *
 * A NULL result is returned only when the statement contains no columns at
 * all (e.g. a completely empty tableElts list, which PostgreSQL would reject
 * at execution time anyway).
 *
 * Granted to PUBLIC: this function only calls parse_table_columns and
 * parse_table_constraints, both of which are already PUBLIC.
 */
CREATE FUNCTION ddl_instead_of.table_signature(statement text)
RETURNS text
LANGUAGE sql
STABLE STRICT
AS $$
    SELECT md5(

        /*
         * Column section
         *
         * One record per column, ordered by ordinal_position.
         * Fields (US-separated):
         *   ordinal_position | column_name | type_name
         *   | not_null | has_default | default_expr
         *
         * Empty string is used in place of a NULL default_expr so that a
         * column with no default and one with DEFAULT NULL can be told apart
         * (has_default covers the latter case, but belt-and-suspenders).
         */
        COALESCE(
            (
                SELECT string_agg(
                           ordinal_position::text
                        || chr(31) || column_name
                        || chr(31) || type_name
                        || chr(31) || not_null::text
                        || chr(31) || has_default::text
                        || chr(31) || COALESCE(default_expr, ''),
                           chr(30)
                           ORDER BY ordinal_position
                       )
                FROM ddl_instead_of.parse_table_columns($1)
            ),
            ''
        )

        /* Section separator */
        || chr(29)

        /*
         * Constraint section
         *
         * One record per constraint, sorted for order-independence.
         * constraint_name is intentionally omitted.
         * Fields (US-separated):
         *   constraint_type | column_names | check_expr
         *   | ref_table | ref_columns | fk_on_delete | fk_on_update
         *
         * NULL fields are replaced with empty string; they are still
         * distinguishable via constraint_type (e.g. fk_on_delete is only
         * meaningful for FOREIGN KEY constraints).
         */
        || COALESCE(
            (
                SELECT string_agg(
                           constraint_type
                        || chr(31) || COALESCE(column_names,  '')
                        || chr(31) || COALESCE(check_expr,    '')
                        || chr(31) || COALESCE(ref_table,     '')
                        || chr(31) || COALESCE(ref_columns,   '')
                        || chr(31) || COALESCE(fk_on_delete,  '')
                        || chr(31) || COALESCE(fk_on_update,  ''),
                           chr(30)
                           ORDER BY
                               constraint_type,
                               COALESCE(column_names,  ''),
                               COALESCE(check_expr,    ''),
                               COALESCE(ref_table,     ''),
                               COALESCE(ref_columns,   '')
                       )
                FROM ddl_instead_of.parse_table_constraints($1)
            ),
            ''
        )
    );
$$;

GRANT EXECUTE ON FUNCTION ddl_instead_of.table_signature(text) TO PUBLIC;
