-- reports.sql
-- Booking system analytics using SQLite window functions.
-- Schema: bookings(id, user_id, resource_id, starts_at, ends_at, cents)
-- starts_at and ends_at are TEXT in ISO-8601 datetime format
-- (e.g. '2024-03-15 14:00:00'), which SQLite's datetime functions expect.

-- ========================================================================
-- Query 1: Monthly revenue with running total and MoM delta
-- ========================================================================
-- Aggregates revenue (sum of cents) per calendar month, then applies two
-- window functions on top of that monthly summary:
--   - SUM() OVER (ORDER BY month) gives a running cumulative total across
--     all months, so you can see how revenue has accrued over time.
--   - LAG(revenue_cents) pulls the previous month's revenue, and subtracting
--     it from the current month yields the month-over-month delta (positive
--     = growth, negative = decline, NULL = first month in the data).
SELECT
    month,
    revenue_cents,
    -- Cumulative revenue from the first month through the current row
    SUM(revenue_cents) OVER (ORDER BY month) AS running_total_cents,
    -- Difference between this month and the previous month (NULL for first month)
    revenue_cents - LAG(revenue_cents) OVER (ORDER BY month) AS mom_delta_cents
FROM (
    SELECT
        strftime('%Y-%m', starts_at) AS month,
        SUM(cents) AS revenue_cents
    FROM bookings
    GROUP BY strftime('%Y-%m', starts_at)
) AS monthly
ORDER BY month;

-- ========================================================================
-- Query 2: Per-resource utilisation ranking with RANK and NTILE
-- ========================================================================
-- Computes total booked seconds per resource (sum of each booking's
-- duration), then ranks every resource by that total using two window
-- functions:
--   - RANK() assigns a competitive rank ordered by booked time descending.
--     Tied resources receive the same rank number and the next rank is
--     skipped (1, 1, 3, ...).
--   - NTILE(4) splits resources into four equally-sized buckets (quartiles)
--     from busiest (1) to least used (4), useful for a quick tier breakdown.
WITH per_resource AS (
    SELECT
        resource_id,
        SUM(
            CAST(
                (julianday(ends_at) - julianday(starts_at)) * 86400 AS INTEGER
            )
        ) AS total_booked_seconds
    FROM bookings
    GROUP BY resource_id
)
SELECT
    resource_id,
    total_booked_seconds,
    RANK() OVER (ORDER BY total_booked_seconds DESC) AS util_rank,
    NTILE(4) OVER (ORDER BY total_booked_seconds DESC) AS util_quartile
FROM per_resource
ORDER BY util_rank;

-- ========================================================================
-- Query 3: Gap detection with LAG and LEAD
-- ========================================================================
-- For every booking, computes the idle time (gap) on the same resource
-- before the booking starts and after it ends:
--   - LAG(ends_at) pulls the end time of the immediately preceding booking
--     on the same resource. The difference between this booking's start and
--     that previous end time is the idle gap before (NULL for the first
--     booking on a resource).
--   - LEAD(starts_at) pulls the start time of the next booking on the same
--     resource. The difference between that next start and this booking's
--     end time is the idle gap after (NULL for the last booking on a resource).
--   - A positive gap means the resource sat idle. A negative gap means
--     overlapping bookings. Zero means back-to-back bookings with no slack.
WITH enriched AS (
    SELECT
        id,
        resource_id,
        starts_at,
        ends_at,
        LAG(ends_at) OVER (
            PARTITION BY resource_id ORDER BY starts_at
        ) AS prev_ends_at,
        LEAD(starts_at) OVER (
            PARTITION BY resource_id ORDER BY starts_at
        ) AS next_starts_at
    FROM bookings
)
SELECT
    id,
    resource_id,
    starts_at,
    ends_at,
    CAST(
        (julianday(starts_at) - julianday(prev_ends_at)) * 86400 AS INTEGER
    ) AS gap_before_seconds,
    CAST(
        (julianday(next_starts_at) - julianday(ends_at)) * 86400 AS INTEGER
    ) AS gap_after_seconds
FROM enriched
ORDER BY resource_id, starts_at;
