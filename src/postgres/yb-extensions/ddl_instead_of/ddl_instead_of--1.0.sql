/* yb-extensions/ddl_instead_of/ddl_instead_of--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use '''CREATE EXTENSION ddl_instead_of''' to load ddl_instead_of. \quit

CREATE TABLE intercept_rule (
	rule_name name PRIMARY KEY,
	command_tag text NOT NULL,
	handler regprocedure NOT NULL,
	priority integer NOT NULL DEFAULT 100,
	enabled boolean NOT NULL DEFAULT true
);

CREATE INDEX intercept_rule_lookup
	ON intercept_rule (enabled, command_tag, priority, rule_name);

REVOKE ALL ON TABLE intercept_rule FROM PUBLIC;

CREATE FUNCTION add_rule(rule_name name, command_tag text, handler regprocedure,
	priority integer DEFAULT 100)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF COALESCE(current_setting('is_superuser', true), 'off') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.add_rule: superuser privileges required';
	END IF;

	IF priority IS NULL THEN
		RAISE EXCEPTION 'ddl_instead_of.add_rule: priority must not be null';
	END IF;

	INSERT INTO intercept_rule AS r (rule_name, command_tag, handler, priority, enabled)
	VALUES (add_rule.rule_name, add_rule.command_tag, add_rule.handler, add_rule.priority, true)
	ON CONFLICT (rule_name) DO UPDATE
		SET command_tag = EXCLUDED.command_tag,
			handler = EXCLUDED.handler,
			priority = EXCLUDED.priority,
			enabled = true;
END;
$fn$;

CREATE FUNCTION drop_rule(rule_name name)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF COALESCE(current_setting('is_superuser', true), 'off') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.drop_rule: superuser privileges required';
	END IF;

	DELETE FROM intercept_rule r WHERE r.rule_name = drop_rule.rule_name;
END;
$fn$;

CREATE FUNCTION set_rule_enabled(rule_name name, enabled boolean)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, ddl_instead_of
AS $fn$
BEGIN
	IF COALESCE(current_setting('is_superuser', true), 'off') <> 'on' THEN
		RAISE EXCEPTION 'ddl_instead_of.set_rule_enabled: superuser privileges required';
	END IF;

	UPDATE intercept_rule r
	SET enabled = set_rule_enabled.enabled
	WHERE r.rule_name = set_rule_enabled.rule_name;

	IF NOT FOUND THEN
		RAISE EXCEPTION 'ddl_instead_of.set_rule_enabled: rule % not found', rule_name;
	END IF;
END;
$fn$;

REVOKE ALL ON FUNCTION add_rule(name, text, regprocedure, integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION drop_rule(name) FROM PUBLIC;
REVOKE ALL ON FUNCTION set_rule_enabled(name, boolean) FROM PUBLIC;
