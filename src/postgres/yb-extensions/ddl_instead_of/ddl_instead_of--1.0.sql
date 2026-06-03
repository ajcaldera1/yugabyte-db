/* yb-extensions/ddl_instead_of/ddl_instead_of--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use '''CREATE EXTENSION ddl_instead_of''' to load ddl_instead_of. \quit

CREATE TABLE intercept_rule (
	rule_name	name		PRIMARY KEY,
	command_tag	text		NOT NULL,
	handler		regprocedure	NOT NULL,
	priority	integer		NOT NULL DEFAULT 100,
	enabled		boolean		NOT NULL DEFAULT true
);

CREATE INDEX intercept_rule_lookup
	ON intercept_rule (enabled, command_tag, priority, rule_name);

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
	-- Fix #10: use current_setting('is_superuser') which is the established
	-- pattern in this codebase and does not rely on COALESCE over a GUC that
	-- is always present when connected to a backend.
	IF current_setting('is_superuser') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.add_rule: superuser privileges required';
	END IF;

	-- Validate handler signature before storing.  Raises ERROR on mismatch.
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

	-- Fix #9: report when the named rule does not exist.
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
