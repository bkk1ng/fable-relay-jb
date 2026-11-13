/*
 * progress.c — reusable ANSI progress bar (C11, POSIX)
 *
 * Features:
 *   - Determinate mode  : shows percentage, filled/empty bar, throughput, ETA
 *   - Indeterminate mode: animated spinner + bouncing fill
 *   - Throughput + ETA estimation via exponential smoothing
 *   - Terminal width detection via ioctl(TIOCGWINSZ)
 *   - Carriage-return redraw (no newline spam)
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -o progress progress.c
 *
 * Usage (library):
 *   progress_t p;
 *   progress_init(&p, 200, PROGRESS_DETERMINATE);
 *   for (int i = 0; i <= 200; i++) {
 *       progress_update(&p, i);
 *       // ... do work ...
 *   }
 *   progress_finish(&p);
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef enum {
    PROGRESS_DETERMINATE,
    PROGRESS_INDETERMINATE
} progress_mode_t;

typedef struct {
    /* configuration */
    uint64_t        total;          /* total items (0 = indeterminate) */
    progress_mode_t mode;

    /* state */
    uint64_t        current;        /* items completed so far           */
    uint64_t        ticks;          /* raw update count                 */

    /* timing */
    struct timespec start_ts;       /* wall time at progress_init()     */
    struct timespec last_ts;        /* wall time at last update         */

    /* exponential smoothing for throughput (items/sec) */
    double          ema_rate;       /* smoothed rate                    */
    bool            ema_seeded;

    /* indeterminate animation */
    int             spinner_idx;
    int             bounce_pos;
    int             bounce_dir;     /* +1 or -1                         */
} progress_t;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Returns seconds elapsed between two timespecs */
static double ts_diff(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec  - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) * 1e-9;
}

/* Queries the current terminal column width; returns 80 on failure */
static int terminal_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 80;
}

/* Formats a duration in seconds into "1h02m03s", "2m03s", or "3s" */
static void format_duration(char *buf, size_t bufsz, double secs)
{
    if (secs < 0 || !isfinite(secs)) {
        snprintf(buf, bufsz, "--:--");
        return;
    }
    uint64_t s = (uint64_t)secs;
    uint64_t m = s / 60; s %= 60;
    uint64_t h = m / 60; m %= 60;
    if (h > 0)
        snprintf(buf, bufsz, "%luh%02lum%02lus", (unsigned long)h,
                 (unsigned long)m, (unsigned long)s);
    else if (m > 0)
        snprintf(buf, bufsz, "%lum%02lus", (unsigned long)m,
                 (unsigned long)s);
    else
        snprintf(buf, bufsz, "%lus", (unsigned long)s);
}

/* Formats throughput as "1234.5/s", "12.3k/s", or "1.2M/s" */
static void format_rate(char *buf, size_t bufsz, double rate)
{
    if (rate < 0 || !isfinite(rate)) {
        snprintf(buf, bufsz, "?/s");
        return;
    }
    if (rate >= 1e6)
        snprintf(buf, bufsz, "%.1fM/s", rate / 1e6);
    else if (rate >= 1e3)
        snprintf(buf, bufsz, "%.1fk/s", rate / 1e3);
    else
        snprintf(buf, bufsz, "%.1f/s", rate);
}

/* -------------------------------------------------------------------------
 * Determinate redraw
 *
 * Layout (fits in `width` columns):
 *   " NNN% [########....] 123/200  12.3k/s  ETA 1m23s "
 * ---------------------------------------------------------------------- */
static void draw_determinate(const progress_t *p, int width)
{
    double frac = (p->total > 0)
                ? (double)p->current / (double)p->total
                : 0.0;
    if (frac > 1.0) frac = 1.0;

    char rate_buf[16];
    format_rate(rate_buf, sizeof(rate_buf), p->ema_rate);

    char eta_buf[16];
    if (p->current >= p->total) {
        /* finished — show elapsed */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = ts_diff(&p->start_ts, &now);
        format_duration(eta_buf, sizeof(eta_buf), elapsed);
    } else if (p->ema_seeded && p->ema_rate > 0.0) {
        double remaining = (double)(p->total - p->current) / p->ema_rate;
        format_duration(eta_buf, sizeof(eta_buf), remaining);
    } else {
        snprintf(eta_buf, sizeof(eta_buf), "--:--");
    }

    /* Right-hand annotation: "  123/200  12.3k/s  ETA 1m23s" */
    char right[64];
    int rlen = snprintf(right, sizeof(right),
                        "  %llu/%llu  %s  ETA %s",
                        (unsigned long long)p->current,
                        (unsigned long long)p->total,
                        rate_buf, eta_buf);

    /* Left-hand: " NNN% [" = 7 chars, then "]" = 1 char */
    int bar_width = width - 7 - 1 - rlen;
    if (bar_width < 4) bar_width = 4;

    int filled = (int)(frac * (double)bar_width);
    if (filled > bar_width) filled = bar_width;

    /* Build the bar string */
    char *bar = malloc((size_t)bar_width + 1);
    if (!bar) return;
    for (int i = 0; i < bar_width; i++)
        bar[i] = (i < filled) ? '#' : '.';
    bar[bar_width] = '\0';

    int pct = (int)(frac * 100.0);

    printf("\r\033[K %3d%% [%s]%s", pct, bar, right);
    fflush(stdout);

    free(bar);
}

/* -------------------------------------------------------------------------
 * Indeterminate redraw
 *
 * Layout: " ⠋ [   ####   ] (bouncing)  12.3k/s "
 * ---------------------------------------------------------------------- */
static void draw_indeterminate(progress_t *p, int width)
{
    static const char *spinners[] = { "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏" };
    static const int  NSPIN       = 10;

    const char *spin = spinners[p->spinner_idx % NSPIN];

    char rate_buf[16];
    format_rate(rate_buf, sizeof(rate_buf), p->ema_rate);

    char right[32];
    int rlen = snprintf(right, sizeof(right), "  %s", rate_buf);

    /* " S [" = 4 chars, "]" = 1 */
    int bar_width = width - 4 - 1 - rlen;
    if (bar_width < 6) bar_width = 6;

    /* Bouncing fill: 25% of bar_width */
    int fill_len = bar_width / 4;
    if (fill_len < 2) fill_len = 2;

    char *bar = calloc((size_t)bar_width + 1, 1);
    if (!bar) return;
    memset(bar, '.', (size_t)bar_width);

    int pos = p->bounce_pos;
    for (int i = 0; i < fill_len && (pos + i) < bar_width; i++)
        bar[pos + i] = '#';
    bar[bar_width] = '\0';

    printf("\r\033[K %s [%s]%s", spin, bar, right);
    fflush(stdout);

    /* Advance bounce */
    p->bounce_pos += p->bounce_dir;
    if (p->bounce_pos + fill_len >= bar_width) {
        p->bounce_pos = bar_width - fill_len;
        p->bounce_dir = -1;
    } else if (p->bounce_pos <= 0) {
        p->bounce_pos = 0;
        p->bounce_dir = +1;
    }

    p->spinner_idx++;
    free(bar);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * progress_init — initialise a progress_t.
 *
 *   total : number of items (use 0 or any value with PROGRESS_INDETERMINATE)
 *   mode  : PROGRESS_DETERMINATE | PROGRESS_INDETERMINATE
 */
void progress_init(progress_t *p, uint64_t total, progress_mode_t mode)
{
    memset(p, 0, sizeof(*p));
    p->total      = total;
    p->mode       = mode;
    p->bounce_dir = +1;
    clock_gettime(CLOCK_MONOTONIC, &p->start_ts);
    p->last_ts    = p->start_ts;
}

/*
 * progress_update — call after each completed item (or on each tick for
 *                   indeterminate mode).
 *
 *   current : items completed so far (ignored in INDETERMINATE mode but
 *             used for throughput if you pass the actual count anyway)
 */
void progress_update(progress_t *p, uint64_t current)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    p->ticks++;

    /* --- throughput estimation ---------------------------------------- */
    double dt = ts_diff(&p->last_ts, &now);
    if (dt > 0.0) {
        uint64_t delta = (current > p->current) ? (current - p->current) : 1;
        double   inst  = (double)delta / dt;

        /* Exponential moving average, α = 0.2 — more weight on recent data */
        const double ALPHA = 0.2;
        if (!p->ema_seeded) {
            p->ema_rate  = inst;
            p->ema_seeded = true;
        } else {
            p->ema_rate = ALPHA * inst + (1.0 - ALPHA) * p->ema_rate;
        }
    }

    p->current = current;
    p->last_ts = now;

    int w = terminal_width();

    if (p->mode == PROGRESS_DETERMINATE)
        draw_determinate(p, w);
    else
        draw_indeterminate(p, w);
}

/*
 * progress_finish — print a final newline so the cursor lands on a fresh
 *                   line.  Optionally call progress_update(p, total) first
 *                   to show 100%.
 */
void progress_finish(const progress_t *p)
{
    (void)p;
    printf("\n");
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Demo
 * ---------------------------------------------------------------------- */

/*
 * Portable nanosleep wrapper — suspends for `ms` milliseconds.
 */
static void sleep_ms(long ms)
{
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

int main(void)
{
    const uint64_t TOTAL = 200;

    /* ------------------------------------------------------------------ */
    printf("=== Determinate mode (%llu items) ===\n",
           (unsigned long long)TOTAL);

    progress_t p;
    progress_init(&p, TOTAL, PROGRESS_DETERMINATE);

    for (uint64_t i = 0; i <= TOTAL; i++) {
        progress_update(&p, i);

        /* Simulate variable-speed work: fast in the middle, slow at ends */
        long delay;
        if (i < 20 || i > 180)
            delay = 40;
        else if (i < 60 || i > 140)
            delay = 20;
        else
            delay = 8;
        sleep_ms(delay);
    }

    progress_finish(&p);
    printf("Done.\n\n");

    /* ------------------------------------------------------------------ */
    printf("=== Indeterminate mode (3 seconds) ===\n");

    progress_t q;
    progress_init(&q, 0, PROGRESS_INDETERMINATE);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t ticks = 0;

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_diff(&t0, &now) >= 3.0) break;

        progress_update(&q, ++ticks);
        sleep_ms(80);
    }

    progress_finish(&q);
    printf("Done.\n");

    return 0;
}
