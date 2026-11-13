-- Bookings system schema
-- SQLite 3.x; requires PRAGMA foreign_keys = ON at connection time.

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY,
    email       TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    display_name TEXT   NOT NULL CHECK (LENGTH(TRIM(display_name)) > 0),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, desks, equipment, etc.)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS resources (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    description TEXT,
    capacity    INTEGER CHECK (capacity IS NULL OR capacity > 0),
    active      INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_resources_active ON resources (active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bookings (
    id          INTEGER PRIMARY KEY,
    user_id     INTEGER NOT NULL
                    REFERENCES users (id) ON DELETE RESTRICT ON UPDATE CASCADE,
    resource_id INTEGER NOT NULL
                    REFERENCES resources (id) ON DELETE RESTRICT ON UPDATE CASCADE,
    -- ISO-8601 strings; SQLite uses text comparison which is correct for this format.
    starts_at   TEXT    NOT NULL,
    ends_at     TEXT    NOT NULL,
    title       TEXT    NOT NULL DEFAULT '' CHECK (LENGTH(title) <= 500),
    notes       TEXT,
    status      TEXT    NOT NULL DEFAULT 'confirmed'
                    CHECK (status IN ('confirmed', 'cancelled', 'pending')),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time range sanity: end must be strictly after start, minimum 1 minute.
    CHECK (ends_at > starts_at),
    CHECK (
        CAST((julianday(ends_at) - julianday(starts_at)) * 1440 AS INTEGER) >= 1
    )
);

-- Common access patterns:
--   • All bookings for a resource in a time window (overlap check + calendar view)
--   • All bookings for a user (profile / history)
--   • Status filtering

CREATE INDEX IF NOT EXISTS idx_bookings_resource_time
    ON bookings (resource_id, starts_at, ends_at)
    WHERE status != 'cancelled';

CREATE INDEX IF NOT EXISTS idx_bookings_user_time
    ON bookings (user_id, starts_at)
    WHERE status != 'cancelled';

CREATE INDEX IF NOT EXISTS idx_bookings_status
    ON bookings (status);

-- ---------------------------------------------------------------------------
-- Overlap prevention
--
-- A new booking conflicts with an existing one when:
--   new.starts_at < existing.ends_at  AND  new.ends_at > existing.starts_at
--
-- Only active (non-cancelled) bookings block a slot.
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_insert
BEFORE INSERT ON bookings
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'booking overlaps an existing reservation')
    WHERE EXISTS (
        SELECT 1 FROM bookings
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
    SELECT RAISE(ABORT, 'booking overlaps an existing reservation')
    WHERE EXISTS (
        SELECT 1 FROM bookings
        WHERE  resource_id = NEW.resource_id
          AND  id          != NEW.id          -- exclude the row being updated
          AND  status      != 'cancelled'
          AND  starts_at   <  NEW.ends_at
          AND  ends_at     >  NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- updated_at maintenance
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
