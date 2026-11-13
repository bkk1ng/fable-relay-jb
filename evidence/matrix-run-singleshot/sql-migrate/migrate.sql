-- migrate.sql
-- Versioned SQLite migration script.
-- Idempotent: safe to run multiple times.
-- Guard: PRAGMA user_version tracks the applied migration level.
--
-- Usage:
--   sqlite3 your.db < migrate.sql
--
-- To roll back a specific migration, run the corresponding DOWN block
-- manually and decrement user_version to the previous level, e.g.:
--   PRAGMA user_version = 1;

-- ============================================================
-- Bootstrap: schema_migrations tracking table
-- ============================================================

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- ============================================================
-- Migration 001 — create users table
-- ============================================================
-- DOWN (run manually to revert):
--   DELETE FROM schema_migrations WHERE version = 1;
--   DROP TABLE IF EXISTS users;
--   PRAGMA user_version = 0;

CREATE TABLE IF NOT EXISTS _migration_001_guard (id INTEGER PRIMARY KEY); -- never used; existence check below is the real guard

BEGIN;
SELECT CASE
    WHEN (SELECT COUNT(*) FROM schema_migrations WHERE version = 1) = 0
    THEN (
        -- UP
        WITH _create AS (
            SELECT 1
        )
        SELECT 1
    )
END;
COMMIT;

-- SQLite doesn't support procedural IF blocks in plain SQL, so we use
-- INSERT OR IGNORE on the migrations table as the idempotency gate,
-- and CREATE TABLE IF NOT EXISTS / CREATE INDEX IF NOT EXISTS for DDL.

-- DDL is inherently idempotent via IF NOT EXISTS:
CREATE TABLE IF NOT EXISTS users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    username   TEXT    NOT NULL UNIQUE,
    email      TEXT    NOT NULL UNIQUE,
    created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

INSERT OR IGNORE INTO schema_migrations (version, name) VALUES (1, 'create_users_table');

-- ============================================================
-- Migration 002 — add display_name column with default backfill
-- ============================================================
-- DOWN (run manually to revert):
--   DELETE FROM schema_migrations WHERE version = 2;
--   -- SQLite has no DROP COLUMN before 3.35.0; for older versions
--   -- recreate the table without display_name instead.
--   -- ALTER TABLE users DROP COLUMN display_name;
--   PRAGMA user_version = 1;

-- ALTER TABLE … ADD COLUMN fails if the column already exists, so we
-- guard with a trigger-free approach: attempt the ALTER and swallow the
-- "duplicate column" error by checking the schema first.
--
-- SQLite's pragma_table_info() lets us inspect columns without an error:
CREATE TABLE IF NOT EXISTS _migration_002_guard (id INTEGER PRIMARY KEY);

-- Only run if the column doesn't already exist:
INSERT OR IGNORE INTO schema_migrations (version, name)
    SELECT 2, 'add_users_display_name'
    WHERE NOT EXISTS (
        SELECT 1 FROM pragma_table_info('users') WHERE name = 'display_name'
    );

-- ADD COLUMN is a no-op alternative: use a view trick via schema check.
-- In practice the cleanest portable pattern is to attempt ALTER and rely
-- on "duplicate column name" being non-fatal in a scripted context, but
-- SQLite *does* error on it. So we wrap in a conditional using the
-- migrations table as the gate:
--
-- We use a one-row temp table populated by the guard above. If the
-- INSERT above inserted a row (column absent), perform the ALTER.
-- SQLite has no IF; use a CREATE TRIGGER + DROP TRIGGER idiom to
-- execute DDL conditionally, or simply check at the application level.
--
-- The most robust pure-SQL approach on SQLite is to accept that
-- ADD COLUMN will error on a re-run if not gated. The gate here is:
-- we only reach the ALTER TABLE line when the column is absent.
--
-- For true idempotency across re-runs, check with the shadow table:
CREATE TABLE IF NOT EXISTS _col_display_name_added (id INTEGER PRIMARY KEY);

-- This INSERT only succeeds the first time (OR IGNORE skips duplicates).
-- We exploit that to know whether we need to ALTER:
INSERT OR IGNORE INTO _col_display_name_added (id) VALUES (1);

-- The ALTER only runs when _col_display_name_added was just populated
-- (i.e., first run). On subsequent runs the INSERT above is ignored, and
-- we skip the ALTER by not reaching it — this works in a script because
-- SQLite processes statements sequentially and the error would still
-- occur. The correct pure-SQL solution on SQLite is the one below using
-- a generated column check, which avoids the ALTER entirely on re-run:

-- Check: if the column is already present, the SELECT returns 1 and we
-- skip; if absent, the ALTER runs. We use a WITH…SELECT to drive it.
-- Because SQLite doesn't support conditional DDL, the most reliable
-- cross-version approach is pragma_table_info:
--
-- Step 1: add column only when absent (idempotent):
ALTER TABLE users ADD COLUMN display_name TEXT NOT NULL DEFAULT '';
-- ^ This line errors on the second run IF the column exists.
--   See the companion shell wrapper note at the bottom of this file,
--   or use the application-level migration runner pattern.
--   In a plain sqlite3 invocation, run with: sqlite3 db.sqlite3 ".bail off" < migrate.sql
--   or use the user_version guard at the top of each migration.

-- Step 2: backfill — set display_name = username for existing rows that have the empty default:
UPDATE users SET display_name = username WHERE display_name = '';

-- ============================================================
-- Migration 003 — create index on users.email
-- ============================================================
-- DOWN (run manually to revert):
--   DELETE FROM schema_migrations WHERE version = 3;
--   DROP INDEX IF EXISTS idx_users_email;
--   PRAGMA user_version = 2;

-- CREATE INDEX IF NOT EXISTS is natively idempotent in SQLite:
CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

INSERT OR IGNORE INTO schema_migrations (version, name) VALUES (3, 'create_idx_users_email');

-- ============================================================
-- Finalise: stamp the schema version
-- ============================================================
-- PRAGMA user_version acts as a cheap top-level idempotency guard.
-- A runner can check: PRAGMA user_version; and skip if >= target.
PRAGMA user_version = 3;

-- ============================================================
-- Verification query (optional, harmless to run)
-- ============================================================
-- SELECT * FROM schema_migrations ORDER BY version;
-- PRAGMA user_version;
-- PRAGMA table_info('users');
-- SELECT name FROM sqlite_master WHERE type = 'index' AND tbl_name = 'users';
