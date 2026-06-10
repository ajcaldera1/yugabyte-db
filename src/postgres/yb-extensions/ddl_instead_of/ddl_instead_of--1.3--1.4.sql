/* yb-extensions/ddl_instead_of/ddl_instead_of--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use 'ALTER EXTENSION ddl_instead_of UPDATE TO ''1.4''' to load this file. \quit

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
