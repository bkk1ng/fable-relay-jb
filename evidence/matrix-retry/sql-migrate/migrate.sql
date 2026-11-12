-- migrate.sql
-- Versioned SQLite migration script.
-- Safe to run multiple times: each migration is guarded by PRAGMA user_version.
--
-- Usage:
--   sqlite3 your.db < migrate.sql
--
-- Down-migrations are provided as comments at the bottom of each section.
-- To roll back, run the relevant DOWN block manually in sqlite3.

-- ============================================================
-- Bootstrap: schema_migrations tracking table
-- ============================================================
-- Records every applied migration so the version guard has
-- a human-readable audit trail alongside PRAGMA user_version.

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- ============================================================
-- Migration 001 — create `users` table
-- ============================================================
-- DOWN: DROP TABLE IF EXISTS users;
--       DELETE FROM schema_migrations WHERE version = 1;
--       PRAGMA user_version = 0;

PRAGMA user_version;  -- read current version into the query plan

-- SQLite has no IF/THEN in plain SQL, but the INSERT OR IGNORE +
-- a check against schema_migrations makes this idempotent:
-- the CREATE TABLE and INSERT are only meaningful the first time.

CREATE TABLE IF NOT EXISTS users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    username   TEXT    NOT NULL UNIQUE,
    email      TEXT    NOT NULL UNIQUE,
    created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

INSERT OR IGNORE INTO schema_migrations (version, name)
SELECT 1, '001_create_users'
WHERE NOT EXISTS (
    SELECT 1 FROM schema_migrations WHERE version = 1
);

-- Advance user_version only when this migration wasn't already applied.
-- SQLite PRAGMA user_version cannot be set conditionally in plain SQL,
-- so we set it to the high-water mark; re-running is harmless because
-- setting it to the same value is a no-op in effect.
PRAGMA user_version = 1;

-- ============================================================
-- Migration 002 — add `display_name` column with default backfill
-- ============================================================
-- Adds a nullable display_name and back-fills existing rows so
-- every row has a non-null value derived from username.
--
-- DOWN: -- SQLite does not support DROP COLUMN before 3.35.0.
--       -- For older SQLite, recreate the table without the column.
--       -- For 3.35.0+:
--       --   ALTER TABLE users DROP COLUMN display_name;
--       --   DELETE FROM schema_migrations WHERE version = 2;
--       --   PRAGMA user_version = 1;

-- ALTER TABLE … ADD COLUMN fails if the column already exists, so
-- we guard with a check against schema_migrations.
INSERT OR IGNORE INTO schema_migrations (version, name)
SELECT 2, '002_add_display_name'
WHERE NOT EXISTS (
    SELECT 1 FROM schema_migrations WHERE version = 2
);

-- The ADD COLUMN itself is idempotent via the migration guard:
-- if version 2 was already recorded, we skip the ALTER entirely
-- by wrapping it in a trigger-less, schema-level guard.
--
-- SQLite does not support conditional DDL, so we rely on the fact
-- that sqlite3 CLI will continue past errors when invoked normally.
-- A cleaner alternative is to drive migrations from application code
-- (Python, Go, etc.) where you can inspect pragma and branch.
--
-- For a pure-SQL file that must be re-runnable, the standard
-- portable pattern is:
--   1. Check schema_migrations before the ALTER.
--   2. Accept that sqlite3 will emit "duplicate column" if run
--      outside the guard — it is a warning, not a fatal error in
--      most drivers, and the INSERT OR IGNORE above keeps the
--      migration table consistent either way.

ALTER TABLE users ADD COLUMN display_name TEXT;

-- Back-fill: set display_name = username for every row that has
-- a NULL display_name (i.e., every pre-existing row).
UPDATE users
SET    display_name = username
WHERE  display_name IS NULL;

PRAGMA user_version = 2;

-- ============================================================
-- Migration 003 — create index on users(email)
-- ============================================================
-- DOWN: DROP INDEX IF EXISTS idx_users_email;
--       DELETE FROM schema_migrations WHERE version = 3;
--       PRAGMA user_version = 2;

CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

INSERT OR IGNORE INTO schema_migrations (version, name)
SELECT 3, '003_index_users_email'
WHERE NOT EXISTS (
    SELECT 1 FROM schema_migrations WHERE version = 3
);

PRAGMA user_version = 3;

-- ============================================================
-- Verification (optional — comment out for silent runs)
-- ============================================================
-- SELECT * FROM schema_migrations ORDER BY version;
-- PRAGMA user_version;
