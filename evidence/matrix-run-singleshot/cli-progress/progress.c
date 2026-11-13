/*
 * progress.c — reusable ANSI progress bar
 *
 * Features:
 *   - Determinate mode  (known total, shows % + ETA)
 *   - Indeterminate mode (unknown total, bouncing fill)
 *   - Throughput and ETA estimation via exponential smoothing
 *   - Terminal width detection via ioctl(TIOCGWINSZ)
 *   - Carriage-return in-place redraw (no newlines mid-progress)
 *   - Demo loop over 200 items
 *
 * Build:  cc -std=c11 -Wall -Wextra -o progress progress.c
 * Run:    ./progress
 *
 * C11 + POSIX (requires _POSIX_C_SOURCE 200809L or later).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Configuration knobs
 * ---------------------------------------------------------------------- */

/* Exponential-smoothing factor for throughput EMA (0 < α ≤ 1).
 * Smaller → smoother but slower to react. */
#define PB_ALPHA        0.15

/* Fallback terminal width if ioctl fails. */
#define PB_DEFAULT_COLS 80

/* Minimum bar fill width (characters) before we just skip the bar. */
#define PB_MIN_BAR      4

/* Indeterminate fill width as a fraction of the bar area. */
#define PB_INDET_FRAC   0.25

/* Characters used for the bar. */
#define PB_CHAR_FILL    '#'
#define PB_CHAR_EMPTY   '-'
#define PB_CHAR_HEAD    '>'

/* ANSI helpers. */
#define ANSI_RESET      "\033[0m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BOLD       "\033[1m"

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef enum {
    PB_DETERMINATE,
    PB_INDETERMINATE
} pb_mode_t;

typedef struct {
    /* Public configuration — set before pb_start(). */
    pb_mode_t   mode;
    const char *label;      /* Optional prefix label, may be NULL. */
    bool        use_color;  /* Enable ANSI colour codes. */

    /* Progress state. */
    long        total;      /* Total item count (DETERMINATE only). */
    long        current;    /* Items completed so far. */

    /* Timing. */
    struct timespec t_start;    /* Wall time at pb_start(). */
    struct timespec t_last;     /* Wall time of last pb_update() call. */

    /* Throughput EMA (items per second). */
    double      ema_rate;       /* Current smoothed rate. */
    bool        ema_seeded;     /* False until we have a first sample. */

    /* Indeterminate animation state. */
    int         indet_pos;      /* Leading edge position (0-based). */
    int         indet_dir;      /* +1 right, -1 left. */
} progress_bar_t;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Returns elapsed seconds between two timespecs. */
static double ts_diff(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec  - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) * 1e-9;
}

/* Query the current terminal width via ioctl; falls back to PB_DEFAULT_COLS. */
static int terminal_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return PB_DEFAULT_COLS;
}

/* Format a duration in seconds into "HH:MM:SS" or "??:??:??" if unknown.
 * buf must be at least 9 bytes. */
static void fmt_eta(char *buf, size_t bufsz, double secs)
{
    if (secs < 0 || !isfinite(secs)) {
        snprintf(buf, bufsz, "??:??:??");
        return;
    }
    long s = (long)secs;
    snprintf(buf, bufsz, "%02ld:%02ld:%02ld",
             s / 3600, (s % 3600) / 60, s % 60);
}

/* Format throughput into e.g. "  1.23 it/s" or "12.3k it/s".
 * buf must be at least 12 bytes. */
static void fmt_rate(char *buf, size_t bufsz, double rate)
{
    if (!isfinite(rate) || rate < 0) {
        snprintf(buf, bufsz, "  ?.?? it/s");
        return;
    }
    if (rate >= 1000.0)
        snprintf(buf, bufsz, "%5.1fk it/s", rate / 1000.0);
    else
        snprintf(buf, bufsz, "%6.2f it/s", rate);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * pb_init — zero-initialise a progress_bar_t with sensible defaults.
 *
 * Call this before setting any fields so the struct is in a known state.
 */
void pb_init(progress_bar_t *pb)
{
    memset(pb, 0, sizeof *pb);
    pb->mode       = PB_DETERMINATE;
    pb->label      = NULL;
    pb->use_color  = true;
    pb->total      = 0;
    pb->current    = 0;
    pb->ema_rate   = 0.0;
    pb->ema_seeded = false;
    pb->indet_pos  = 0;
    pb->indet_dir  = 1;
}

/*
 * pb_start — record the start time and print the initial (empty) bar.
 */
void pb_start(progress_bar_t *pb)
{
    clock_gettime(CLOCK_MONOTONIC, &pb->t_start);
    pb->t_last     = pb->t_start;
    pb->ema_seeded = false;
    pb->current    = 0;
    pb->indet_pos  = 0;
    pb->indet_dir  = 1;
}

/*
 * pb_render — draw the bar in-place using '\r' (no newline).
 *
 * This may be called directly, but pb_update() calls it automatically.
 */
void pb_render(progress_bar_t *pb)
{
    int cols = terminal_width();

    /* ---- Build the right-hand side status string first so we know its width. */
    char status[64] = {0};

    if (pb->mode == PB_DETERMINATE) {
        double pct = (pb->total > 0)
                   ? 100.0 * (double)pb->current / (double)pb->total
                   : 0.0;

        char rate_str[16];
        fmt_rate(rate_str, sizeof rate_str, pb->ema_rate);

        double remaining = -1.0;
        if (pb->ema_rate > 0.0 && pb->total > pb->current)
            remaining = (double)(pb->total - pb->current) / pb->ema_rate;

        char eta_str[12];
        fmt_eta(eta_str, sizeof eta_str, remaining);

        snprintf(status, sizeof status,
                 " %3.0f%% %ld/%ld  %s  ETA %s",
                 pct, pb->current, pb->total, rate_str, eta_str);
    } else {
        char rate_str[16];
        fmt_rate(rate_str, sizeof rate_str, pb->ema_rate);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = ts_diff(&pb->t_start, &now);

        char ela_str[12];
        fmt_eta(ela_str, sizeof ela_str, elapsed);

        snprintf(status, sizeof status,
                 " %ld items  %s  %s",
                 pb->current, rate_str, ela_str);
    }

    /* ---- Build the left-hand label (may be empty). */
    char label_buf[64] = {0};
    if (pb->label && pb->label[0]) {
        snprintf(label_buf, sizeof label_buf, "%-12s ", pb->label);
    }

    int label_len  = (int)strlen(label_buf);
    int status_len = (int)strlen(status);

    /* ---- The bar occupies what's left; at minimum PB_MIN_BAR chars. */
    /* Format: [label][BAR][ status] */
    /* Bar section: "[" + fill + "]" = 2 + fill_width chars */
    int bar_outer = cols - label_len - status_len;
    int fill_w    = bar_outer - 2; /* subtract the two bracket chars */

    bool show_bar = (fill_w >= PB_MIN_BAR);

    /* ---- Compose the output into a heap buffer (max cols+some). */
    /* We'll build it piece by piece into a fixed buffer sized generously. */
    char line[1024];
    int  pos = 0;

    /* Carriage return to overwrite the current line. */
    line[pos++] = '\r';

    /* Label. */
    if (pb->use_color) {
        int n = snprintf(line + pos, sizeof line - (size_t)pos,
                         ANSI_BOLD "%s" ANSI_RESET, label_buf);
        if (n > 0) pos += n;
    } else {
        int n = snprintf(line + pos, sizeof line - (size_t)pos,
                         "%s", label_buf);
        if (n > 0) pos += n;
    }

    /* Bar. */
    if (show_bar) {
        line[pos++] = '[';

        if (pb->mode == PB_DETERMINATE) {
            int filled = (pb->total > 0)
                       ? (int)((double)fill_w * pb->current / pb->total)
                       : 0;
            if (filled > fill_w) filled = fill_w;

            if (pb->use_color) {
                int n = snprintf(line + pos, sizeof line - (size_t)pos,
                                 ANSI_GREEN);
                if (n > 0) pos += n;
            }
            for (int i = 0; i < fill_w; i++) {
                if (i < filled - 1)
                    line[pos++] = PB_CHAR_FILL;
                else if (i == filled - 1 && pb->current < pb->total)
                    line[pos++] = PB_CHAR_HEAD;
                else if (i < filled)
                    line[pos++] = PB_CHAR_FILL;
                else
                    line[pos++] = PB_CHAR_EMPTY;
            }
            if (pb->use_color) {
                int n = snprintf(line + pos, sizeof line - (size_t)pos,
                                 ANSI_RESET);
                if (n > 0) pos += n;
            }

        } else {
            /* Indeterminate: a bouncing block. */
            int head_w = (int)((double)fill_w * PB_INDET_FRAC);
            if (head_w < 1) head_w = 1;

            if (pb->use_color) {
                int n = snprintf(line + pos, sizeof line - (size_t)pos,
                                 ANSI_CYAN);
                if (n > 0) pos += n;
            }
            for (int i = 0; i < fill_w; i++) {
                int dist = i - pb->indet_pos;
                if (dist >= 0 && dist < head_w)
                    line[pos++] = PB_CHAR_FILL;
                else
                    line[pos++] = PB_CHAR_EMPTY;
            }
            if (pb->use_color) {
                int n = snprintf(line + pos, sizeof line - (size_t)pos,
                                 ANSI_RESET);
                if (n > 0) pos += n;
            }

            /* Advance bouncing position. */
            int head_w2 = (int)((double)fill_w * PB_INDET_FRAC);
            if (head_w2 < 1) head_w2 = 1;
            pb->indet_pos += pb->indet_dir;
            if (pb->indet_pos + head_w2 >= fill_w) {
                pb->indet_pos = fill_w - head_w2;
                pb->indet_dir = -1;
            } else if (pb->indet_pos <= 0) {
                pb->indet_pos = 0;
                pb->indet_dir = 1;
            }
        }

        line[pos++] = ']';
    }

    /* Status string. */
    if (pb->use_color) {
        int n = snprintf(line + pos, sizeof line - (size_t)pos,
                         ANSI_YELLOW "%s" ANSI_RESET, status);
        if (n > 0) pos += n;
    } else {
        int n = snprintf(line + pos, sizeof line - (size_t)pos,
                         "%s", status);
        if (n > 0) pos += n;
    }

    /* Pad to clear any leftover characters from a wider previous render. */
    int rendered_chars = label_len + (show_bar ? fill_w + 2 : 0) + status_len;
    int padding        = cols - rendered_chars;
    for (int i = 0; i < padding && pos < (int)(sizeof line) - 1; i++)
        line[pos++] = ' ';

    line[pos] = '\0';

    fputs(line, stdout);
    fflush(stdout);
}

/*
 * pb_update — advance the progress counter by `delta` items and redraw.
 *
 * Also updates the exponential moving average for throughput estimation.
 */
void pb_update(progress_bar_t *pb, long delta)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double dt = ts_diff(&pb->t_last, &now);

    if (dt > 1e-9) {
        double inst_rate = (double)delta / dt;
        if (!pb->ema_seeded) {
            pb->ema_rate   = inst_rate;
            pb->ema_seeded = true;
        } else {
            pb->ema_rate = PB_ALPHA * inst_rate
                         + (1.0 - PB_ALPHA) * pb->ema_rate;
        }
    }

    pb->current += delta;
    pb->t_last   = now;

    pb_render(pb);
}

/*
 * pb_finish — mark the bar complete and move to the next line.
 *
 * For DETERMINATE mode, snaps current to total so the bar shows 100%.
 */
void pb_finish(progress_bar_t *pb)
{
    if (pb->mode == PB_DETERMINATE && pb->current < pb->total)
        pb->current = pb->total;

    pb_render(pb);
    putchar('\n');
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Demo
 * ---------------------------------------------------------------------- */

/*
 * Portable nanosleep wrapper (POSIX).
 */
static void sleep_ms(long ms)
{
    struct timespec req = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&req, NULL);
}

int main(void)
{
    const int ITEMS = 200;

    /* ---- Demo 1: determinate bar ---- */
    printf("Demo 1 — determinate mode (%d items)\n", ITEMS);

    progress_bar_t pb;
    pb_init(&pb);
    pb.mode      = PB_DETERMINATE;
    pb.label     = "Processing";
    pb.use_color = true;
    pb.total     = ITEMS;

    pb_start(&pb);

    for (int i = 0; i < ITEMS; i++) {
        /* Simulate variable-speed work: faster in the middle. */
        long delay_ms;
        if (i < 50)
            delay_ms = 30;
        else if (i < 150)
            delay_ms = 10;
        else
            delay_ms = 25;

        sleep_ms(delay_ms);
        pb_update(&pb, 1);
    }

    pb_finish(&pb);
    printf("Done.\n\n");

    /* ---- Demo 2: indeterminate bar ---- */
    printf("Demo 2 — indeterminate mode (simulated scan, %d events)\n", ITEMS);

    pb_init(&pb);
    pb.mode      = PB_INDETERMINATE;
    pb.label     = "Scanning";
    pb.use_color = true;

    pb_start(&pb);

    for (int i = 0; i < ITEMS; i++) {
        sleep_ms(20);
        pb_update(&pb, 1);
    }

    pb_finish(&pb);
    printf("Done.\n\n");

    /* ---- Demo 3: determinate, no colour ---- */
    printf("Demo 3 — determinate, colour disabled\n");

    pb_init(&pb);
    pb.mode      = PB_DETERMINATE;
    pb.label     = "Copying";
    pb.use_color = false;
    pb.total     = ITEMS;

    pb_start(&pb);

    for (int i = 0; i < ITEMS; i++) {
        sleep_ms(15);
        pb_update(&pb, 1);
    }

    pb_finish(&pb);
    printf("Done.\n");

    return EXIT_SUCCESS;
}
