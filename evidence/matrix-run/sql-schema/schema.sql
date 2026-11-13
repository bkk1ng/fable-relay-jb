-- Bookings system schema
-- SQLite 3.35+: supports generated columns, RETURNING, DROP COLUMN
-- Foreign keys must be enabled per-connection: PRAGMA foreign_keys = ON;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY,
    username    TEXT    NOT NULL UNIQUE,
    email       TEXT    NOT NULL UNIQUE,
    display_name TEXT,
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    CHECK (length(username) >= 2),
    CHECK (email LIKE '%_@_%.__%')
);

CREATE INDEX IF NOT EXISTS idx_users_email    ON users (email);
CREATE INDEX IF NOT EXISTS idx_users_username ON users (username);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, desks, equipment, etc.)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS resources (
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL,
    type         TEXT    NOT NULL DEFAULT 'generic',
    capacity     INTEGER NOT NULL DEFAULT 1 CHECK (capacity >= 1),
    location     TEXT,
    is_active    INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    CHECK (length(name) >= 1)
);

CREATE INDEX IF NOT EXISTS idx_resources_type      ON resources (type);
CREATE INDEX IF NOT EXISTS idx_resources_is_active ON resources (is_active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bookings (
    id          INTEGER PRIMARY KEY,
    user_id     INTEGER NOT NULL REFERENCES users     (id) ON DELETE RESTRICT,
    resource_id INTEGER NOT NULL REFERENCES resources (id) ON DELETE RESTRICT,

    -- ISO-8601 strings, stored as TEXT so SQLite datetime functions work
    starts_at   TEXT    NOT NULL,
    ends_at     TEXT    NOT NULL,

    title       TEXT,
    notes       TEXT,
    status      TEXT    NOT NULL DEFAULT 'confirmed'
                        CHECK (status IN ('confirmed', 'cancelled', 'pending')),

    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time-range sanity: end must be strictly after start, minimum 1 minute
    CHECK (ends_at > starts_at),
    CHECK (
        (julianday(ends_at) - julianday(starts_at)) * 1440 >= 1
    )
);

-- Common lookup patterns
CREATE INDEX IF NOT EXISTS idx_bookings_user_id     ON bookings (user_id);
CREATE INDEX IF NOT EXISTS idx_bookings_resource_id ON bookings (resource_id);
CREATE INDEX IF NOT EXISTS idx_bookings_starts_at   ON bookings (starts_at);
CREATE INDEX IF NOT EXISTS idx_bookings_ends_at     ON bookings (ends_at);

-- Composite index for the overlap query: resource + time window
CREATE INDEX IF NOT EXISTS idx_bookings_resource_time
    ON bookings (resource_id, starts_at, ends_at);

-- Status filter is common (e.g. ignore cancelled when checking overlap)
CREATE INDEX IF NOT EXISTS idx_bookings_status ON bookings (status);

-- ---------------------------------------------------------------------------
-- Overlap prevention
--
-- A new booking (starts_at = :s, ends_at = :e) overlaps an existing one when:
--   existing.starts_at < :e  AND  existing.ends_at > :s
--
-- This trigger fires BEFORE INSERT and BEFORE UPDATE on bookings.
-- It raises an error if any confirmed/pending booking for the same resource
-- already occupies the requested window.
--
-- Note: SQLite does not support deferred constraint triggers, so this is the
-- standard approach.  If you need advisory locking across concurrent writers
-- use WAL mode + BEGIN IMMEDIATE transactions in the application layer.
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_insert
BEFORE INSERT ON bookings
FOR EACH ROW
WHEN NEW.status IN ('confirmed', 'pending')
BEGIN
    SELECT RAISE(ABORT, 'booking overlaps an existing reservation')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  status      IN ('confirmed', 'pending')
          AND  starts_at   <  NEW.ends_at
          AND  ends_at     >  NEW.starts_at
    );
END;

CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_update
BEFORE UPDATE ON bookings
FOR EACH ROW
WHEN NEW.status IN ('confirmed', 'pending')
BEGIN
    SELECT RAISE(ABORT, 'booking overlaps an existing reservation')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  id         <> NEW.id           -- exclude the row being updated
          AND  status      IN ('confirmed', 'pending')
          AND  starts_at   <  NEW.ends_at
          AND  ends_at     >  NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- updated_at maintenance
-- One AFTER UPDATE trigger per table; fires only when a real column changed.
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_users_updated_at
AFTER UPDATE ON users
FOR EACH ROW
BEGIN
    UPDATE users
       SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
     WHERE id = NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_resources_updated_at
AFTER UPDATE ON resources
FOR EACH ROW
BEGIN
    UPDATE resources
       SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
     WHERE id = NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_bookings_updated_at
AFTER UPDATE ON bookings
FOR EACH ROW
BEGIN
    UPDATE bookings
       SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
     WHERE id = NEW.id;
END;
