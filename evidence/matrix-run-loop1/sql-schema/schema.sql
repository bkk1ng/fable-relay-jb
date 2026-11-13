-- Bookings system schema
-- SQLite 3.35+: supports generated columns, RETURNING, DROP COLUMN
-- Foreign keys must be enabled per-connection: PRAGMA foreign_keys = ON;

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    email       TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    display_name TEXT   NOT NULL,
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    CHECK (length(trim(email)) > 0),
    CHECK (length(trim(display_name)) > 0)
);

CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, equipment, desks, etc.)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS resources (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT    NOT NULL,
    type        TEXT    NOT NULL DEFAULT 'room',   -- room | desk | equipment | …
    capacity    INTEGER NOT NULL DEFAULT 1,
    location    TEXT,
    is_active   INTEGER NOT NULL DEFAULT 1,        -- 0 = soft-deleted / disabled
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    CHECK (length(trim(name)) > 0),
    CHECK (capacity >= 1),
    CHECK (is_active IN (0, 1))
);

CREATE INDEX IF NOT EXISTS idx_resources_type     ON resources (type);
CREATE INDEX IF NOT EXISTS idx_resources_active   ON resources (is_active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bookings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL REFERENCES users(id)     ON DELETE RESTRICT,
    resource_id INTEGER NOT NULL REFERENCES resources(id) ON DELETE RESTRICT,

    -- ISO-8601 strings stored as TEXT; SQLite's time functions work on them
    starts_at   TEXT    NOT NULL,
    ends_at     TEXT    NOT NULL,

    title       TEXT,
    notes       TEXT,
    status      TEXT    NOT NULL DEFAULT 'confirmed', -- confirmed | cancelled | pending

    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time range sanity
    CHECK (ends_at > starts_at),
    -- Minimum 5-minute booking
    CHECK ((julianday(ends_at) - julianday(starts_at)) * 1440 >= 5),
    -- Maximum 24-hour booking
    CHECK ((julianday(ends_at) - julianday(starts_at)) <= 1),

    CHECK (status IN ('confirmed', 'cancelled', 'pending')),
    CHECK (length(trim(starts_at)) > 0),
    CHECK (length(trim(ends_at)) > 0)
);

-- Core lookup: find bookings for a resource in a time window
CREATE INDEX IF NOT EXISTS idx_bookings_resource_time
    ON bookings (resource_id, starts_at, ends_at);

-- Look up all bookings for a user
CREATE INDEX IF NOT EXISTS idx_bookings_user
    ON bookings (user_id, starts_at);

-- Filter by status efficiently
CREATE INDEX IF NOT EXISTS idx_bookings_status
    ON bookings (status, starts_at);

-- ---------------------------------------------------------------------------
-- Overlap prevention
--
-- Two bookings for the same resource overlap when:
--   new.starts_at < existing.ends_at  AND  new.ends_at > existing.starts_at
--
-- We allow a booking to share an exact boundary (back-to-back is fine).
-- Cancelled bookings do not block the slot.
--
-- SQLite does not support DEFERRABLE constraints, so we implement this in a
-- BEFORE INSERT and BEFORE UPDATE trigger.
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_insert
BEFORE INSERT ON bookings
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps with an existing confirmed or pending booking')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings b
        WHERE  b.resource_id = NEW.resource_id
          AND  b.status      != 'cancelled'
          AND  b.starts_at    < NEW.ends_at
          AND  b.ends_at      > NEW.starts_at
    );
END;

CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_update
BEFORE UPDATE ON bookings
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps with an existing confirmed or pending booking')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings b
        WHERE  b.resource_id = NEW.resource_id
          AND  b.id          != NEW.id          -- exclude self
          AND  b.status      != 'cancelled'
          AND  b.starts_at    < NEW.ends_at
          AND  b.ends_at      > NEW.starts_at
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
