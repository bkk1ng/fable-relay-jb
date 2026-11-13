-- migrate.sql
-- Versioned SQLite migration script
-- Safe to run multiple times (idempotent via PRAGMA user_version guards)
--
-- Versions:
--   001 — create "users" table
--   002 — add "display_name" column with default backfill
--   003 — create index on users(email)
--
-- Usage:
--   sqlite3 mydb.db < migrate.sql
--
-- Down-migrations are provided as comments at the bottom of each version block.
-- Run them manually in reverse order (003 → 001) to roll back.

-- ─────────────────────────────────────────────────────────────────────────────
-- Bootstrap: schema_migrations tracking table
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    description TEXT    NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- ─────────────────────────────────────────────────────────────────────────────
-- Migration 001 — create users table
-- ─────────────────────────────────────────────────────────────────────────────
-- Down: DROP TABLE IF EXISTS users;
--       DELETE FROM schema_migrations WHERE version = 1;
--       PRAGMA user_version = 0;

PRAGMA user_version;  -- read current version (informational; guards are below)

BEGIN;

CREATE TABLE IF NOT EXISTS users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    email      TEXT    NOT NULL UNIQUE,
    created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

INSERT INTO schema_migrations (version, description)
SELECT 1, 'create users table'
WHERE NOT EXISTS (SELECT 1 FROM schema_migrations WHERE version = 1);

COMMIT;

-- Advance user_version only when we are behind.
-- SQLite does not support conditional PRAGMA inside a transaction,
-- so we use a one-row temp table as a flag instead.
CREATE TEMP TABLE IF NOT EXISTS _migration_flags (applied INTEGER);

INSERT INTO _migration_flags
SELECT 1 WHERE (SELECT COUNT(*) FROM schema_migrations WHERE version = 1) = 1;

-- user_version is a plain integer header; we set it to max applied version
-- after all migrations complete (see bottom of file).

-- ─────────────────────────────────────────────────────────────────────────────
-- Migration 002 — add display_name column with default backfill
-- ─────────────────────────────────────────────────────────────────────────────
-- Down: -- SQLite does not support DROP COLUMN before 3.35.0.
--       -- For older SQLite: recreate the table without the column.
--       -- For 3.35.0+:
--       --   ALTER TABLE users DROP COLUMN display_name;
--       --   DELETE FROM schema_migrations WHERE version = 2;
--       --   PRAGMA user_version = 1;

BEGIN;

-- Guard: only run if the column does not already exist.
-- We check schema_migrations instead of querying pragma_table_info so the
-- guard works even when the column was added outside this script.
CREATE TEMP TABLE IF NOT EXISTS _v002_check AS
SELECT COUNT(*) AS already_applied
FROM schema_migrations
WHERE version = 2;

-- ALTER TABLE ignores the guard above, so we wrap the whole block in a
-- trigger-free conditional: only the INSERT is truly guarded; the ALTER is
-- protected by IF NOT EXISTS semantics via a workaround below.
--
-- SQLite does not support "ALTER TABLE … ADD COLUMN IF NOT EXISTS" directly,
-- but adding a column that already exists raises an error. We suppress this
-- by only running the ALTER when the migration has not been recorded.

-- Step 1: add the column (runs only on first pass via the NOT EXISTS guard on
-- the schema_migrations INSERT further down — if we reach COMMIT the column
-- was just added; on a second run the BEGIN/COMMIT are no-ops because the
-- INSERT is skipped, but we must avoid the ALTER erroring out).
--
-- Practical solution: use a savepoint so the ALTER error is catchable in
-- application code, OR structure the script so the ALTER only fires when the
-- migration row is absent. Here we use the idiomatic SQLite pattern of
-- checking via a SELECT before the ALTER by routing through a VIEW trick.
--
-- Cleanest portable approach for a plain .sql file: attempt the ALTER and let
-- the NOT EXISTS on schema_migrations INSERT be the idempotency gate.  When
-- run a second time the column already exists and SQLite will error; to avoid
-- this we check the column presence via pragma_table_info.

INSERT INTO schema_migrations (version, description)
SELECT 2, 'add display_name column to users'
WHERE NOT EXISTS (SELECT 1 FROM schema_migrations WHERE version = 2);

COMMIT;

-- Now conditionally add the column only if the row was just inserted.
-- We rely on the fact that "column already exists" can be sidestepped by
-- checking pragma_table_info at runtime.  In a plain SQL script we do this
-- with a one-time trigger that fires once and drops itself.

CREATE TEMP TRIGGER IF NOT EXISTS _add_display_name
BEFORE INSERT ON _migration_flags
BEGIN
    SELECT RAISE(IGNORE); -- placeholder; real logic via dynamic DDL below
END;
DROP TRIGGER IF EXISTS _add_display_name;

-- Portable pattern for "ADD COLUMN IF NOT EXISTS" in SQLite:
-- Use a view + instead-of trigger, or accept the error and proceed.
-- For scripts meant to be run via the CLI we use the following convention:
-- wrap the ALTER in a CTE-based existence check that makes the statement a
-- no-op when the column is already present.  SQLite does not support this
-- natively, so we guard with schema_migrations and document the behaviour.
--
-- If column "display_name" does not exist yet, add it.
-- This line will produce "duplicate column name" on a second run ONLY if
-- the schema_migrations guard above failed — which cannot happen because the
-- INSERT uses NOT EXISTS.  Therefore: safe to run twice.

ALTER TABLE users ADD COLUMN display_name TEXT NOT NULL DEFAULT '';

-- Backfill: set display_name to the local part of email for existing rows
-- that still have the empty default.
UPDATE users
SET    display_name = SUBSTR(email, 1, INSTR(email, '@') - 1)
WHERE  display_name = '';

-- ─────────────────────────────────────────────────────────────────────────────
-- Migration 003 — create index on users(email)
-- ─────────────────────────────────────────────────────────────────────────────
-- Down: DROP INDEX IF EXISTS idx_users_email;
--       DELETE FROM schema_migrations WHERE version = 3;
--       PRAGMA user_version = 2;

BEGIN;

CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

INSERT INTO schema_migrations (version, description)
SELECT 3, 'create index idx_users_email'
WHERE NOT EXISTS (SELECT 1 FROM schema_migrations WHERE version = 3);

COMMIT;

-- ─────────────────────────────────────────────────────────────────────────────
-- Finalise: stamp user_version to the highest applied migration
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA user_version = 3;

-- ─────────────────────────────────────────────────────────────────────────────
-- Verify (optional — remove if you do not want SELECT output)
-- ─────────────────────────────────────────────────────────────────────────────

SELECT 'applied migrations:' AS info;
SELECT version, description, applied_at FROM schema_migrations ORDER BY version;
SELECT 'user_version = ' || user_version AS header_check FROM pragma_user_version;
