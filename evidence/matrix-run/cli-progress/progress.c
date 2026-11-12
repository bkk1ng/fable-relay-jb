/*
 * progress.c — reusable ANSI progress bar, C11 / POSIX
 *
 * Features
 *   • Determinate and indeterminate modes
 *   • Throughput and ETA estimation with exponential smoothing
 *   • Terminal width detection via ioctl(TIOCGWINSZ)
 *   • Carriage-return redraw (in-place, no scroll)
 *   • Self-contained demo (compile with -DPROGRESS_DEMO)
 *
 * Build (library):
 *   cc -std=c11 -D_POSIX_C_SOURCE=200809L -c progress.c
 *
 * Build + run demo:
 *   cc -std=c11 -D_POSIX_C_SOURCE=200809L -DPROGRESS_DEMO \
 *      -o progress_demo progress.c && ./progress_demo
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ── tuning ─────────────────────────────────────────────────────────────── */

/* exponential-smoothing factor for throughput (0 < α ≤ 1).
 * Lower values → smoother but slower to react. */
#define PB_ALPHA          0.12

/* assumed terminal width when ioctl fails */
#define PB_FALLBACK_WIDTH 80

/* hard cap so stack buffers stay bounded */
#define PB_MAX_COLS       512

/* ── bar / spinner glyphs ────────────────────────────────────────────────── */

/* Each of these is a UTF-8 sequence that occupies exactly 1 display column. */
#define GLYPH_FILL  "\xe2\x96\x88"  /* U+2588 FULL BLOCK           █ */
#define GLYPH_EMPTY "\xe2\x96\x91"  /* U+2591 LIGHT SHADE          ░ */
#define GLYPH_HEAD  "\xe2\x96\x8c"  /* U+258C LEFT HALF BLOCK      ▌ */

static const char *SPINNER_FRAMES[] = {
    "\xe2\xa0\x8b", /* ⠋ */ "\xe2\xa0\x99", /* ⠙ */
    "\xe2\xa0\xb9", /* ⠹ */ "\xe2\xa0\xb8", /* ⠸ */
    "\xe2\xa0\xbc", /* ⠼ */ "\xe2\xa0\xb4", /* ⠴ */
    "\xe2\xa0\xa6", /* ⠦ */ "\xe2\xa0\xa7", /* ⠧ */
    "\xe2\xa0\x87", /* ⠇ */ "\xe2\xa0\x8f", /* ⠏ */
};
#define SPINNER_COUNT ((int)(sizeof(SPINNER_FRAMES) / sizeof(*SPINNER_FRAMES)))

/* ── types ───────────────────────────────────────────────────────────────── */

typedef enum {
    PB_DETERMINATE,
    PB_INDETERMINATE,
} PbMode;

typedef struct {
    PbMode      mode;

    /* determinate state */
    long        total;      /* expected total item count */
    long        current;    /* items completed so far    */

    /* timing */
    struct timespec t_start;
    struct timespec t_prev;

    /* exponential-smoothed throughput (items / second) */
    double      smoothed_rate;
    bool        rate_seeded;

    /* indeterminate animation */
    int         spinner_idx;
    int         bounce_pos;
    int         bounce_dir;  /* +1 or -1 */

    /* optional prefix label (truncated to 32 visible chars) */
    char        label[128];  /* UTF-8; byte budget > display budget */

    bool        active;
} ProgressBar;

/* ── internal helpers ────────────────────────────────────────────────────── */

/*
 * Count display columns for a UTF-8 string.
 * Assumes every code point occupies exactly 1 column (true for the
 * Latin, block-element, and Braille ranges we use here; CJK wide
 * characters are not expected in labels or glyphs).
 */
static int pb_display_width(const char *s)
{
    int w = 0;
    while (*s) {
        unsigned char b = (unsigned char)*s;
        if      ((b & 0x80) == 0x00) { w++; s += 1; }  /* 1-byte (ASCII) */
        else if ((b & 0xE0) == 0xC0) { w++; s += 2; }  /* 2-byte          */
        else if ((b & 0xF0) == 0xE0) { w++; s += 3; }  /* 3-byte          */
        else if ((b & 0xF8) == 0xF0) { w++; s += 4; }  /* 4-byte          */
        else                         {      s += 1; }  /* invalid, skip   */
    }
    return w;
}

static int pb_terminal_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col < PB_MAX_COLS ? (int)ws.ws_col : PB_MAX_COLS;
    return PB_FALLBACK_WIDTH;
}

static double pb_diff_sec(const struct timespec *a, const struct timespec *b)
{
    return (double)(a->tv_sec  - b->tv_sec)
         + (double)(a->tv_nsec - b->tv_nsec) * 1e-9;
}

/* Write a human-readable throughput string into buf. */
static void pb_fmt_rate(char *buf, size_t sz, double rate)
{
    if      (rate >= 1e9) snprintf(buf, sz, "%.2fG/s", rate / 1e9);
    else if (rate >= 1e6) snprintf(buf, sz, "%.2fM/s", rate / 1e6);
    else if (rate >= 1e3) snprintf(buf, sz, "%.1fk/s", rate / 1e3);
    else                  snprintf(buf, sz, "%.0f/s",  rate);
}

/* Write a human-readable ETA string (mm:ss or h:mm:ss) into buf. */
static void pb_fmt_eta(char *buf, size_t sz, double secs)
{
    if (secs < 0 || secs > 359999.0) {
        snprintf(buf, sz, "--:--");
        return;
    }
    int h = (int)(secs / 3600);
    int m = (int)(secs / 60) % 60;
    int s = (int)secs % 60;
    if (h > 0) snprintf(buf, sz, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, sz, "%02d:%02d",        m, s);
}

/*
 * Append `count` copies of `glyph` into `dst` (which has `cap` bytes total).
 * Returns updated write position (byte offset past what was written).
 */
static size_t pb_fill_glyphs(char *dst, size_t cap, size_t pos,
                              const char *glyph, int count)
{
    size_t glen = strlen(glyph);
    for (int i = 0; i < count; i++) {
        if (pos + glen >= cap) break;
        memcpy(dst + pos, glyph, glen);
        pos += glen;
    }
    return pos;
}

/* ── public API ──────────────────────────────────────────────────────────── */

/*
 * pb_init_determinate — initialise for a known-length task.
 *
 *   total : expected number of items (must be > 0)
 *   label : optional prefix string (may be NULL)
 */
void pb_init_determinate(ProgressBar *pb, long total, const char *label)
{
    memset(pb, 0, sizeof(*pb));
    pb->mode       = PB_DETERMINATE;
    pb->total      = total > 0 ? total : 1;
    pb->bounce_dir = 1;
    if (label)
        snprintf(pb->label, sizeof(pb->label), "%s", label);
}

/*
 * pb_init_indeterminate — initialise for an open-ended task.
 *
 *   label : optional prefix string (may be NULL)
 */
void pb_init_indeterminate(ProgressBar *pb, const char *label)
{
    memset(pb, 0, sizeof(*pb));
    pb->mode       = PB_INDETERMINATE;
    pb->bounce_dir = 1;
    if (label)
        snprintf(pb->label, sizeof(pb->label), "%s", label);
}

/* pb_start — record the start time; call once before the first pb_update. */
void pb_start(ProgressBar *pb)
{
    clock_gettime(CLOCK_MONOTONIC, &pb->t_start);
    pb->t_prev = pb->t_start;
    pb->active = true;
}

/*
 * pb_update — redraw the bar in place.
 *
 *   done : for PB_DETERMINATE, the cumulative items completed so far.
 *          for PB_INDETERMINATE, the value is ignored (pass 0).
 *
 * Layout (determinate):
 *   [label] NNN% [████████████░░░░░░░░] 1.2k/s ETA 00:14
 *
 * Layout (indeterminate):
 *   [label] ⠹ [░░░░░░░░░█░░░░░░░░░░░] 847/s
 */
void pb_update(ProgressBar *pb, long done)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double dt = pb_diff_sec(&now, &pb->t_prev);

    /* update exponentially-smoothed throughput */
    if (dt > 1e-6) {
        long   delta     = (pb->mode == PB_DETERMINATE) ? (done - pb->current) : 1;
        double inst_rate = (double)delta / dt;

        if (!pb->rate_seeded) {
            pb->smoothed_rate = inst_rate;
            pb->rate_seeded   = true;
        } else {
            pb->smoothed_rate =
                PB_ALPHA * inst_rate + (1.0 - PB_ALPHA) * pb->smoothed_rate;
        }
    }

    if (pb->mode == PB_DETERMINATE)
        pb->current = done;

    pb->t_prev = now;

    int cols = pb_terminal_width();

    /* ── build prefix and suffix strings first; size bar to fit ──────── */

    /*
     * bar_buf holds the rendered bar including enclosing brackets.
     * worst case: PB_MAX_COLS columns × 3 bytes/glyph + 2 brackets + NUL
     */
    char bar_buf[PB_MAX_COLS * 3 + 4];

    char rate_str[20];
    if (pb->rate_seeded)
        pb_fmt_rate(rate_str, sizeof(rate_str), pb->smoothed_rate);
    else
        snprintf(rate_str, sizeof(rate_str), "  ?/s");

    if (pb->mode == PB_DETERMINATE) {

        double pct  = (double)pb->current / (double)pb->total;
        if (pct > 1.0) pct = 1.0;

        char eta_str[16];
        double remaining = (pb->smoothed_rate > 0.1)
                         ? (double)(pb->total - pb->current) / pb->smoothed_rate
                         : -1.0;
        pb_fmt_eta(eta_str, sizeof(eta_str), remaining);

        /* prefix: "Label  42% " (note trailing space) */
        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s %3.0f%% ",
                 pb->label, pct * 100.0);

        /* suffix: " 1.2k/s ETA 01:23" */
        char suffix[64];
        snprintf(suffix, sizeof(suffix), " %s ETA %s", rate_str, eta_str);

        int prefix_dw = pb_display_width(prefix);
        int suffix_dw = pb_display_width(suffix);

        /* bar interior width in columns (2 for the [ ] brackets) */
        int bar_cols = cols - prefix_dw - suffix_dw - 2;
        if (bar_cols < 2) bar_cols = 2;

        int filled = (int)(pct * (double)bar_cols);
        if (filled > bar_cols) filled = bar_cols;
        int empty  = bar_cols - filled;

        /* render: prefix + [ + fills + empties + ] + suffix */
        size_t p = 0;
        bar_buf[p++] = '[';
        p = pb_fill_glyphs(bar_buf, sizeof(bar_buf), p, GLYPH_FILL,  filled);
        p = pb_fill_glyphs(bar_buf, sizeof(bar_buf), p, GLYPH_EMPTY, empty);
        bar_buf[p++] = ']';
        bar_buf[p]   = '\0';

        fprintf(stdout, "\r%s%s%s", prefix, bar_buf, suffix);

    } else {

        /* spinner + bounce block */
        const char *spin = SPINNER_FRAMES[pb->spinner_idx];
        pb->spinner_idx  = (pb->spinner_idx + 1) % SPINNER_COUNT;

        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s%s ", pb->label, spin);

        char suffix[32];
        snprintf(suffix, sizeof(suffix), " %s", rate_str);

        int prefix_dw = pb_display_width(prefix);
        int suffix_dw = pb_display_width(suffix);

        int bar_cols = cols - prefix_dw - suffix_dw - 2;
        if (bar_cols < 2) bar_cols = 2;

        /* keep bounce position in range */
        if (pb->bounce_pos < 0)              { pb->bounce_pos = 0;            pb->bounce_dir =  1; }
        if (pb->bounce_pos >= bar_cols)      { pb->bounce_pos = bar_cols - 1; pb->bounce_dir = -1; }

        size_t p = 0;
        bar_buf[p++] = '[';
        for (int i = 0; i < bar_cols; i++) {
            const char *g = (i == pb->bounce_pos) ? GLYPH_HEAD : GLYPH_EMPTY;
            size_t glen   = strlen(g);
            if (p + glen >= sizeof(bar_buf) - 2) break;
            memcpy(bar_buf + p, g, glen);
            p += glen;
        }
        bar_buf[p++] = ']';
        bar_buf[p]   = '\0';

        pb->bounce_pos += pb->bounce_dir;

        fprintf(stdout, "\r%s%s%s", prefix, bar_buf, suffix);
    }

    fflush(stdout);
}

/*
 * pb_finish — clear the bar line and print a completion summary.
 *
 *   msg : custom message (may be NULL for a default "✓ label done" line)
 */
void pb_finish(ProgressBar *pb, const char *msg)
{
    if (!pb->active) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = pb_diff_sec(&now, &pb->t_start);

    char rate_str[20];
    if (pb->rate_seeded)
        pb_fmt_rate(rate_str, sizeof(rate_str), pb->smoothed_rate);
    else
        snprintf(rate_str, sizeof(rate_str), "?/s");

    int cols = pb_terminal_width();

    /* overwrite the bar line with blanks, then print summary */
    fprintf(stdout, "\r%*s\r", cols - 1, "");

    if (msg) {
        fprintf(stdout, "✓ %s  (%.1fs, %s)\n", msg, elapsed, rate_str);
    } else {
        const char *name = pb->label[0] ? pb->label : "done";
        fprintf(stdout, "✓ %s  %.1fs  %s\n", name, elapsed, rate_str);
    }

    fflush(stdout);
    pb->active = false;
}

/* ── demo ────────────────────────────────────────────────────────────────── */

#ifdef PROGRESS_DEMO

#include <stdlib.h>   /* rand() */

static void demo_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000L, (ms % 1000L) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void)
{
    srand(42);
    const int N = 200;

    /* ── determinate: 200 items with jittered delay ───────────────────── */
    printf("Determinate mode — %d items\n", N);

    ProgressBar pb;
    pb_init_determinate(&pb, N, "loading");
    pb_start(&pb);

    for (int i = 0; i <= N; i++) {
        pb_update(&pb, i);
        demo_sleep_ms(10 + rand() % 25);   /* jitter exercises smoothing */
    }

    pb_finish(&pb, NULL);

    /* ── indeterminate: 3-second scan ────────────────────────────────── */
    printf("\nIndeterminate mode — 3 seconds\n");

    pb_init_indeterminate(&pb, "scanning");
    pb_start(&pb);

    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        pb_update(&pb, 0);
        demo_sleep_ms(60);
        clock_gettime(CLOCK_MONOTONIC, &tn);
    } while (pb_diff_sec(&tn, &t0) < 3.0);

    pb_finish(&pb, "scan complete");

    return 0;
}

#endif /* PROGRESS_DEMO */
