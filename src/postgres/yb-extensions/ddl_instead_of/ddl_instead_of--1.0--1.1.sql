/* yb-extensions/ddl_instead_of/ddl_instead_of--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use 'ALTER EXTENSION ddl_instead_of UPDATE TO ''1.1''' to load this file. \quit

-- Fix intercept_rule_lookup index column order.
-- The old index led with the boolean 'enabled', making it a near-useless
-- hash bucket (only two distinct values).  The new index leads with
-- command_tag (the equality predicate in load_handler_oids) so rows are
-- distributed by the most selective filter.
DROP INDEX ddl_instead_of.intercept_rule_lookup;
CREATE INDEX intercept_rule_lookup
	ON ddl_instead_of.intercept_rule (command_tag ASC, enabled ASC, priority ASC, rule_name ASC);

-- Fix add_rule: replace ON CONFLICT (rule_name) with
-- ON CONFLICT ON CONSTRAINT intercept_rule_pkey so PL/pgSQL can
-- unambiguously distinguish the parameter name from the column name.
CREATE OR REPLACE FUNCTION ddl_instead_of.add_rule(
	rule_name	name,
	command_tag	text,
	handler		regprocedure,
	priority	integer DEFAULT 100)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF current_setting('is_superuser') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.add_rule: superuser privileges required';
	END IF;

	PERFORM ddl_instead_of.validate_handler(add_rule.handler);

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
