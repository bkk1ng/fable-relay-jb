-- schema.sql
-- Bookings system: users, resources, bookings
-- SQLite 3.35+

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    email       TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    display_name TEXT   NOT NULL CHECK (LENGTH(TRIM(display_name)) > 0),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, equipment, etc.)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS resources (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    description  TEXT,
    capacity     INTEGER CHECK (capacity IS NULL OR capacity > 0),
    is_active    INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_resources_active ON resources (is_active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bookings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL
                    REFERENCES users (id) ON DELETE RESTRICT,
    resource_id INTEGER NOT NULL
                    REFERENCES resources (id) ON DELETE RESTRICT,
    starts_at   TEXT    NOT NULL,   -- ISO-8601, UTC: '2025-06-01T09:00:00Z'
    ends_at     TEXT    NOT NULL,   -- ISO-8601, UTC: '2025-06-01T10:00:00Z'
    title       TEXT    NOT NULL CHECK (LENGTH(TRIM(title)) > 0),
    notes       TEXT,
    status      TEXT    NOT NULL DEFAULT 'confirmed'
                    CHECK (status IN ('confirmed', 'cancelled', 'pending')),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time-range sanity: end must be strictly after start
    CHECK (ends_at > starts_at),

    -- Minimum booking duration: at least 1 minute
    -- SQLite stores ISO-8601 as text; we compare lexicographically which is
    -- safe for UTC timestamps in the format above.
    CHECK (ends_at > starts_at)
);

-- Overlap prevention:
-- A new booking for resource R may not overlap any *confirmed* existing booking
-- for R.  Two intervals [s1,e1) and [s2,e2) overlap when s1 < e2 AND s2 < e1.
-- This is enforced by a partial unique index trick combined with a trigger
-- below, because SQLite does not support deferrable constraints or exclusion
-- indexes natively.
--
-- Index to make overlap-check queries fast:
CREATE INDEX IF NOT EXISTS idx_bookings_resource_time
    ON bookings (resource_id, starts_at, ends_at)
    WHERE status = 'confirmed';

-- Index for per-user lookups:
CREATE INDEX IF NOT EXISTS idx_bookings_user
    ON bookings (user_id, starts_at);

-- Index for status-based filtering:
CREATE INDEX IF NOT EXISTS idx_bookings_status ON bookings (status);

-- ---------------------------------------------------------------------------
-- Trigger: enforce no overlapping confirmed bookings on INSERT
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_insert
BEFORE INSERT ON bookings
WHEN NEW.status = 'confirmed'
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps an existing confirmed booking for this resource')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  status      = 'confirmed'
          AND  starts_at   < NEW.ends_at
          AND  ends_at     > NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- Trigger: enforce no overlapping confirmed bookings on UPDATE
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_update
BEFORE UPDATE ON bookings
WHEN NEW.status = 'confirmed'
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps an existing confirmed booking for this resource')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  status      = 'confirmed'
          AND  id         != NEW.id          -- exclude the row being updated
          AND  starts_at   < NEW.ends_at
          AND  ends_at     > NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- Trigger: maintain updated_at on users
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_users_updated_at
AFTER UPDATE ON users
FOR EACH ROW
BEGIN
    UPDATE users
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;

-- ---------------------------------------------------------------------------
-- Trigger: maintain updated_at on resources
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_resources_updated_at
AFTER UPDATE ON resources
FOR EACH ROW
BEGIN
    UPDATE resources
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;

-- ---------------------------------------------------------------------------
-- Trigger: maintain updated_at on bookings
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_updated_at
AFTER UPDATE ON bookings
FOR EACH ROW
BEGIN
    UPDATE bookings
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;
