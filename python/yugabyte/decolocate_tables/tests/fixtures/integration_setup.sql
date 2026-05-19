-- Integration test fixture for decolocate_tables.
-- Run against a colocated database before invoking the tool.

CREATE TABLE IF NOT EXISTS decolocate_base (
    id int PRIMARY KEY,
    v int NOT NULL,
    username text,
    e int DEFAULT 10,
    f int NOT NULL
);

TRUNCATE decolocate_base;

INSERT INTO decolocate_base (id, v, f, username)
SELECT g, g, g, 'user' || g::text
FROM generate_series(1, 5) g;

ALTER TABLE decolocate_base ADD CONSTRAINT IF NOT EXISTS a_check CHECK (id > 0);

CREATE UNIQUE INDEX IF NOT EXISTS decolocate_base_b_idx ON decolocate_base (v);

CREATE OR REPLACE VIEW decolocate_v1 AS
    SELECT id, v FROM decolocate_base;

CREATE OR REPLACE VIEW decolocate_v2 AS
    SELECT id, v FROM decolocate_v1 WHERE v > 2;

ALTER TABLE decolocate_base ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'decolocate_user1') THEN
        CREATE USER decolocate_user1;
    END IF;
END $$;

DROP POLICY IF EXISTS decolocate_p ON decolocate_base;
CREATE POLICY decolocate_p ON decolocate_base
    FOR SELECT TO decolocate_user1
    USING (username = current_user);

GRANT SELECT ON decolocate_base TO decolocate_user1;

CREATE OR REPLACE FUNCTION decolocate_dummy() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    RETURN NULL;
END;
$$;

DROP TRIGGER IF EXISTS decolocate_dummy_trigger ON decolocate_base;
CREATE TRIGGER decolocate_dummy_trigger
    AFTER INSERT ON decolocate_base
    FOR EACH ROW
    EXECUTE PROCEDURE decolocate_dummy();
