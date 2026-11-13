/*
 * progress.c — reusable ANSI terminal progress bar
 *
 * Features:
 *   - Determinate mode  (known total: percentage, ETA)
 *   - Indeterminate mode (unknown total: bouncing slider)
 *   - Throughput + ETA with exponential smoothing (α = 0.15)
 *   - Terminal width via ioctl(TIOCGWINSZ), fallback 80
 *   - Carriage-return in-place redraw, no scrolling
 *
 * Build:  cc -std=c11 -Wall -Wextra -o progress progress.c
 * Run:    ./progress
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* =========================================================================
 * Public types
 * ========================================================================= */

typedef enum {
    PROGRESS_DETERMINATE,   /* known total — renders % + ETA  */
    PROGRESS_INDETERMINATE  /* unknown total — bouncing slider */
} progress_mode_t;

typedef struct {
    progress_mode_t mode;

    /* determinate */
    long            total;          /* expected item count            */
    long            done;           /* items completed so far         */

    /* wall-clock timing */
    struct timespec t_start;        /* time at progress_init()        */
    struct timespec t_last;         /* time of last delta > 0 update  */

    /* exponential-smoothed throughput (items/s) */
    double          rate_smooth;
    int             rate_valid;     /* 0 until first measurement      */

    /* indeterminate bounce state */
    int             bounce_pos;     /* leading-edge column of slider  */
    int             bounce_dir;     /* +1 right, -1 left              */

    /* display */
    const char     *label;          /* short prefix string, or NULL   */
} progress_t;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Elapsed seconds between two CLOCK_MONOTONIC snapshots. */
static double elapsed_sec(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec  - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) * 1e-9;
}

/* Terminal column width via ioctl; fallback 80. */
static int terminal_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 10)
        return (int)ws.ws_col;
    return 80;
}

/*
 * Format seconds as "MM:SS" (capped at 99:59).
 * Writes "??:??" for values that are negative or unrepresentably large.
 */
static void fmt_eta(double secs, char *buf, size_t bufsz)
{
    if (secs < 0.0 || secs > 5999.0) {
        snprintf(buf, bufsz, "??:??");
        return;
    }
    int s = (int)secs;
    snprintf(buf, bufsz, "%02d:%02d", s / 60, s % 60);
}

/*
 * Format a throughput rate as "NNN.N/s".
 * Writes "    -/s" when rate is not yet available (< 0).
 */
static void fmt_rate(double rate, char *buf, size_t bufsz)
{
    if (rate < 0.0) {
        snprintf(buf, bufsz, "    -/s");
        return;
    }
    if (rate >= 1000.0)
        snprintf(buf, bufsz, "%5.0f/s", rate);
    else if (rate >= 10.0)
        snprintf(buf, bufsz, "%5.1f/s", rate);
    else
        snprintf(buf, bufsz, "%5.2f/s", rate);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * Initialise a progress bar.
 *
 *   p      Caller-allocated progress_t (any storage class is fine).
 *   mode   PROGRESS_DETERMINATE   — total must be > 0
 *          PROGRESS_INDETERMINATE — total is ignored
 *   total  Expected item count (determinate only; pass 0 otherwise).
 *   label  Short prefix shown to the left of the bar, or NULL.
 */
void progress_init(progress_t *p, progress_mode_t mode, long total,
                   const char *label)
{
    memset(p, 0, sizeof(*p));
    p->mode       = mode;
    p->total      = (mode == PROGRESS_DETERMINATE && total > 0) ? total : 0;
    p->label      = label;
    p->bounce_dir = 1;
    clock_gettime(CLOCK_MONOTONIC, &p->t_start);
    p->t_last = p->t_start;
}

/*
 * Advance the counter by delta_items and redraw the bar.
 *
 * Pass delta_items = 0 to force a redraw without advancing (useful for
 * the final call in progress_finish, or for time-based refresh).
 *
 * Throughput smoothing uses α = 0.15:
 *   rate = α × instant_rate + (1 − α) × rate_prev
 *
 * A lower α produces a smoother (slower-reacting) estimate; 0.10–0.20
 * is a practical range for sub-second updates.
 */
#define PROGRESS_SMOOTH_ALPHA 0.15

void progress_update(progress_t *p, long delta_items)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* ---- update counter and smoothed rate ------------------------------ */
    if (delta_items > 0) {
        p->done += delta_items;

        double dt = elapsed_sec(&p->t_last, &now);
        if (dt > 1e-9) {
            double instant = (double)delta_items / dt;
            if (!p->rate_valid) {
                p->rate_smooth = instant;
                p->rate_valid  = 1;
            } else {
                p->rate_smooth = PROGRESS_SMOOTH_ALPHA * instant
                               + (1.0 - PROGRESS_SMOOTH_ALPHA) * p->rate_smooth;
            }
        }
        p->t_last = now;
    }

    /* ---- layout -------------------------------------------------------- */
    int tw = terminal_width();

    /* Right-side annotation. Build it first so we know its width. */
    char ann[64];
    char rate_s[16];
    fmt_rate(p->rate_valid ? p->rate_smooth : -1.0, rate_s, sizeof(rate_s));

    if (p->mode == PROGRESS_DETERMINATE) {
        double frac = (p->total > 0)
                      ? (double)p->done / (double)p->total
                      : 0.0;
        if (frac > 1.0) frac = 1.0;

        double eta_secs = -1.0;
        if (p->rate_valid && p->rate_smooth > 1e-9 && p->done < p->total)
            eta_secs = (double)(p->total - p->done) / p->rate_smooth;

        char eta_s[8];
        fmt_eta(eta_secs, eta_s, sizeof(eta_s));
        snprintf(ann, sizeof(ann), " %3.0f%% %s ETA %s",
                 frac * 100.0, rate_s, eta_s);
    } else {
        /* Show raw item count — no percentage possible. */
        snprintf(ann, sizeof(ann), " %7ld items %s", p->done, rate_s);
    }

    /* Left-side label (truncated to 12 chars, padded). */
    char prefix[32] = "";
    if (p->label)
        snprintf(prefix, sizeof(prefix), "%-12.12s ", p->label);

    int prefix_w = (int)strlen(prefix);
    int ann_w    = (int)strlen(ann);

    /*
     * Bar occupies what's left: prefix [ <inner> ] ann
     * Keep inner at minimum 4 so the bar is always visible.
     */
    int bar_inner = tw - prefix_w - 2 /* brackets */ - ann_w;
    if (bar_inner < 4) bar_inner = 4;

    /* Use a fixed max-width stack buffer; no terminal is 1 024 columns. */
    if (bar_inner > 1020) bar_inner = 1020;

    char bar[1024];
    memset(bar, ' ', (size_t)bar_inner);
    bar[bar_inner] = '\0';

    /* ---- fill the bar -------------------------------------------------- */
    if (p->mode == PROGRESS_DETERMINATE) {
        double frac = (p->total > 0)
                      ? (double)p->done / (double)p->total
                      : 0.0;
        if (frac > 1.0) frac = 1.0;
        int filled = (int)(frac * (double)bar_inner);
        if (filled > bar_inner) filled = bar_inner;
        if (filled > 0) memset(bar, '=', (size_t)filled);
        /* Arrow head: > one column past the fill, unless 100%. */
        if (filled < bar_inner && frac < 1.0)
            bar[filled] = '>';
    } else {
        /*
         * Bouncing slider: width is ~12% of the bar, minimum 3 chars.
         * Advance one column per call; reverse direction at both ends.
         */
        int slider = bar_inner / 8;
        if (slider < 3)         slider = 3;
        if (slider > bar_inner) slider = bar_inner;

        int max_pos = bar_inner - slider;
        if (max_pos < 0) max_pos = 0;

        p->bounce_pos += p->bounce_dir;
        if (p->bounce_pos >= max_pos) {
            p->bounce_pos = max_pos;
            p->bounce_dir = -1;
        } else if (p->bounce_pos <= 0) {
            p->bounce_pos = 0;
            p->bounce_dir = 1;
        }

        for (int i = p->bounce_pos;
             i < p->bounce_pos + slider && i < bar_inner; ++i)
            bar[i] = '=';
    }

    /* ---- render -------------------------------------------------------- */
    /* \r returns to column 0; omitting \n keeps us on the same line.      */
    printf("\r%s[%s]%s", prefix, bar, ann);
    fflush(stdout);
}

/*
 * Finalise the progress bar.
 *
 * Forces one last redraw (shows 100% in determinate mode), then prints a
 * newline and a one-line elapsed-time + average-throughput summary.
 */
void progress_finish(progress_t *p)
{
    progress_update(p, 0);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double total_secs = elapsed_sec(&p->t_start, &now);

    char rate_s[16];
    double avg = (p->done > 0 && total_secs > 1e-9)
                 ? (double)p->done / total_secs : 0.0;
    fmt_rate(avg, rate_s, sizeof(rate_s));

    printf("\n  finished: %ld items in %.2f s  avg %s\n",
           p->done, total_secs, rate_s);
}

/* =========================================================================
 * Demo
 * ========================================================================= */

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(void)
{
    progress_t p;

    /* ------------------------------------------------------------------ */
    /* Determinate: 200 items, variable speed to exercise ETA smoothing.  */
    /* ------------------------------------------------------------------ */
    printf("Determinate mode (200 items):\n");
    progress_init(&p, PROGRESS_DETERMINATE, 200, "loading");

    for (int i = 0; i < 200; ++i) {
        /*
         * Deliberately vary the per-item delay so the smoothed ETA has
         * something interesting to track: slow start, fast middle, slow end.
         */
        int delay_ms;
        if      (i <  40)              delay_ms = 30;
        else if (i < 100)              delay_ms = 10;
        else if (i < 160)              delay_ms = 20;
        else                           delay_ms = 35;

        sleep_ms(delay_ms);
        progress_update(&p, 1);
    }
    progress_finish(&p);

    sleep_ms(600);

    /* ------------------------------------------------------------------ */
    /* Indeterminate: 200 items, jittered timing to test rate smoothing.  */
    /* ------------------------------------------------------------------ */
    printf("\nIndeterminate mode (200 items, total unknown):\n");
    progress_init(&p, PROGRESS_INDETERMINATE, 0, "scanning");

    for (int i = 0; i < 200; ++i) {
        /* Jitter in [10, 40] ms */
        int delay_ms = 10 + (i % 7) * 5;
        sleep_ms(delay_ms);
        progress_update(&p, 1);
    }
    progress_finish(&p);

    return 0;
}
