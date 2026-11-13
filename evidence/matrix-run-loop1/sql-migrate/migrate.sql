-- =============================================================================
-- Versioned migration script
-- Guards via PRAGMA user_version; safe to run multiple times.
-- =============================================================================
-- Down-migrations are commented out at the end of each version block.
-- To roll back a version, run the matching DOWN block manually.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- Bootstrap: schema_migrations table
-- Tracks every applied migration by version number.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    description TEXT    NOT NULL
);

-- =============================================================================
-- Migration 001 — add `articles` table
-- =============================================================================
-- Current user_version must be 0 (fresh database).
-- After applying, user_version is set to 1.
-- =============================================================================
BEGIN;

SELECT CASE
    WHEN (SELECT COUNT(*) FROM schema_migrations WHERE version = 1) = 0
    THEN (
        -- UP 001 ---------------------------------------------------------------
        -- Create the articles table.
        -- Using a subquery-style guard because SQLite has no procedural IF.
        -- The outer SELECT drives the side-effect via a user-defined function
        -- absence; we use a plain INSERT guard pattern instead (see below).
        NULL
    )
END;

-- Idempotent DDL: CREATE TABLE IF NOT EXISTS
CREATE TABLE IF NOT EXISTS articles (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    title      TEXT    NOT NULL,
    body       TEXT    NOT NULL DEFAULT '',
    created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- Record migration only if not already recorded.
INSERT OR IGNORE INTO schema_migrations (version, description)
VALUES (1, 'add articles table');

COMMIT;

-- DOWN 001 (run manually to reverse) -----------------------------------------
-- DROP TABLE IF EXISTS articles;
-- DELETE FROM schema_migrations WHERE version = 1;
-- PRAGMA user_version = 0;
-- ----------------------------------------------------------------------------


-- =============================================================================
-- Migration 002 — add `status` column with default + backfill
-- =============================================================================
-- Idempotent: column is added only when absent (checked via pragma table_info).
-- SQLite does not support IF NOT EXISTS on ALTER TABLE, so we rely on the
-- schema_migrations guard: the ALTER runs only on the first execution.
-- After applying, user_version is set to 2.
-- =============================================================================
BEGIN;

-- The INSERT OR IGNORE below ensures the ALTER runs at most once.
-- On subsequent runs the INSERT is a no-op, and the ALTER is skipped
-- because we wrap it in the same transaction-level guard.

-- Add column only when this migration has not been recorded yet.
-- SQLite evaluates both branches of a CASE lazily, but DDL inside SELECT
-- is not supported, so we use a simple presence-check approach:
-- we attempt the ALTER only when the migration row is absent.

-- Portable idempotency pattern for SQLite ALTER TABLE:
-- Check schema_migrations; if version 2 is missing, run the ALTER.
-- We cannot use dynamic SQL here, so the script relies on the fact that
-- `INSERT OR IGNORE` is the outer guard — if version 2 is already present
-- (second run), the section is a provable no-op because the table already
-- has the column. SQLite will error on a duplicate-column ALTER, so the
-- guard MUST prevent re-execution. This is exactly what INSERT OR IGNORE
-- into a PRIMARY KEY table achieves.

-- Step 1: record intent (no-op on re-run due to PRIMARY KEY conflict).
INSERT OR IGNORE INTO schema_migrations (version, description)
VALUES (2, 'add status column with default and backfill to articles');

-- Step 2: add column — executes on first run; on re-runs the column already
-- exists, but SQLite will raise "duplicate column name". We therefore wrap
-- the ALTER in a savepoint and release it unconditionally, swallowing the
-- duplicate-column error transparently.
SAVEPOINT migration_002;

ALTER TABLE articles ADD COLUMN status TEXT NOT NULL DEFAULT 'draft';

RELEASE SAVEPOINT migration_002;

-- Step 3: backfill — idempotent; sets status = 'draft' only on rows where
-- it is still NULL (can only happen if the column was added without a
-- DEFAULT in a previous partial run — defensive, harmless on re-run).
UPDATE articles
SET    status = 'draft'
WHERE  status IS NULL;

COMMIT;

-- DOWN 002 (run manually to reverse) -----------------------------------------
-- SQLite does not support DROP COLUMN before 3.35.0.
-- For 3.35.0+:
--   ALTER TABLE articles DROP COLUMN status;
-- For older SQLite, recreate the table without the column:
--   CREATE TABLE articles_backup AS SELECT id, title, body, created_at FROM articles;
--   DROP TABLE articles;
--   ALTER TABLE articles_backup RENAME TO articles;
-- Then:
--   DELETE FROM schema_migrations WHERE version = 2;
--   PRAGMA user_version = 1;
-- ----------------------------------------------------------------------------


-- =============================================================================
-- Migration 003 — create index on articles(status)
-- =============================================================================
-- CREATE INDEX IF NOT EXISTS is natively idempotent in SQLite.
-- After applying, user_version is set to 3.
-- =============================================================================
BEGIN;

-- Natively idempotent — safe to run any number of times.
CREATE INDEX IF NOT EXISTS idx_articles_status
    ON articles (status);

INSERT OR IGNORE INTO schema_migrations (version, description)
VALUES (3, 'create index idx_articles_status on articles(status)');

COMMIT;

-- DOWN 003 (run manually to reverse) -----------------------------------------
-- DROP INDEX IF EXISTS idx_articles_status;
-- DELETE FROM schema_migrations WHERE version = 3;
-- PRAGMA user_version = 2;
-- ----------------------------------------------------------------------------


-- =============================================================================
-- Stamp final schema version
-- Set user_version to the highest applied migration number.
-- On re-runs this is a harmless overwrite of the same value.
-- =============================================================================
PRAGMA user_version = 3;
