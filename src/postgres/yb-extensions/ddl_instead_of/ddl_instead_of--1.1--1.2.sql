/* yb-extensions/ddl_instead_of/ddl_instead_of--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use 'ALTER EXTENSION ddl_instead_of UPDATE TO ''1.2''' to load this file. \quit

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
CREATE FUNCTION ddl_instead_of.parse_command_info(command_tag text, statement text)
RETURNS TABLE(
	object_type      text,
	schema_name      text,
	object_name      text,
	object_identity  text
)
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'ddl_instead_of_parse_command_info';

GRANT EXECUTE ON FUNCTION ddl_instead_of.parse_command_info(text, text) TO PUBLIC;
