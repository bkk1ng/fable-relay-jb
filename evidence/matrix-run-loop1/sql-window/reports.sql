-- Schema assumed:
--   bookings(id, user_id, resource_id, starts_at, ends_at, cents)
--
-- All queries require SQLite 3.25+ (window function support).

-- ============================================================
-- 1. Monthly revenue: running total and month-over-month delta
-- ============================================================

WITH monthly AS (
    -- Bucket each booking into its calendar month and sum revenue.
    SELECT
        strftime('%Y-%m', starts_at)        AS month,
        SUM(cents)                           AS revenue_cents
    FROM bookings
    GROUP BY strftime('%Y-%m', starts_at)
)
SELECT
    month,
    revenue_cents,

    -- Running total: sum every month up to and including the current row,
    -- ordered chronologically.
    SUM(revenue_cents) OVER (
        ORDER BY month
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )                                        AS running_total_cents,

    -- Month-over-month delta: current month minus the previous month.
    -- LAG(..., 1) looks one row back in the same chronological order.
    revenue_cents - LAG(revenue_cents, 1) OVER (
        ORDER BY month
    )                                        AS mom_delta_cents

FROM monthly
ORDER BY month;


-- ============================================================
-- 2. Per-resource utilisation ranking (RANK and NTILE)
-- ============================================================

WITH resource_hours AS (
    -- Total booked hours per resource across all time.
    -- (ends_at - starts_at) in SQLite returns a real value in days when
    -- both columns are ISO-8601 strings; multiply by 24 to get hours.
    SELECT
        resource_id,
        SUM(
            (julianday(ends_at) - julianday(starts_at)) * 24.0
        )                                    AS booked_hours
    FROM bookings
    GROUP BY resource_id
)
SELECT
    resource_id,
    ROUND(booked_hours, 2)                   AS booked_hours,

    -- RANK: resources with equal hours share a rank; the next rank is
    -- gapped (1, 2, 2, 4 ...).  Highest utilisation ranks first.
    RANK() OVER (
        ORDER BY booked_hours DESC
    )                                        AS utilisation_rank,

    -- NTILE(4): divide resources into four equal-sized quartiles.
    -- Quartile 1 = most utilised, quartile 4 = least utilised.
    NTILE(4) OVER (
        ORDER BY booked_hours DESC
    )                                        AS utilisation_quartile

FROM resource_hours
ORDER BY utilisation_rank;


-- ============================================================
-- 3. Gap detection: idle windows between consecutive bookings
-- ============================================================
-- A "gap" is a period where a resource has no active booking.
-- We find gaps by comparing each booking's starts_at against the
-- ends_at of the previous booking on the same resource.

WITH ordered AS (
    -- Attach the previous booking's end time for every row,
    -- partitioned per resource so we never compare across resources.
    SELECT
        id,
        resource_id,
        starts_at,
        ends_at,

        -- LAG: end time of the booking immediately before this one.
        LAG(ends_at) OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        )                                    AS prev_ends_at,

        -- LEAD: start time of the booking immediately after this one
        -- (useful for detecting a trailing gap after the last booking).
        LEAD(starts_at) OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        )                                    AS next_starts_at

    FROM bookings
)
SELECT
    resource_id,

    -- The gap runs from the end of the previous booking to the start
    -- of the current one.
    prev_ends_at                             AS gap_starts_at,
    starts_at                                AS gap_ends_at,

    -- Gap duration in hours for quick inspection.
    ROUND(
        (julianday(starts_at) - julianday(prev_ends_at)) * 24.0,
        2
    )                                        AS gap_hours

FROM ordered
WHERE
    -- Only emit rows where there actually is a preceding booking
    -- and the gap is positive (overlapping bookings would be negative).
    prev_ends_at IS NOT NULL
    AND starts_at > prev_ends_at

ORDER BY resource_id, gap_starts_at;
