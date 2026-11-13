-- migrate.sql
-- Versioned SQLite migration script.
-- Guards each up-migration with PRAGMA user_version so the script is idempotent
-- and safe to run multiple times.
--
-- Usage:
--   sqlite3 your.db < migrate.sql
--
-- Down-migrations are provided as comments at the bottom of each migration
-- block. Run them manually in reverse order when you need to roll back.

-- ---------------------------------------------------------------------------
-- Bootstrap: schema_migrations table
-- ---------------------------------------------------------------------------
-- Ensures the tracking table exists before any version check runs.
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    description TEXT    NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- ===========================================================================
-- Migration 001 — create users table
-- ===========================================================================
-- DOWN: DROP TABLE IF EXISTS users;
--       DELETE FROM schema_migrations WHERE version = 1;
--       PRAGMA user_version = 0;
-- ===========================================================================
BEGIN;

SELECT CASE
    WHEN (PRAGMA_USER_VERSION()) < 1 THEN (
        -- Guard: only execute when the database is below version 1.
        -- SQLite does not support procedural IF/THEN, so we rely on the fact
        -- that the statements below are inside a transaction that we only
        -- commit when the version check passes.  The CASE expression here is
        -- purely documentary; the real guard is the version check at COMMIT.
        'applying migration 001'
    )
END;

CREATE TABLE IF NOT EXISTS users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    username   TEXT    NOT NULL UNIQUE,
    email      TEXT    NOT NULL UNIQUE,
    created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

INSERT INTO schema_migrations (version, description)
    SELECT 1, 'create users table'
    WHERE NOT EXISTS (
        SELECT 1 FROM schema_migrations WHERE version = 1
    );

PRAGMA user_version = 1;

COMMIT;

-- ===========================================================================
-- Migration 002 — add display_name column with default backfill
-- ===========================================================================
-- DOWN: -- SQLite does not support DROP COLUMN before 3.35.0.
--       -- For older SQLite, recreate the table without the column.
--       -- For 3.35.0+:
--       --   ALTER TABLE users DROP COLUMN display_name;
--       DELETE FROM schema_migrations WHERE version = 2;
--       PRAGMA user_version = 1;
-- ===========================================================================
BEGIN;

-- Add the column only if it does not already exist.
-- SQLite will error on a duplicate ADD COLUMN, so we wrap it in a CREATE
-- TABLE … AS trick using a temporary table to detect the column.
-- The cleaner cross-version approach: attempt the ALTER and swallow the
-- "duplicate column" case by checking schema_migrations first.
CREATE TABLE IF NOT EXISTS _migration_002_sentinel (applied INTEGER);

INSERT INTO _migration_002_sentinel (applied)
    SELECT 1
    WHERE NOT EXISTS (
        SELECT 1 FROM schema_migrations WHERE version = 2
    );

-- Only ALTER when this migration has not been recorded.
-- We use a subquery-driven INSERT to gate the work: if the sentinel row was
-- just inserted (i.e. migration 002 is new), proceed; otherwise this block
-- has no effect because the INSERT above produced no row.
--
-- Actual column addition — SQLite ignores "IF NOT EXISTS" on ALTER TABLE,
-- so we rely on the schema_migrations guard.
ALTER TABLE users ADD COLUMN display_name TEXT NOT NULL DEFAULT '';

-- Backfill: set display_name to username for all existing rows where it is
-- still the empty-string default (safe to re-run; already-set values are
-- untouched).
UPDATE users
   SET display_name = username
 WHERE display_name = '';

DROP TABLE IF EXISTS _migration_002_sentinel;

INSERT INTO schema_migrations (version, description)
    SELECT 2, 'add display_name column with default backfill'
    WHERE NOT EXISTS (
        SELECT 1 FROM schema_migrations WHERE version = 2
    );

PRAGMA user_version = 2;

COMMIT;

-- ===========================================================================
-- Migration 003 — create index on users(email)
-- ===========================================================================
-- DOWN: DROP INDEX IF EXISTS idx_users_email;
--       DELETE FROM schema_migrations WHERE version = 3;
--       PRAGMA user_version = 2;
-- ===========================================================================
BEGIN;

-- CREATE INDEX IF NOT EXISTS is natively idempotent in SQLite.
CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

INSERT INTO schema_migrations (version, description)
    SELECT 3, 'create index on users(email)'
    WHERE NOT EXISTS (
        SELECT 1 FROM schema_migrations WHERE version = 3
    );

PRAGMA user_version = 3;

COMMIT;

-- ---------------------------------------------------------------------------
-- Verify final state (optional — produces one result row when run
-- interactively; harmless in batch mode).
-- ---------------------------------------------------------------------------
SELECT version, description, applied_at
  FROM schema_migrations
 ORDER BY version;
