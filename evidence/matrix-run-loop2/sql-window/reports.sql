-- reports.sql
-- Booking-system reporting queries for SQLite (window functions enabled).
-- Schema: bookings(id, user_id, resource_id, starts_at, ends_at, cents)
--   - starts_at / ends_at are TEXT in ISO-8601 datetime form ("YYYY-MM-DD HH:MM:SS")
--   - cents is INTEGER — monetary amount in cents

-- ---------------------------------------------------------------------------
-- 1. Monthly revenue with running total and month-over-month delta
-- ---------------------------------------------------------------------------
-- The CTE collapses every booking into one row per calendar month (keyed by
-- "YYYY-MM") and sums the cents column. The outer query then layers three
-- window functions on top of those monthly aggregates:
--   * SUM(revenue_cents) OVER (ORDER BY month)  — cumulative running total
--   * LAG(revenue_cents) OVER (ORDER BY month) — previous month's revenue
--   * The delta and percentage columns are derived from those two values.
-- NULLIF guards the percentage against a divide-by-zero on the first row.
WITH monthly AS (
    SELECT
        strftime('%Y-%m', starts_at) AS month,
        SUM(cents)                    AS revenue_cents
    FROM bookings
    GROUP BY strftime('%Y-%m', starts_at)
)
SELECT
    month,
    revenue_cents,
    SUM(revenue_cents) OVER (ORDER BY month)                      AS running_total_cents,
    LAG(revenue_cents) OVER (ORDER BY month)                     AS prev_month_revenue_cents,
    revenue_cents - LAG(revenue_cents) OVER (ORDER BY month)     AS mom_delta_cents,
    ROUND(
        100.0 * (revenue_cents - LAG(revenue_cents) OVER (ORDER BY month))
        / NULLIF(LAG(revenue_cents) OVER (ORDER BY month), 0),
        2
    )                                                            AS mom_delta_pct
FROM monthly
ORDER BY month;

-- ---------------------------------------------------------------------------
-- 2. Per-resource utilisation ranking with RANK and NTILE
-- ---------------------------------------------------------------------------
-- Booked hours are the utilisation proxy: each booking's duration in hours is
-- (julianday(ends_at) - julianday(starts_at)) * 24, summed per resource.
--   * RANK() assigns a competitive rank (ties share a rank, next rank skips).
--   * NTILE(4) splits the resources into four quartiles by booked hours, so
--     quartile 1 is the busiest quarter and quartile 4 the least used.
-- Resources with zero bookings are excluded (they simply don't appear).
WITH resource_usage AS (
    SELECT
        resource_id,
        SUM((julianday(ends_at) - julianday(starts_at)) * 24.0) AS booked_hours,
        SUM(cents)                                              AS revenue_cents,
        COUNT(*)                                                AS booking_count
    FROM bookings
    GROUP BY resource_id
)
SELECT
    resource_id,
    booking_count,
    ROUND(booked_hours, 2)                                      AS booked_hours,
    revenue_cents,
    RANK()   OVER (ORDER BY booked_hours DESC)                  AS utilisation_rank,
    NTILE(4) OVER (ORDER BY booked_hours DESC)                  AS utilisation_quartile
FROM resource_usage
ORDER BY utilisation_rank;

-- ---------------------------------------------------------------------------
-- 3. Gap detection using LAG and LEAD
-- ---------------------------------------------------------------------------
-- For each resource the bookings are ordered by start time. Two window
-- functions reveal what sits immediately before and after every booking:
--   * LAG(ends_at)   — the end time of the preceding booking on the same
--                      resource. If the current start is later, the resource
--                      was idle in between (a gap).
--   * LEAD(starts_at) — the start time of the following booking on the same
--                      resource. If it falls before the current end, the two
--                      bookings overlap (a double-booking).
-- The final WHERE keeps only rows where at least one condition holds, and the
-- two CASE columns quantify the gap or overlap in seconds (86400 seconds per
-- Julian day).
WITH ordered_bookings AS (
    SELECT
        id,
        resource_id,
        starts_at,
        ends_at,
        LAG(ends_at)    OVER (PARTITION BY resource_id ORDER BY starts_at) AS prev_ends_at,
        LEAD(starts_at) OVER (PARTITION BY resource_id ORDER BY starts_at) AS next_starts_at
    FROM bookings
)
SELECT
    resource_id,
    id            AS booking_id,
    starts_at,
    ends_at,
    prev_ends_at,
    next_starts_at,
    CASE
        WHEN prev_ends_at IS NOT NULL AND starts_at > prev_ends_at
        THEN ROUND((julianday(starts_at) - julianday(prev_ends_at)) * 86400, 0)
    END                                                         AS gap_before_seconds,
    CASE
        WHEN next_starts_at IS NOT NULL AND next_starts_at < ends_at
        THEN ROUND((julianday(ends_at) - julianday(next_starts_at)) * 86400, 0)
    END                                                         AS overlap_after_seconds
FROM ordered_bookings
WHERE (prev_ends_at IS NOT NULL AND starts_at > prev_ends_at)
   OR (next_starts_at IS NOT NULL AND next_starts_at < ends_at)
ORDER BY resource_id, starts_at;
