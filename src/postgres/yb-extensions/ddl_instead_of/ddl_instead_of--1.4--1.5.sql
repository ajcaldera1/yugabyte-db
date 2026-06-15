-- ddl_instead_of--1.4--1.5.sql
-- Upgrade from 1.4 to 1.5: add index storage estimation functions.
--
-- New functions
--   parse_index_columns(statement text)
--   parse_index_predicates(statement text)
--   _normalize_literal(lit text)
--   estimate_column_selectivity(...)
--   estimate_index_where_selectivity(table_oid, statement)
--   _estimate_avg_col_width(table_oid, col_name)
--   estimate_index_size(statement [, target_tablet_bytes])

-- -------------------------------------------------------------------------
-- C-backed SRFs
-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of.parse_index_columns(statement text)
RETURNS TABLE (
    is_key       boolean,
    ordinal      integer,
    column_name  text,
    expression   text,
    ordering     text,
    nulls_first  boolean,
    where_clause text
)
LANGUAGE C STRICT STABLE
AS 'MODULE_PATHNAME', 'ddl_instead_of_parse_index_columns';

COMMENT ON FUNCTION ddl_instead_of.parse_index_columns(text) IS
'Parse a CREATE INDEX statement and return one row per key column followed
by one row per INCLUDE column.  is_key = true for key columns, false for
INCLUDE columns.  where_clause (same on every row) contains the raw WHERE
predicate text, or NULL when there is no WHERE clause.';

GRANT EXECUTE ON FUNCTION ddl_instead_of.parse_index_columns(text) TO PUBLIC;

-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of.parse_index_predicates(statement text)
RETURNS TABLE (
    conjunction  text,
    column_name  text,
    operator     text,
    literal      text,
    literal_list text,
    negated      boolean
)
LANGUAGE C STRICT STABLE
AS 'MODULE_PATHNAME', 'ddl_instead_of_parse_index_predicates';

COMMENT ON FUNCTION ddl_instead_of.parse_index_predicates(text) IS
'Parse the WHERE clause of a CREATE INDEX statement and return one row per
leaf condition.  conjunction is AND / OR / NONE (top-level single predicate).
literal holds the scalar constant; literal_list holds comma-separated values
for IN and BETWEEN operators.  Returns zero rows when there is no WHERE clause.';

GRANT EXECUTE ON FUNCTION ddl_instead_of.parse_index_predicates(text) TO PUBLIC;

-- -------------------------------------------------------------------------
-- Internal helper: normalise a literal string produced by the C formatter
-- so it can be compared against pg_stats.most_common_vals elements.
--
-- The C formatter (ddlii_format_expr) wraps string constants in single quotes
-- ('active') and may append type casts ('active'::status_enum).  pg_stats
-- stores MCV elements in the column's native text representation without
-- surrounding quotes.
-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of._normalize_literal(lit text)
RETURNS text
LANGUAGE plpgsql IMMUTABLE STRICT
AS $$
DECLARE
    v text := lit;
BEGIN
    -- Remove trailing type cast (e.g. ::text, ::my_schema.my_type)
    v := regexp_replace(v, '::[A-Za-z_][A-Za-z0-9_.]*$', '');

    -- Strip surrounding single quotes and unescape doubled quotes
    IF v ~ E'^''.*''$' THEN
        v := substring(v, 2, length(v) - 2);
        v := replace(v, '''''', '''');
    END IF;

    -- Normalise boolean representations to the pg_stats storage form
    CASE lower(v)
        WHEN 'true',  'yes', 'on',  '1' THEN RETURN 't';
        WHEN 'false', 'no',  'off', '0' THEN RETURN 'f';
        ELSE NULL;
    END CASE;

    RETURN v;
END;
$$;

REVOKE ALL ON FUNCTION ddl_instead_of._normalize_literal(text) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION ddl_instead_of._normalize_literal(text) TO PUBLIC;

-- -------------------------------------------------------------------------
-- Internal helper: estimate the average on-disk width of a single column.
--
-- Tries pg_stats.avg_width first, then falls back to pg_attribute.atttypmod
-- and finally to a type-family heuristic.
-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of._estimate_avg_col_width(
    p_table_oid  oid,
    p_col_name   text
) RETURNS float8
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    v_width    float8;
    v_typname  text;
    v_typmod   integer;
BEGIN
    -- 1. pg_stats avg_width (most reliable when ANALYZE has been run)
    SELECT avg_width::float8
    INTO   v_width
    FROM   pg_stats s
    JOIN   pg_class c  ON c.relname  = s.tablename
    JOIN   pg_namespace n ON n.oid   = c.relnamespace
                         AND n.nspname = s.schemaname
    WHERE  c.oid       = p_table_oid
      AND  s.attname   = p_col_name;

    IF FOUND AND v_width IS NOT NULL THEN
        RETURN v_width;
    END IF;

    -- 2. Type-based heuristic via pg_attribute
    SELECT t.typname, a.atttypmod
    INTO   v_typname, v_typmod
    FROM   pg_attribute a
    JOIN   pg_type      t ON t.oid = a.atttypid
    WHERE  a.attrelid = p_table_oid
      AND  a.attname  = p_col_name
      AND  a.attnum   > 0
      AND  NOT a.attisdropped;

    IF FOUND THEN
        CASE
            -- Fixed-width numeric types
            WHEN v_typname IN ('bool', 'char', '"char"')               THEN RETURN 1;
            WHEN v_typname IN ('int2', 'int4', 'float4', 'date')       THEN RETURN 4;
            WHEN v_typname IN ('int8', 'float8', 'timestamp',
                               'timestamptz', 'numeric', 'money',
                               'interval', 'time', 'timetz', 'oid',
                               'xid', 'cid')                           THEN RETURN 8;
            WHEN v_typname IN ('uuid')                                 THEN RETURN 16;
            -- Variable-length: use declared length when available
            WHEN v_typname IN ('bpchar', 'varchar') AND v_typmod > 4  THEN
                RETURN LEAST(v_typmod - 4, 128)::float8;
            -- Text / bytea: reasonable default
            WHEN v_typname IN ('text', 'varchar', 'bpchar', 'bytea',
                               'name', 'json', 'jsonb', 'xml')         THEN RETURN 32;
            ELSE RETURN 24;  -- catch-all for arrays, composites, etc.
        END CASE;
    END IF;

    -- 3. Last resort
    RETURN 24;
END;
$$;

REVOKE ALL ON FUNCTION ddl_instead_of._estimate_avg_col_width(oid, text) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION ddl_instead_of._estimate_avg_col_width(oid, text) TO PUBLIC;

-- -------------------------------------------------------------------------
-- estimate_column_selectivity
--
-- Estimate the fraction of rows in a table that satisfy a single column
-- predicate, using pg_stats (Approach B: no EXPLAIN on the live cluster).
--
-- Selectivity values are clipped to [0.0001, 1.0].
-- Returns 0.333 (PostgreSQL's default for unknown predicates) when
-- statistics are unavailable or the operator is not handled.
-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of.estimate_column_selectivity(
    p_table_oid   oid,
    p_col_name    text,
    p_operator    text,
    p_literal     text      DEFAULT NULL,
    p_literal_list text     DEFAULT NULL,
    p_negated     boolean   DEFAULT false
) RETURNS float8
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    v_null_frac     float8;
    v_n_distinct    float8;
    v_mcv_vals      text[];
    v_mcv_freqs     float8[];
    v_non_null_frac float8;
    v_selectivity   float8;
    v_total_mcv     float8;
    v_compare       text;
    v_i             integer;
    v_in_elems      text[];
    v_elem          text;
    v_elem_sel      float8;
    v_matched_freq  float8;
BEGIN
    -- Fetch statistics once
    SELECT s.null_frac,
           s.n_distinct,
           (s.most_common_vals::text)::text[],
           s.most_common_freqs
    INTO   v_null_frac, v_n_distinct, v_mcv_vals, v_mcv_freqs
    FROM   pg_stats s
    JOIN   pg_class c ON c.relname = s.tablename
    JOIN   pg_namespace n ON n.oid = c.relnamespace
                          AND n.nspname = s.schemaname
    WHERE  c.oid = p_table_oid
      AND  s.attname = p_col_name;

    IF NOT FOUND THEN
        RETURN 0.333;  -- no statistics: use PostgreSQL's default
    END IF;

    v_null_frac     := COALESCE(v_null_frac, 0.0);
    v_non_null_frac := 1.0 - v_null_frac;

    CASE p_operator

        -- ---- NULL tests ------------------------------------------------
        WHEN 'IS NULL' THEN
            v_selectivity := v_null_frac;

        WHEN 'IS NOT NULL' THEN
            v_selectivity := v_non_null_frac;

        -- ---- Equality --------------------------------------------------
        WHEN '=' THEN
            v_compare := ddl_instead_of._normalize_literal(p_literal);
            v_total_mcv  := 0.0;
            v_matched_freq := NULL;

            IF v_mcv_vals IS NOT NULL THEN
                FOR v_i IN 1 .. array_length(v_mcv_vals, 1) LOOP
                    v_total_mcv := v_total_mcv + v_mcv_freqs[v_i];
                    IF v_mcv_vals[v_i] = v_compare AND v_matched_freq IS NULL THEN
                        v_matched_freq := v_mcv_freqs[v_i];
                    END IF;
                END LOOP;
            END IF;

            IF v_matched_freq IS NOT NULL THEN
                v_selectivity := v_matched_freq;
            ELSIF ABS(COALESCE(v_n_distinct, 0)) > array_length(COALESCE(v_mcv_vals, ARRAY[]::text[]), 1) THEN
                -- Estimate from the non-MCV, non-null fraction
                v_selectivity :=
                    (v_non_null_frac - v_total_mcv)
                    / NULLIF(ABS(v_n_distinct) - COALESCE(array_length(v_mcv_vals, 1), 0), 0);
            ELSE
                v_selectivity := 1.0 / NULLIF(ABS(v_n_distinct), 0);
            END IF;

            v_selectivity := GREATEST(0.0, LEAST(v_non_null_frac, COALESCE(v_selectivity, 0.005)));

        -- ---- Inequality ------------------------------------------------
        WHEN '<>' THEN
            v_compare := ddl_instead_of._normalize_literal(p_literal);
            v_matched_freq := 0.0;
            IF v_mcv_vals IS NOT NULL THEN
                FOR v_i IN 1 .. array_length(v_mcv_vals, 1) LOOP
                    IF v_mcv_vals[v_i] = v_compare THEN
                        v_matched_freq := v_mcv_freqs[v_i];
                        EXIT;
                    END IF;
                END LOOP;
            END IF;
            v_selectivity := v_non_null_frac - v_matched_freq;

        -- ---- IN list ---------------------------------------------------
        WHEN 'IN' THEN
            v_in_elems := string_to_array(p_literal_list, ', ');
            v_selectivity := 0.0;

            IF v_in_elems IS NOT NULL THEN
                FOREACH v_elem IN ARRAY v_in_elems LOOP
                    v_compare  := ddl_instead_of._normalize_literal(v_elem);
                    v_elem_sel := 0.0;

                    IF v_mcv_vals IS NOT NULL THEN
                        FOR v_i IN 1 .. array_length(v_mcv_vals, 1) LOOP
                            IF v_mcv_vals[v_i] = v_compare THEN
                                v_elem_sel := v_mcv_freqs[v_i];
                                EXIT;
                            END IF;
                        END LOOP;
                    END IF;

                    IF v_elem_sel = 0.0 THEN
                        v_elem_sel := 1.0 / NULLIF(ABS(v_n_distinct), 0);
                    END IF;
                    v_selectivity := v_selectivity + COALESCE(v_elem_sel, 0.0);
                END LOOP;
            END IF;

            v_selectivity := LEAST(v_non_null_frac, COALESCE(v_selectivity, 0.333));

        WHEN 'NOT IN' THEN
            -- Treat as 1 - IN selectivity (approximate)
            v_in_elems := string_to_array(p_literal_list, ', ');
            v_selectivity := v_non_null_frac;
            IF v_in_elems IS NOT NULL THEN
                FOREACH v_elem IN ARRAY v_in_elems LOOP
                    v_compare := ddl_instead_of._normalize_literal(v_elem);
                    IF v_mcv_vals IS NOT NULL THEN
                        FOR v_i IN 1 .. array_length(v_mcv_vals, 1) LOOP
                            IF v_mcv_vals[v_i] = v_compare THEN
                                v_selectivity := v_selectivity - v_mcv_freqs[v_i];
                                EXIT;
                            END IF;
                        END LOOP;
                    ELSE
                        v_selectivity := v_selectivity - 1.0 / NULLIF(ABS(v_n_distinct), 0);
                    END IF;
                END LOOP;
            END IF;

        -- ---- Boolean / enum tests --------------------------------------
        WHEN 'IS TRUE', 'IS NOT FALSE' THEN
            v_compare := 't';
            v_matched_freq := 0.0;
            IF v_mcv_vals IS NOT NULL THEN
                FOR v_i IN 1 .. array_length(v_mcv_vals, 1) LOOP
                    IF v_mcv_vals[v_i] = v_compare THEN
                        v_matched_freq := v_mcv_freqs[v_i]; EXIT;
                    END IF;
                END LOOP;
            END IF;
            v_selectivity := COALESCE(v_matched_freq, v_non_null_frac / 2.0);

        WHEN 'IS FALSE', 'IS NOT TRUE' THEN
            v_compare := 'f';
            v_matched_freq := 0.0;
            IF v_mcv_vals IS NOT NULL THEN
                FOR v_i IN 1 .. array_length(v_mcv_vals, 1) LOOP
                    IF v_mcv_vals[v_i] = v_compare THEN
                        v_matched_freq := v_mcv_freqs[v_i]; EXIT;
                    END IF;
                END LOOP;
            END IF;
            v_selectivity := COALESCE(v_matched_freq, v_non_null_frac / 2.0);

        -- ---- Range predicates (histogram not yet used) -----------------
        WHEN '<', '<=', '>', '>=' THEN
            -- Without histogram interpolation, use the same default as PG
            v_selectivity := 0.333;

        WHEN 'BETWEEN', 'BETWEEN SYMMETRIC' THEN
            v_selectivity := 0.333;

        WHEN 'NOT BETWEEN', 'NOT BETWEEN SYMMETRIC' THEN
            v_selectivity := 0.667;

        -- ---- Pattern matching ------------------------------------------
        WHEN 'LIKE', 'ILIKE' THEN
            v_selectivity := 0.2;

        WHEN 'NOT LIKE', 'NOT ILIKE' THEN
            v_selectivity := 0.8;

        -- ---- Fallback --------------------------------------------------
        ELSE
            v_selectivity := 0.333;

    END CASE;

    -- Apply NOT if the predicate was wrapped in a NOT expression and the
    -- operator has not already accounted for negation internally.
    IF p_negated AND p_operator NOT IN
       ('IS NULL', 'IS NOT NULL', '<>', 'NOT IN', 'IS NOT TRUE', 'IS NOT FALSE',
        'NOT LIKE', 'NOT ILIKE', 'NOT BETWEEN', 'NOT BETWEEN SYMMETRIC') THEN
        v_selectivity := v_non_null_frac - v_selectivity;
    END IF;

    RETURN GREATEST(0.0001, LEAST(1.0, COALESCE(v_selectivity, 0.333)));
END;
$$;

COMMENT ON FUNCTION ddl_instead_of.estimate_column_selectivity(oid,text,text,text,text,boolean) IS
'Estimate the fraction of rows matching a single column predicate using
pg_stats MCV data (Approach B, no EXPLAIN).  Range predicates fall back
to the PostgreSQL default of 0.333.';

REVOKE ALL ON FUNCTION ddl_instead_of.estimate_column_selectivity(oid,text,text,text,text,boolean) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION ddl_instead_of.estimate_column_selectivity(oid,text,text,text,text,boolean) TO PUBLIC;

-- -------------------------------------------------------------------------
-- estimate_index_where_selectivity
--
-- Combine the per-predicate selectivities for the WHERE clause of a
-- CREATE INDEX statement into a single combined selectivity value:
--
--   * AND leaves: multiply individual selectivities.
--   * OR  leaves: apply  1 - PRODUCT(1 - s_i)  (inclusion-exclusion).
--   * A mixed AND/OR tree is handled in two passes:
--     the OR leaves are first combined with the OR formula, and the
--     resulting OR-block selectivity is multiplied with the AND leaves.
--   * UNKNOWN or expression predicates: use PostgreSQL's default 0.333.
--   * Returns 1.0 when there is no WHERE clause (no rows from
--     parse_index_predicates).
-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of.estimate_index_where_selectivity(
    p_table_oid  oid,
    p_statement  text
) RETURNS float8
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    r                record;
    v_and_sel        float8 := 1.0;
    v_or_complement  float8 := 1.0;   -- PRODUCT(1 - s_i) for OR leaves
    v_has_or         boolean := false;
    v_has_predicate  boolean := false;
    v_leaf_sel       float8;
BEGIN
    FOR r IN
        SELECT conjunction, column_name, operator, literal, literal_list, negated
        FROM   ddl_instead_of.parse_index_predicates(p_statement)
    LOOP
        v_has_predicate := true;

        IF r.operator = 'UNKNOWN' OR r.column_name IS NULL THEN
            -- Cannot estimate; use PostgreSQL's default
            v_leaf_sel := 0.333;
        ELSE
            v_leaf_sel := ddl_instead_of.estimate_column_selectivity(
                p_table_oid,
                r.column_name,
                r.operator,
                r.literal,
                r.literal_list,
                r.negated
            );
        END IF;

        IF r.conjunction = 'OR' THEN
            v_has_or       := true;
            v_or_complement := v_or_complement * (1.0 - v_leaf_sel);
        ELSE
            -- 'AND' or 'NONE': multiply into the AND accumulator
            v_and_sel := v_and_sel * v_leaf_sel;
        END IF;
    END LOOP;

    IF NOT v_has_predicate THEN
        RETURN 1.0;  -- no WHERE clause
    END IF;

    -- Combine OR block with AND block
    IF v_has_or THEN
        v_and_sel := v_and_sel * (1.0 - v_or_complement);
    END IF;

    RETURN GREATEST(0.0001, LEAST(1.0, v_and_sel));
END;
$$;

COMMENT ON FUNCTION ddl_instead_of.estimate_index_where_selectivity(oid, text) IS
'Estimate the combined WHERE-clause selectivity for a CREATE INDEX statement
using pg_stats heuristics (Approach B).  AND predicates are multiplied; OR
predicates are combined with inclusion-exclusion; UNKNOWN predicates use 0.333.
Returns 1.0 when the statement has no WHERE clause.';

REVOKE ALL ON FUNCTION ddl_instead_of.estimate_index_where_selectivity(oid, text) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION ddl_instead_of.estimate_index_where_selectivity(oid, text) TO PUBLIC;

-- -------------------------------------------------------------------------
-- estimate_index_size
--
-- Estimate the DocDB storage footprint and recommended tablet count for a
-- CREATE INDEX statement, based on the current row count and column
-- statistics of the base table.
--
-- DocDB storage model per index entry
-- ------------------------------------
--   Key part  = SUM(key_col_avg_width + 1 value-type byte)
--             + ybctid_bytes          (base-table row identifier)
--             + 12 bytes HLC
--             + 8 bytes RocksDB key metadata
--   Value part = SUM(include_col_avg_width + 1 value-type byte)
--              + 4 bytes DocDB value prefix
--   Raw entry = key_part + value_part
--   Disk entry = raw_entry × 1.15  (SST block + filter overhead)
--
-- Tablet recommendation
-- ----------------------
--   effective_rows = reltuples × where_selectivity
--   total_bytes    = effective_rows × disk_entry_bytes
--   tablets        = total_bytes / target_tablet_bytes  (no rounding)
--
-- Parameters
-- -----------
--   p_statement          Raw CREATE INDEX SQL text.
--   p_target_tablet_bytes Target size per tablet in bytes (default 10 GiB).
--   p_ybctid_bytes       Expected ybctid size; default 24 bytes.
--
-- Returns
-- -------
--   table_name           Base table (schema-qualified).
--   index_key_cols       Comma-separated key column list.
--   index_include_cols   Comma-separated INCLUDE column list ('' if none).
--   where_clause         WHERE predicate text ('' if none).
--   base_rows            pg_class.reltuples (may be -1 before first ANALYZE).
--   where_selectivity    Estimated fraction of rows that satisfy the WHERE clause.
--   effective_rows       base_rows × where_selectivity.
--   entry_bytes_raw      Estimated uncompressed bytes per index entry.
--   entry_bytes_disk     entry_bytes_raw × SST amplification factor.
--   total_bytes          effective_rows × entry_bytes_disk.
--   recommended_tablets  total_bytes / target_tablet_bytes.
--   notes                Diagnostic / warning messages.
-- -------------------------------------------------------------------------

CREATE FUNCTION ddl_instead_of.estimate_index_size(
    p_statement           text,
    p_target_tablet_bytes bigint  DEFAULT 10737418240,  -- 10 GiB
    p_ybctid_bytes        integer DEFAULT 24
) RETURNS TABLE (
    table_name            text,
    index_key_cols        text,
    index_include_cols    text,
    where_clause          text,
    base_rows             float8,
    where_selectivity     float8,
    effective_rows        float8,
    entry_bytes_raw       float8,
    entry_bytes_disk      float8,
    total_bytes           float8,
    recommended_tablets   float8,
    notes                 text
)
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    -- ---- DocDB overhead constants (bytes) -----
    c_value_type_byte    CONSTANT float8 := 1.0;  -- type byte per column in key/value
    c_hlc_bytes          CONSTANT float8 := 12.0; -- hybrid logical clock
    c_rkv_meta_bytes     CONSTANT float8 := 8.0;  -- RocksDB key metadata
    c_value_prefix_bytes CONSTANT float8 := 4.0;  -- DocDB value prefix
    c_sst_amplification  CONSTANT float8 := 1.15; -- block/filter/bloom overhead

    v_schema_name        text;
    v_table_name_raw     text;
    v_table_fqn          text;
    v_table_oid          oid;
    v_base_rows          float8;
    v_where_sel          float8;
    v_effective_rows     float8;

    v_key_width          float8 := 0.0;
    v_key_type_bytes     float8 := 0.0;
    v_val_width          float8 := 0.0;
    v_val_type_bytes     float8 := 0.0;

    v_key_cols           text[] := '{}';
    v_inc_cols           text[] := '{}';
    v_where_text         text   := '';

    v_entry_raw          float8;
    v_entry_disk         float8;
    v_total_bytes        float8;
    v_tablets            float8;

    v_col_width          float8;
    v_notes              text[] := '{}';

    r                    record;
BEGIN
    -- ----------------------------------------------------------------
    -- 1. Resolve base table OID and row count
    -- ----------------------------------------------------------------
    SELECT schema_name, object_name
    INTO   v_schema_name, v_table_name_raw
    FROM   ddl_instead_of.parse_command_info('CREATE INDEX', p_statement)
    LIMIT  1;

    IF v_table_name_raw IS NULL THEN
        RAISE EXCEPTION 'estimate_index_size: could not parse table name from statement';
    END IF;

    v_table_fqn := CASE
        WHEN v_schema_name IS NOT NULL
        THEN quote_ident(v_schema_name) || '.' || quote_ident(v_table_name_raw)
        ELSE quote_ident(v_table_name_raw)
    END;

    BEGIN
        v_table_oid := v_table_fqn::regclass::oid;
    EXCEPTION WHEN undefined_table OR invalid_schema_name THEN
        RAISE EXCEPTION 'estimate_index_size: table % does not exist', v_table_fqn;
    END;

    SELECT GREATEST(reltuples, 0)
    INTO   v_base_rows
    FROM   pg_class
    WHERE  oid = v_table_oid;

    IF v_base_rows = 0 THEN
        v_notes := array_append(v_notes,
            'reltuples = 0; run ANALYZE on the table for accurate estimates');
    END IF;

    -- ----------------------------------------------------------------
    -- 2. Walk index columns; accumulate key width and include width
    -- ----------------------------------------------------------------
    FOR r IN
        SELECT is_key, ordinal, column_name, expression, ordering, where_clause
        FROM   ddl_instead_of.parse_index_columns(p_statement)
    LOOP
        -- Capture where_clause from any row (it is the same on all rows)
        IF v_where_text = '' AND r.where_clause IS NOT NULL THEN
            v_where_text := r.where_clause;
        END IF;

        -- Compute avg width for this column
        IF r.column_name IS NOT NULL THEN
            v_col_width := ddl_instead_of._estimate_avg_col_width(
                               v_table_oid, r.column_name);
        ELSE
            -- Expression column: use a conservative default
            v_col_width := 32.0;
            v_notes := array_append(v_notes,
                format('expression index column #%s: defaulting to 32 bytes',
                       r.ordinal));
        END IF;

        IF r.is_key THEN
            v_key_width      := v_key_width + v_col_width;
            v_key_type_bytes := v_key_type_bytes + c_value_type_byte;
            v_key_cols       := array_append(v_key_cols,
                                    COALESCE(r.column_name,
                                             '(' || r.expression || ')'));
        ELSE
            v_val_width      := v_val_width + v_col_width;
            v_val_type_bytes := v_val_type_bytes + c_value_type_byte;
            v_inc_cols       := array_append(v_inc_cols,
                                    COALESCE(r.column_name,
                                             '(' || r.expression || ')'));
        END IF;
    END LOOP;

    -- ----------------------------------------------------------------
    -- 3. Estimate WHERE selectivity using pg_stats heuristics
    -- ----------------------------------------------------------------
    IF v_where_text <> '' THEN
        v_where_sel := ddl_instead_of.estimate_index_where_selectivity(
                           v_table_oid, p_statement);
    ELSE
        v_where_sel := 1.0;
    END IF;

    v_effective_rows := v_base_rows * v_where_sel;

    -- ----------------------------------------------------------------
    -- 4. Compute per-entry DocDB storage
    --
    --   Key = col data + type bytes + ybctid + HLC + RKV metadata
    --   Val = include col data + type bytes + value prefix
    -- ----------------------------------------------------------------
    v_entry_raw :=
        (v_key_width  + v_key_type_bytes)           -- key column payload
        + p_ybctid_bytes                            -- ybctid (base-table PK)
        + c_hlc_bytes                               -- HLC timestamp
        + c_rkv_meta_bytes                          -- RocksDB key overhead
        + (v_val_width + v_val_type_bytes)          -- INCLUDE columns
        + c_value_prefix_bytes;                     -- DocDB value header

    v_entry_disk  := v_entry_raw * c_sst_amplification;
    v_total_bytes := v_effective_rows * v_entry_disk;
    v_tablets     := v_total_bytes / NULLIF(p_target_tablet_bytes, 0);

    IF v_tablets IS NULL THEN
        v_tablets := 0;
        v_notes := array_append(v_notes, 'target_tablet_bytes = 0: tablet count undefined');
    END IF;

    -- ----------------------------------------------------------------
    -- 5. Return result row
    -- ----------------------------------------------------------------
    RETURN QUERY SELECT
        v_table_fqn,
        array_to_string(v_key_cols, ', '),
        array_to_string(v_inc_cols, ', '),
        v_where_text,
        v_base_rows,
        v_where_sel,
        v_effective_rows,
        v_entry_raw,
        v_entry_disk,
        v_total_bytes,
        v_tablets,
        array_to_string(v_notes, '; ');
END;
$$;

COMMENT ON FUNCTION ddl_instead_of.estimate_index_size(text, bigint, integer) IS
'Estimate the DocDB storage footprint (bytes) and recommended tablet count for
a CREATE INDEX statement.  Uses pg_stats heuristics (Approach B) for WHERE
predicate selectivity estimation.  No rounding is applied to the tablet count
so callers can apply their own ceiling / threshold logic.';

REVOKE ALL ON FUNCTION ddl_instead_of.estimate_index_size(text, bigint, integer) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION ddl_instead_of.estimate_index_size(text, bigint, integer) TO PUBLIC;
