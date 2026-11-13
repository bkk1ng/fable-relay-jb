-- reports.sql
-- Bookings system analytics for SQLite (window functions available).
-- Schema: bookings(id, user_id, resource_id, starts_at, ends_at, cents)
--
-- Run with: sqlite3 bookings.db < reports.sql
--   or load the file in the sqlite3 REPL:  .read reports.sql


-- =============================================================================
-- Query 1: Monthly revenue with running total and month-over-month delta
-- =============================================================================
-- Inner query: collapse every booking into its billing month and sum the cents,
-- producing one revenue figure per YYYY-MM.
-- Outer query: uses window functions ordered by month so the frame spans from
-- the first month up to and including the current row.
--   SUM() OVER (ORDER BY month)  -> cumulative running total across all months
--   LAG()  OVER (ORDER BY month) -> prior month's revenue, for the delta
-- The delta is NULL for the earliest month because there is no previous value.
SELECT
    month,
    revenue_cents,
    -- Cumulative revenue from the first month through the current month.
    SUM(revenue_cents) OVER (ORDER BY month) AS running_total_cents,
    -- Current month revenue minus the previous month's revenue (NULL for the first row).
    revenue_cents - LAG(revenue_cents) OVER (ORDER BY month) AS mom_delta_cents
FROM (
    SELECT
        strftime('%Y-%m', starts_at) AS month,
        SUM(cents)                    AS revenue_cents
    FROM bookings
    GROUP BY strftime('%Y-%m', starts_at)
)
ORDER BY month;


-- =============================================================================
-- Query 2: Per-resource utilisation ranking with RANK and NTILE
-- =============================================================================
-- Utilisation is measured as total booked time per resource (seconds booked),
-- with revenue shown alongside for context.
-- Inner query: per resource, sum the elapsed booking time and the revenue.
--   julianday() difference * 86400 converts day fractions to seconds.
-- Outer query: ranks every resource by utilisation.
--   RANK()  gives 1,2,2,4-style ordering (ties share a rank, next rank skips).
--   NTILE(4) splits the resources into four equally sized buckets (quartiles),
--   with larger buckets absorbing the remainder when the count isn't divisible.
SELECT
    resource_id,
    booked_seconds,
    revenue_cents,
    -- Dense ordinal rank by utilisation (highest booked time first).
    RANK() OVER (ORDER BY booked_seconds DESC) AS util_rank,
    -- Quartile bucket: 1 = top utilisation quarter ... 4 = bottom quarter.
    NTILE(4) OVER (ORDER BY booked_seconds DESC) AS util_quartile
FROM (
    SELECT
        resource_id,
        SUM((julianday(ends_at) - julianday(starts_at)) * 86400) AS booked_seconds,
        SUM(cents)                                              AS revenue_cents
    FROM bookings
    GROUP BY resource_id
)
ORDER BY util_rank;


-- =============================================================================
-- Query 3: Gap detection between consecutive bookings per resource using LAG/LEAD
-- =============================================================================
-- Finds idle windows: the gap between the end of one booking and the start of
-- the next booking on the same resource, ordered by start time.
-- Inner query: LAG(ends_at) pulls the previous booking's end time into the
-- current row, partitioned per resource so gaps are only compared within one
-- resource's timeline.
-- Outer query: keeps only rows where a real gap exists (start is later than
-- the previous end), filtering out overlapping/contiguous bookings and the
-- first booking of each resource (which has no previous end).
-- gap_seconds is the idle time in seconds; swap the filter comparison to
-- '<' if you instead want to surface overlaps/double-bookings.
SELECT
    resource_id,
    prev_ends_at,
    starts_at,
    -- Idle seconds between the previous booking's end and this booking's start.
    (julianday(starts_at) - julianday(prev_ends_at)) * 86400 AS gap_seconds
FROM (
    SELECT
        resource_id,
        starts_at,
        ends_at,
        LAG(ends_at) OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        ) AS prev_ends_at
    FROM bookings
)
WHERE prev_ends_at IS NOT NULL
  AND starts_at > prev_ends_at
ORDER BY resource_id, starts_at;
