-- Schema reference:
--   bookings(id, user_id, resource_id, starts_at, ends_at, cents)
--
-- All three queries assume SQLite with window-function support (3.25+).


-- ─────────────────────────────────────────────────────────────────────────────
-- 1. Monthly revenue: running total and month-over-month delta
-- ─────────────────────────────────────────────────────────────────────────────

WITH monthly AS (
    SELECT
        -- Truncate to the first day of each calendar month
        STRFTIME('%Y-%m-01', starts_at)                              AS month,
        SUM(cents)                                                   AS revenue_cents
    FROM bookings
    GROUP BY month
)
SELECT
    month,
    revenue_cents,

    -- Cumulative sum of revenue up to and including the current month,
    -- ordered chronologically.  ROWS UNBOUNDED PRECEDING ensures every
    -- prior month is included even when gaps exist.
    SUM(revenue_cents) OVER (
        ORDER BY month
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )                                                                AS running_total_cents,

    -- Absolute change vs. the immediately preceding month.
    -- LAG with no offset defaults to 1 row back; NULL for the first row.
    revenue_cents - LAG(revenue_cents) OVER (ORDER BY month)        AS mom_delta_cents,

    -- Percentage change vs. the preceding month, rounded to two decimal
    -- places.  NULLIF guards against division-by-zero when prior revenue
    -- was 0.
    ROUND(
        100.0 * (revenue_cents - LAG(revenue_cents) OVER (ORDER BY month))
        / NULLIF(LAG(revenue_cents) OVER (ORDER BY month), 0),
        2
    )                                                                AS mom_delta_pct
FROM monthly
ORDER BY month;


-- ─────────────────────────────────────────────────────────────────────────────
-- 2. Per-resource utilisation ranking (RANK + NTILE)
-- ─────────────────────────────────────────────────────────────────────────────

WITH resource_stats AS (
    SELECT
        resource_id,
        COUNT(*)                                                     AS booking_count,
        SUM(cents)                                                   AS total_revenue_cents,

        -- Total booked seconds as a proxy for utilisation.
        -- UNIXEPOCH returns integer seconds in SQLite 3.38+; fall back to
        -- (JULIANDAY(ends_at) - JULIANDAY(starts_at)) * 86400 if needed.
        SUM(UNIXEPOCH(ends_at) - UNIXEPOCH(starts_at))              AS booked_seconds
    FROM bookings
    GROUP BY resource_id
)
SELECT
    resource_id,
    booking_count,
    total_revenue_cents,
    booked_seconds,

    -- Dense rank is NOT used here deliberately: RANK leaves gaps after ties,
    -- which makes the rank value meaningful as an absolute position.
    RANK()  OVER (ORDER BY booked_seconds DESC)                      AS utilisation_rank,

    -- Divide resources into four equal buckets by utilisation.
    -- Bucket 1 = highest 25 %, bucket 4 = lowest 25 %.
    NTILE(4) OVER (ORDER BY booked_seconds DESC)                     AS utilisation_quartile,

    -- Rank purely by revenue for a secondary perspective
    RANK()  OVER (ORDER BY total_revenue_cents DESC)                 AS revenue_rank
FROM resource_stats
ORDER BY utilisation_rank;


-- ─────────────────────────────────────────────────────────────────────────────
-- 3. Gap detection: find unbooked intervals for each resource
--    using LAG / LEAD
-- ─────────────────────────────────────────────────────────────────────────────

WITH booking_window AS (
    SELECT
        resource_id,
        starts_at,
        ends_at,

        -- The end time of the previous booking for the same resource.
        -- When this is NULL the current booking is the first one.
        LAG(ends_at)  OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        )                                                            AS prev_ends_at,

        -- The start time of the next booking for the same resource.
        -- When this is NULL the current booking is the last one.
        LEAD(starts_at) OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        )                                                            AS next_starts_at
    FROM bookings
)
SELECT
    resource_id,

    -- A gap BEFORE this booking: the resource was idle between the
    -- previous booking's end and this booking's start.
    prev_ends_at                                                     AS gap_start,
    starts_at                                                        AS gap_end,
    ROUND(
        (UNIXEPOCH(starts_at) - UNIXEPOCH(prev_ends_at)) / 60.0,
        1
    )                                                                AS gap_minutes_before,

    -- The booking that the gap leads into
    starts_at                                                        AS next_booking_start,
    ends_at                                                          AS next_booking_end

FROM booking_window
WHERE
    -- Only emit a row when an actual gap exists (prev booking ended
    -- before this one starts, with at least 1 second between them).
    prev_ends_at IS NOT NULL
    AND UNIXEPOCH(starts_at) > UNIXEPOCH(prev_ends_at)

ORDER BY resource_id, gap_start;
