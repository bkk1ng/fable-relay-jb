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
    username    TEXT    NOT NULL UNIQUE         COLLATE NOCASE,
    email       TEXT    NOT NULL UNIQUE         COLLATE NOCASE,
    display_name TEXT,
    is_active   INTEGER NOT NULL DEFAULT 1      CHECK (is_active IN (0, 1)),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_users_email    ON users (email);
CREATE INDEX IF NOT EXISTS idx_users_username ON users (username);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, equipment, desks, etc.)
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS resources (
    id            INTEGER PRIMARY KEY,
    name          TEXT    NOT NULL,
    resource_type TEXT    NOT NULL,              -- e.g. 'room', 'desk', 'vehicle'
    capacity      INTEGER          DEFAULT NULL  CHECK (capacity IS NULL OR capacity > 0),
    location      TEXT,
    is_active     INTEGER NOT NULL DEFAULT 1     CHECK (is_active IN (0, 1)),
    created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_resources_type   ON resources (resource_type);
CREATE INDEX IF NOT EXISTS idx_resources_active ON resources (is_active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
-- Overlap prevention is handled by:
--   1. A CHECK constraint that enforces starts_at < ends_at
--   2. A partial unique index that prevents any two non-cancelled bookings for
--      the same resource from overlapping.
--      SQLite supports the WHERE clause on indexes but cannot enforce the
--      interval overlap condition directly in an index alone, so a BEFORE
--      INSERT / BEFORE UPDATE trigger performs the runtime check.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS bookings (
    id          INTEGER PRIMARY KEY,
    user_id     INTEGER NOT NULL
                    REFERENCES users (id)
                        ON DELETE RESTRICT
                        ON UPDATE CASCADE,
    resource_id INTEGER NOT NULL
                    REFERENCES resources (id)
                        ON DELETE RESTRICT
                        ON UPDATE CASCADE,
    starts_at   TEXT    NOT NULL,               -- ISO-8601 UTC, e.g. '2025-06-01T09:00:00Z'
    ends_at     TEXT    NOT NULL,               -- ISO-8601 UTC
    title       TEXT,
    notes       TEXT,
    status      TEXT    NOT NULL DEFAULT 'confirmed'
                    CHECK (status IN ('confirmed', 'tentative', 'cancelled')),
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time-range sanity: end must be strictly after start
    CHECK (ends_at > starts_at)
);

-- Fast lookups by resource + time window (the most common query pattern)
CREATE INDEX IF NOT EXISTS idx_bookings_resource_time
    ON bookings (resource_id, starts_at, ends_at)
    WHERE status != 'cancelled';

-- Fast lookups by user
CREATE INDEX IF NOT EXISTS idx_bookings_user
    ON bookings (user_id, starts_at);

-- Fast lookups by status
CREATE INDEX IF NOT EXISTS idx_bookings_status
    ON bookings (status, starts_at);

-- ---------------------------------------------------------------------------
-- Overlap-prevention trigger  (INSERT)
-- ---------------------------------------------------------------------------
-- Raises an error when a new confirmed/tentative booking overlaps any
-- existing confirmed/tentative booking for the same resource.
-- Two intervals [A.starts, A.ends) and [B.starts, B.ends) overlap when:
--   A.starts < B.ends  AND  A.ends > B.starts
-- ---------------------------------------------------------------------------

CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_insert
BEFORE INSERT ON bookings
FOR EACH ROW
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps an existing booking for this resource')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  status      != 'cancelled'
          AND  starts_at   <  NEW.ends_at
          AND  ends_at     >  NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- Overlap-prevention trigger  (UPDATE)
-- ---------------------------------------------------------------------------

CREATE TRIGGER IF NOT EXISTS trg_bookings_no_overlap_update
BEFORE UPDATE OF resource_id, starts_at, ends_at, status ON bookings
FOR EACH ROW
WHEN NEW.status != 'cancelled'
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps an existing booking for this resource')
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
FOR EACH ROW
BEGIN
    UPDATE users
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_resources_updated_at
AFTER UPDATE ON resources
FOR EACH ROW
BEGIN
    UPDATE resources
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_bookings_updated_at
AFTER UPDATE ON bookings
FOR EACH ROW
BEGIN
    UPDATE bookings
    SET    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    WHERE  id = NEW.id;
END;
