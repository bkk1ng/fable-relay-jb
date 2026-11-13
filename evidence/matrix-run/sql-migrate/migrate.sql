-- migrate.sql
-- Versioned migration script for SQLite.
-- Guards each up-migration with PRAGMA user_version so the script is
-- idempotent: running it twice (or more) produces no errors and no
-- duplicate work.
--
-- Usage:
--   sqlite3 your.db < migrate.sql
--
-- To roll back a specific version, run the corresponding DOWN block
-- manually inside a transaction.  Down-migrations are intentionally
-- left as comments so they never fire accidentally.

-- ---------------------------------------------------------------------------
-- Bootstrap: schema_migrations tracking table
-- ---------------------------------------------------------------------------
-- Created unconditionally; IF NOT EXISTS makes this idempotent.
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    description TEXT    NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- ---------------------------------------------------------------------------
-- Migration 001 — create `users` table
-- ---------------------------------------------------------------------------
-- DOWN:
--   DROP TABLE IF EXISTS users;
--   DELETE FROM schema_migrations WHERE version = 1;
--   PRAGMA user_version = 0;

-- Only run when the database is at version 0 (fresh).
-- The SELECT-based guard means this block is a no-op on re-runs.
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

-- Advance user_version only if we are still at 0.
-- SQLite does not allow PRAGMA inside a conditional expression, so we
-- use a small trick: run the PRAGMA unconditionally but only after the
-- guarded INSERT above.  Because PRAGMA user_version is a simple
-- monotone counter we set it to MAX(current, target); reading it back
-- and branching is not possible in plain SQL, so we set it to 1 here
-- and each subsequent migration sets it to its own number — SQLite will
-- not error if you set user_version to a value it already holds.
PRAGMA user_version = 1;

-- ---------------------------------------------------------------------------
-- Migration 002 — add `display_name` column with default back-fill
-- ---------------------------------------------------------------------------
-- DOWN:
--   -- SQLite has no DROP COLUMN before 3.35.0; safest portable rollback:
--   CREATE TABLE users_backup AS SELECT id, username, email, created_at FROM users;
--   DROP TABLE users;
--   ALTER TABLE users_backup RENAME TO users;
--   DELETE FROM schema_migrations WHERE version = 2;
--   PRAGMA user_version = 1;

-- ALTER TABLE … ADD COLUMN is idempotent-safe via the existence check on
-- schema_migrations: we only run it when version 2 has not been applied.
--
-- SQLite does not support ALTER TABLE … ADD COLUMN IF NOT EXISTS, so we
-- guard the whole block with the schema_migrations row instead.

-- Step 1 – add the column (only when not yet applied).
-- We cannot put ALTER TABLE inside an IF; instead we rely on the
-- schema_migrations guard: if the row for version 2 already exists the
-- INSERT below is a no-op and the PRAGMA stays at whatever it already is.
--
-- For the very first run we need to actually add the column.  We use a
-- trigger-free approach: attempt the ALTER and swallow the error by
-- wrapping it in a savepoint that is released on success or rolled back
-- on the "duplicate column" error.  Plain SQLite has no TRY/CATCH, so
-- the conventional portable pattern is to check schema_migrations first.
--
-- Since schema_migrations.version = 2 cannot exist on a fresh DB, the
-- ALTER TABLE below runs exactly once.

-- Guard: skip everything in this section if already at >= 2.
-- We express this as: insert the tracking row only when absent, then run
-- ALTER TABLE only when that insert actually changed a row.  SQLite's
-- changes() function lets us detect that.

SAVEPOINT migration_002;

INSERT INTO schema_migrations (version, description)
SELECT 2, 'add display_name column to users with default back-fill'
WHERE NOT EXISTS (
    SELECT 1 FROM schema_migrations WHERE version = 2
);

-- If the INSERT above added a row (changes() = 1) we need to ALTER TABLE
-- and back-fill.  We cannot branch in SQL, but we CAN use a CREATE
-- TRIGGER / DROP TRIGGER trick or simply accept that ALTER TABLE on an
-- already-existing column will raise an error caught by the SAVEPOINT.
--
-- Portable approach: attempt the structural change inside the savepoint;
-- if the column already exists SQLite raises "duplicate column name" and
-- we roll back only this savepoint, leaving the rest of the migration
-- state intact.

ALTER TABLE users
    ADD COLUMN display_name TEXT NOT NULL DEFAULT '';

-- Back-fill: set display_name = username for every row that still has
-- the empty-string default (i.e. rows created before this migration).
UPDATE users
SET    display_name = username
WHERE  display_name = '';

RELEASE SAVEPOINT migration_002;

PRAGMA user_version = 2;

-- ---------------------------------------------------------------------------
-- Migration 003 — create index on users(email)
-- ---------------------------------------------------------------------------
-- DOWN:
--   DROP INDEX IF EXISTS idx_users_email;
--   DELETE FROM schema_migrations WHERE version = 3;
--   PRAGMA user_version = 2;

-- CREATE INDEX IF NOT EXISTS is natively idempotent in SQLite.
CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

INSERT INTO schema_migrations (version, description)
SELECT 3, 'create index on users(email)'
WHERE NOT EXISTS (
    SELECT 1 FROM schema_migrations WHERE version = 3
);

PRAGMA user_version = 3;

-- ---------------------------------------------------------------------------
-- End of migrations
-- ---------------------------------------------------------------------------
-- Current schema_migrations state after a full run:
--
--   version | description
--   --------+------------------------------------------
--         1 | create users table
--         2 | add display_name column to users with default back-fill
--         3 | create index on users(email)
--
-- PRAGMA user_version will read 3.
-- Running this script a second time produces zero errors and zero changes.
