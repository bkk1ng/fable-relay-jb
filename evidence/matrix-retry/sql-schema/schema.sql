-- Bookings system schema
-- SQLite 3.x (requires PRAGMA foreign_keys = ON at connection time)

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY,
    email       TEXT    NOT NULL UNIQUE,
    display_name TEXT   NOT NULL,
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_users_email ON users (email);

-- ---------------------------------------------------------------------------
-- Resources  (rooms, equipment, desks, etc.)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS resources (
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL,
    type         TEXT    NOT NULL,                -- e.g. 'room', 'desk', 'vehicle'
    capacity     INTEGER NOT NULL DEFAULT 1
                         CHECK (capacity >= 1),
    is_active    INTEGER NOT NULL DEFAULT 1
                         CHECK (is_active IN (0, 1)),
    created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_resources_type     ON resources (type);
CREATE INDEX IF NOT EXISTS idx_resources_active   ON resources (is_active);

-- ---------------------------------------------------------------------------
-- Bookings
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bookings (
    id          INTEGER PRIMARY KEY,
    user_id     INTEGER NOT NULL
                REFERENCES users (id) ON DELETE CASCADE,
    resource_id INTEGER NOT NULL
                REFERENCES resources (id) ON DELETE RESTRICT,
    starts_at   TEXT    NOT NULL,               -- ISO-8601, e.g. '2025-03-01T09:00:00Z'
    ends_at     TEXT    NOT NULL,
    status      TEXT    NOT NULL DEFAULT 'confirmed'
                CHECK (status IN ('confirmed', 'cancelled', 'pending')),
    notes       TEXT,
    created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),

    -- Time range must be valid
    CHECK (ends_at > starts_at),

    -- Minimum booking duration: 1 minute
    CHECK (
        (julianday(ends_at) - julianday(starts_at)) * 1440.0 >= 1.0
    ),

    -- Maximum booking duration: 30 days
    CHECK (
        (julianday(ends_at) - julianday(starts_at)) <= 30.0
    )
);

-- Covering indexes for the common lookup patterns
CREATE INDEX IF NOT EXISTS idx_bookings_user
    ON bookings (user_id, starts_at, ends_at);

CREATE INDEX IF NOT EXISTS idx_bookings_resource
    ON bookings (resource_id, starts_at, ends_at);

CREATE INDEX IF NOT EXISTS idx_bookings_status
    ON bookings (status, starts_at);

-- Overlap prevention: a resource cannot have two confirmed/pending bookings
-- whose time ranges intersect.
--
-- Two intervals [s1,e1) and [s2,e2) overlap when s1 < e2 AND s2 < e1.
-- The partial index covers only active bookings, so cancellations are ignored.
CREATE UNIQUE INDEX IF NOT EXISTS uix_bookings_no_overlap
    ON bookings (resource_id, starts_at, ends_at)
    WHERE status IN ('confirmed', 'pending');

-- A stricter overlap check is enforced by the trigger below, because SQLite
-- unique indexes alone cannot express the half-open interval condition.

-- ---------------------------------------------------------------------------
-- Trigger: reject overlapping active bookings on INSERT
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_overlap_insert
BEFORE INSERT ON bookings
WHEN NEW.status IN ('confirmed', 'pending')
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps with an existing active booking')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  status      IN ('confirmed', 'pending')
          AND  starts_at   < NEW.ends_at
          AND  ends_at     > NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- Trigger: reject overlapping active bookings on UPDATE
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_overlap_update
BEFORE UPDATE ON bookings
WHEN NEW.status IN ('confirmed', 'pending')
BEGIN
    SELECT RAISE(ABORT, 'Booking overlaps with an existing active booking')
    WHERE EXISTS (
        SELECT 1
        FROM   bookings
        WHERE  resource_id = NEW.resource_id
          AND  id          != NEW.id
          AND  status      IN ('confirmed', 'pending')
          AND  starts_at   < NEW.ends_at
          AND  ends_at     > NEW.starts_at
    );
END;

-- ---------------------------------------------------------------------------
-- Trigger: maintain updated_at — users
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_users_updated_at
AFTER UPDATE ON users
FOR EACH ROW
BEGIN
    UPDATE users
       SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
     WHERE id = OLD.id;
END;

-- ---------------------------------------------------------------------------
-- Trigger: maintain updated_at — resources
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_resources_updated_at
AFTER UPDATE ON resources
FOR EACH ROW
BEGIN
    UPDATE resources
       SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
     WHERE id = OLD.id;
END;

-- ---------------------------------------------------------------------------
-- Trigger: maintain updated_at — bookings
-- ---------------------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_bookings_updated_at
AFTER UPDATE ON bookings
FOR EACH ROW
BEGIN
    UPDATE bookings
       SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
     WHERE id = OLD.id;
END;
