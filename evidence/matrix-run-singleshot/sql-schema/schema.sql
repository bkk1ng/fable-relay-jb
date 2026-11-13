-- Bookings system schema
-- SQLite 3.x (requires PRAGMA foreign_keys = ON at connection time)

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

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

CREATE INDEX IF NOT EXISTS idx_users_email
    ON users (email);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, equipment, desks, etc.)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS resources (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT    NOT NULL CHECK (LENGTH(TRIM(name)) > 0),
    type         TEXT    NOT NULL DEFAULT 'generic',   -- e.g. 'room', 'desk', 'equipment'
    capacity     INTEGER          CHECK (capacity IS NULL OR capacity > 0),
    description  TEXT,
    is_active    INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_resources_type
    ON resources (type);

CREATE INDEX IF NOT EXISTS idx_resources_active
    ON resources (is_active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bookings (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id      INTEGER NOT NULL REFERENCES users     (id) ON DELETE RESTRICT,
    resource_id  INTEGER NOT NULL REFERENCES resources (id) ON DELETE RESTRICT,
    title        TEXT    NOT NULL CHECK (LENGTH(TRIM(title)) > 0),
    starts_at    TEXT    NOT NULL,  -- ISO-8601 UTC, e.g. '2025-06-01T09:00:00Z'
    ends_at      TEXT    NOT NULL,  -- ISO-8601 UTC
    status       TEXT    NOT NULL DEFAULT 'confirmed'
                         CHECK (status IN ('confirmed', 'cancelled', 'pending')),
    notes        TEXT,
    created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time range must be valid: end strictly after start, minimum 1-minute duration
    CHECK (ends_at > starts_at),
    CHECK (
        (JULIANDAY(ends_at) - JULIANDAY(starts_at)) * 1440 >= 1
    )
);

-- Covering index for the per-user schedule query
CREATE INDEX IF NOT EXISTS idx_bookings_user_time
    ON bookings (user_id, starts_at, ends_at)
    WHERE status != 'cancelled';

-- Covering index for the per-resource availability query
CREATE INDEX IF NOT EXISTS idx_bookings_resource_time
    ON bookings (resource_id, starts_at, ends_at)
    WHERE status != 'cancelled';

-- Index to support listing bookings by status
CREATE INDEX IF NOT EXISTS idx_bookings_status
    ON bookings (status);

-- ---------------------------------------------------------------------------
-- Overlap-prevention trigger
--
-- Fires BEFORE INSERT and BEFORE UPDATE on bookings.
-- Raises an error if any other non-cancelled booking on the same resource
-- has a time range that overlaps the new/modified booking.
--
-- Two intervals [s1,e1) and [s2,e2) overlap when s1 < e2 AND s2 < e1.
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_insert
BEFORE INSERT ON bookings
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'booking overlaps an existing booking for this resource')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  status      != 'cancelled'
          AND  starts_at   <  NEW.ends_at
          AND  ends_at     >  NEW.starts_at
    );
END;

CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_update
BEFORE UPDATE ON bookings
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'booking overlaps an existing booking for this resource')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  id          != NEW.id           -- exclude the row being updated
          AND  status      != 'cancelled'
          AND  starts_at   <  NEW.ends_at
          AND  ends_at     >  NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- updated_at maintenance triggers
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_users_updated_at
AFTER UPDATE ON users
BEGIN
    UPDATE users
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_resources_updated_at
AFTER UPDATE ON resources
BEGIN
    UPDATE resources
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_bookings_updated_at
AFTER UPDATE ON bookings
BEGIN
    UPDATE bookings
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;
