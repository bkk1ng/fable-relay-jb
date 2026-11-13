-- Schema reference:
--   bookings(id, user_id, resource_id, starts_at, ends_at, cents)
--
-- Assumptions:
--   starts_at / ends_at are DATETIME (or TIMESTAMP) values.
--   cents is an INTEGER representing the booking amount in cents.
--   SQLite 3.25+ window functions are available.


-- ============================================================
-- 1. Monthly revenue with running total and month-over-month delta
-- ============================================================
-- We aggregate bookings by calendar month, then use window functions
-- to build a cumulative (running) total and the difference vs. the
-- prior month.  LAG looks back one row in the month-ordered partition.

WITH monthly AS (
    SELECT
        -- Normalise to YYYY-MM so rows sort chronologically as strings
        strftime('%Y-%m', starts_at)       AS month,
        SUM(cents)                         AS revenue_cents
    FROM bookings
    GROUP BY strftime('%Y-%m', starts_at)
)
SELECT
    month,
    revenue_cents,

    -- Running total: sum every row up to and including the current month
    SUM(revenue_cents) OVER (
        ORDER BY month
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )                                      AS running_total_cents,

    -- Month-over-month delta: current month minus the previous month.
    -- NULL for the first month in the series (no prior row exists).
    revenue_cents - LAG(revenue_cents, 1) OVER (
        ORDER BY month
    )                                      AS mom_delta_cents

FROM monthly
ORDER BY month;


-- ============================================================
-- 2. Per-resource utilisation ranking with RANK and NTILE
-- ============================================================
-- "Utilisation" is defined here as total booked seconds per resource.
-- RANK assigns the same rank to ties (leaving gaps); NTILE(4) bins
-- resources into four equal quartiles (Q1 = highest utilisation).

WITH resource_util AS (
    SELECT
        resource_id,
        -- Total seconds booked across all bookings for this resource
        SUM(
            strftime('%s', ends_at) - strftime('%s', starts_at)
        )                                  AS total_booked_seconds,
        COUNT(*)                           AS booking_count,
        SUM(cents)                         AS total_revenue_cents
    FROM bookings
    GROUP BY resource_id
)
SELECT
    resource_id,
    total_booked_seconds,
    booking_count,
    total_revenue_cents,

    -- Dense utilisation rank: 1 = most utilised.
    -- RANK (not DENSE_RANK) intentionally leaves gaps after ties so
    -- callers can detect ties by looking for repeated rank values.
    RANK() OVER (
        ORDER BY total_booked_seconds DESC
    )                                      AS utilisation_rank,

    -- Quartile bucket: 1 = top 25 %, 4 = bottom 25 %.
    -- NTILE distributes rows as evenly as possible; with fewer than
    -- four resources some buckets may be empty.
    NTILE(4) OVER (
        ORDER BY total_booked_seconds DESC
    )                                      AS utilisation_quartile

FROM resource_util
ORDER BY utilisation_rank;


-- ============================================================
-- 3. Gap detection: find unbooked intervals between bookings
-- ============================================================
-- For each resource we look at consecutive bookings sorted by
-- starts_at.  A gap exists whenever the next booking's starts_at
-- is later than the current booking's ends_at.
--
-- LAG  retrieves the previous booking's ends_at so we can find
--      the start of a gap.
-- LEAD retrieves the next booking's starts_at so we can find
--      the end of a gap.
--
-- Both values are NULL at the boundary rows; those rows are
-- filtered out in the outer query.

WITH ordered_bookings AS (
    SELECT
        id                                     AS booking_id,
        resource_id,
        starts_at,
        ends_at,

        -- When did the previous booking for this resource end?
        LAG(ends_at) OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        )                                      AS prev_ends_at,

        -- When does the next booking for this resource start?
        LEAD(starts_at) OVER (
            PARTITION BY resource_id
            ORDER BY starts_at
        )                                      AS next_starts_at
    FROM bookings
),
gaps AS (
    SELECT
        resource_id,

        -- The gap starts when the previous booking ended ...
        prev_ends_at                           AS gap_start,

        -- ... and ends when this booking begins.
        starts_at                              AS gap_end,

        -- Gap duration in seconds for easy filtering/sorting
        strftime('%s', starts_at)
            - strftime('%s', prev_ends_at)     AS gap_seconds

    FROM ordered_bookings
    WHERE
        -- Only rows that have a predecessor (not the first booking)
        prev_ends_at IS NOT NULL

        -- Only rows where there is actually a gap (no overlap/adjacency)
        AND starts_at > prev_ends_at
)
SELECT
    resource_id,
    gap_start,
    gap_end,
    gap_seconds,

    -- Human-friendly: gap expressed as HH:MM:SS
    -- SQLite has no built-in interval type; we format manually.
    printf(
        '%02d:%02d:%02d',
        gap_seconds / 3600,
        (gap_seconds % 3600) / 60,
        gap_seconds % 60
    )                                          AS gap_duration_hhmmss

FROM gaps
ORDER BY resource_id, gap_start;
