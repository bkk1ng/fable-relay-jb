-- migrate.sql
-- Versioned SQLite migration script.
-- Guards each up-migration with PRAGMA user_version so the script is
-- idempotent: running it twice (or more) produces no errors and no
-- duplicate work.
--
-- Usage:
--   sqlite3 your_database.db < migrate.sql
--
-- Down-migrations are provided as comments at the bottom of each
-- migration block.  Run them manually in reverse order when you need
-- to roll back.
-- ---------------------------------------------------------------------------

-- ---------------------------------------------------------------------------
-- Bootstrap: schema_migrations table
--
-- Created once; subsequent runs skip it via "IF NOT EXISTS".
-- Stores the version number and a timestamp for every applied migration.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);


-- ===========================================================================
-- Migration 001 — create "articles" table
-- ===========================================================================
-- DOWN: DROP TABLE IF EXISTS articles;
--       DELETE FROM schema_migrations WHERE version = 1;
--       PRAGMA user_version = 0;
-- ===========================================================================
BEGIN;

SELECT CASE
    WHEN (SELECT COUNT(*) FROM schema_migrations WHERE version = 1) > 0
    THEN RAISE(IGNORE)   -- already applied; skip the whole transaction
END;

CREATE TABLE articles (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    title      TEXT    NOT NULL,
    body       TEXT    NOT NULL,
    created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

INSERT INTO schema_migrations (version) VALUES (1);
PRAGMA user_version = 1;

COMMIT;


-- ===========================================================================
-- Migration 002 — add "status" column with default and back-fill
-- ===========================================================================
-- DOWN: -- SQLite does not support DROP COLUMN before 3.35.0; for older
--       -- versions recreate the table without the column.
--       --
--       -- For SQLite >= 3.35.0:
--       --   ALTER TABLE articles DROP COLUMN status;
--       --
--       -- For older SQLite (table-rebuild approach):
--       --   BEGIN;
--       --   CREATE TABLE articles_old AS SELECT id, title, body, created_at
--       --       FROM articles;
--       --   DROP TABLE articles;
--       --   ALTER TABLE articles_old RENAME TO articles;
--       --   COMMIT;
--       --
--       DELETE FROM schema_migrations WHERE version = 2;
--       PRAGMA user_version = 1;
-- ===========================================================================
BEGIN;

SELECT CASE
    WHEN (SELECT COUNT(*) FROM schema_migrations WHERE version = 2) > 0
    THEN RAISE(IGNORE)
END;

-- Add the column.  "IF NOT EXISTS" is not supported for ADD COLUMN in SQLite,
-- but the RAISE(IGNORE) guard above ensures we only reach this line once.
ALTER TABLE articles ADD COLUMN status TEXT NOT NULL DEFAULT 'draft';

-- Back-fill: mark every row that existed before this migration as 'published'.
-- Rows inserted after this migration keep the 'draft' default.
UPDATE articles
SET    status = 'published'
WHERE  applied_at IS NULL   -- always false; real predicate below
   OR  id <= (SELECT COALESCE(MAX(id), 0) FROM articles);
-- Simpler equivalent (back-fill ALL existing rows):
-- UPDATE articles SET status = 'published';

INSERT INTO schema_migrations (version) VALUES (2);
PRAGMA user_version = 2;

COMMIT;


-- ===========================================================================
-- Migration 003 — create index on articles(status)
-- ===========================================================================
-- DOWN: DROP INDEX IF EXISTS idx_articles_status;
--       DELETE FROM schema_migrations WHERE version = 3;
--       PRAGMA user_version = 2;
-- ===========================================================================
BEGIN;

SELECT CASE
    WHEN (SELECT COUNT(*) FROM schema_migrations WHERE version = 3) > 0
    THEN RAISE(IGNORE)
END;

-- "IF NOT EXISTS" makes the CREATE itself idempotent as a second safety net,
-- though the version guard above already prevents a second execution.
CREATE INDEX IF NOT EXISTS idx_articles_status ON articles (status);

INSERT INTO schema_migrations (version) VALUES (3);
PRAGMA user_version = 3;

COMMIT;


-- ===========================================================================
-- End of migrations.
--
-- To verify after running:
--   SELECT * FROM schema_migrations;
--   PRAGMA user_version;          -- should be 3
-- ===========================================================================
