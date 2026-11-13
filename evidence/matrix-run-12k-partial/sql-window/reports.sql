-- Schema assumed:
--   bookings(id, user_id, resource_id, starts_at, ends_at, cents)
--
-- All queries target SQLite with window-function support (3.25+).

-- ============================================================
-- 1. Monthly revenue: running total + month-over-month delta
-- ============================================================
-- Aggregate cents into calendar months, then use SUM and LAG
-- as window functions over the ordered month sequence to
-- produce a cumulative total and the absolute change from the
-- previous month without a self-join.

WITH monthly AS (
    SELECT
        strftime('%Y-%m', starts_at)        AS month,
        SUM(cents)                          AS revenue_cents
    FROM bookings
    GROUP BY month
)
SELECT
    month,
    revenue_cents,

    -- Running total: SUM over all rows up to and including
    -- the current month, ordered chronologically.
    SUM(revenue_cents) OVER (
        ORDER BY month
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )                                       AS running_total_cents,

    -- Month-over-month delta: current month minus the previous
    -- month's revenue.  NULL for the first month in the set.
    revenue_cents - LAG(revenue_cents) OVER (
        ORDER BY month
    )                                       AS mom_delta_cents

FROM monthly
ORDER BY month;


-- ============================================================
-- 2. Per-resource utilisation ranking (RANK + NTILE)
-- ============================================================
-- "Utilisation" is modelled as the total booked duration in
-- seconds across all bookings for the resource.  RANK assigns
-- a dense-style rank (ties share a rank, next rank skips),
-- and NTILE(4) places each resource into a performance
-- quartile (1 = top 25 %).

WITH resource_utilisation AS (
    SELECT
        resource_id,
        COUNT(*)                                        AS booking_count,
        SUM(
            (julianday(ends_at) - julianday(starts_at))
            * 86400                                     -- convert days → seconds
        )                                               AS total_booked_seconds
    FROM bookings
    GROUP BY resource_id
)
SELECT
    resource_id,
    booking_count,
    ROUND(total_booked_seconds / 3600.0, 2)             AS total_booked_hours,

    -- RANK: resources with equal utilisation share the same
    -- rank; the next distinct value gets a rank that reflects
    -- the gap (e.g. 1, 2, 2, 4).
    RANK() OVER (
        ORDER BY total_booked_seconds DESC
    )                                                   AS utilisation_rank,

    -- NTILE(4): divides the result set into four equal buckets
    -- ordered by utilisation descending.  Bucket 1 is the
    -- highest-utilisation quartile.
    NTILE(4) OVER (
        ORDER BY total_booked_seconds DESC
    )                                                   AS utilisation_quartile

FROM resource_utilisation
ORDER BY utilisation_rank;


-- ============================================================
-- 3. Gap detection between consecutive bookings (LAG / LEAD)
-- ============================================================
-- For each booking, find the gap in seconds between it and
-- the booking that immediately precedes it for the same
-- resource (via LAG) and the one that immediately follows it
-- (via LEAD).  A negative gap_after_prev_seconds would
-- indicate an overlap; a large positive value flags
-- under-utilised windows.

SELECT
    id                                                  AS booking_id,
    resource_id,
    starts_at,
    ends_at,

    -- Time between the previous booking's end and this
    -- booking's start, for the same resource.
    -- NULL for the first booking of each resource.
    ROUND(
        (julianday(starts_at)
         - julianday(
             LAG(ends_at) OVER (
                 PARTITION BY resource_id
                 ORDER BY starts_at
             )
           )
        ) * 86400,
        1
    )                                                   AS gap_after_prev_seconds,

    -- Time between this booking's end and the next booking's
    -- start, for the same resource.
    -- NULL for the last booking of each resource.
    ROUND(
        (julianday(
             LEAD(starts_at) OVER (
                 PARTITION BY resource_id
                 ORDER BY starts_at
             )
           )
         - julianday(ends_at)
        ) * 86400,
        1
    )                                                   AS gap_before_next_seconds

FROM bookings
ORDER BY resource_id, starts_at;
