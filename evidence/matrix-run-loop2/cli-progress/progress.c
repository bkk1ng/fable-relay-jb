/*
 * progress.c — reusable ANSI progress bar, C11/POSIX
 *
 * Modes
 *   PB_DETERMINATE   filled bar, %, count, throughput, ETA
 *   PB_INDETERMINATE bouncing fill, spinner, throughput, elapsed
 *
 * Both modes use exponential moving average (α=0.15) for throughput
 * and ETA estimation.  Terminal width is queried via ioctl(TIOCGWINSZ)
 * on every draw so the bar tracks window resizes live.
 *
 * Build:  cc -std=c11 -Wall -Wextra -o progress progress.c -lm
 * Run:    ./progress
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ── constants ─────────────────────────────────────────────────────────── */

#define EMA_ALPHA        0.15   /* smoothing factor; lower = smoother      */
#define BAR_MIN_WIDTH    10     /* never render narrower than this          */
#define SPINNER_FRAMES   4
static const char SPINNER[SPINNER_FRAMES] = { '|', '/', '-', '\\' };

/* ── types ─────────────────────────────────────────────────────────────── */

typedef enum {
    PB_DETERMINATE,
    PB_INDETERMINATE,
} PBMode;

typedef struct {
    PBMode  mode;
    long    total;          /* total items (meaningful in DETERMINATE)     */
    long    current;        /* items completed so far                      */

    /* timing */
    struct timespec t_start;
    struct timespec t_prev;
    long    prev_count;     /* pb->current at the previous update call     */

    /* throughput */
    double  ema_rate;       /* items/s, exponentially smoothed             */
    bool    ema_seeded;

    /* indeterminate animation state */
    int     ind_pos;        /* left edge of the bouncing chunk             */
    int     ind_dir;        /* +1 (right) or -1 (left)                    */
    int     spin_idx;       /* wraps over SPINNER_FRAMES                   */

    /* housekeeping */
    unsigned long draw_count;
    char   *label;          /* optional short prefix, heap-allocated       */
} ProgressBar;

/* ── internal helpers ───────────────────────────────────────────────────── */

static struct timespec mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

/* elapsed seconds between two CLOCK_MONOTONIC timestamps */
static double ts_elapsed(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec  - a.tv_sec)
         + (double)(b.tv_nsec - a.tv_nsec) * 1e-9;
}

static int query_terminal_width(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return (int)w.ws_col;
    return 80;
}

/*
 * Format seconds into MM:SS or HH:MM:SS.
 * buf must hold at least 9 bytes.  Writes "--:--" for unknown.
 */
static void fmt_duration(char *buf, size_t len, double secs)
{
    if (secs < 0.0 || !isfinite(secs)) {
        snprintf(buf, len, "--:--");
        return;
    }
    long s = (long)secs;
    long h = s / 3600;
    long m = (s % 3600) / 60;
    long r = s % 60;
    if (h > 0)
        snprintf(buf, len, "%02ld:%02ld:%02ld", h, m, r);
    else
        snprintf(buf, len, "%02ld:%02ld", m, r);
}

/*
 * Format items/second as "NNN.N/s" or "NN.Nk/s".
 * buf must hold at least 12 bytes.
 */
static void fmt_rate(char *buf, size_t len, double rate)
{
    if (!isfinite(rate) || rate <= 0.0) {
        snprintf(buf, len, "?/s");
        return;
    }
    if (rate >= 1000.0)
        snprintf(buf, len, "%.1fk/s", rate / 1000.0);
    else if (rate >= 10.0)
        snprintf(buf, len, "%.0f/s", rate);
    else
        snprintf(buf, len, "%.2f/s", rate);
}

/* Feed a new instantaneous observation into the EMA. */
static void ema_feed(ProgressBar *pb, double delta_items, double delta_secs)
{
    if (delta_secs <= 0.0 || delta_items <= 0.0)
        return;
    double inst = delta_items / delta_secs;
    if (!pb->ema_seeded) {
        pb->ema_rate   = inst;
        pb->ema_seeded = true;
    } else {
        pb->ema_rate = EMA_ALPHA * inst + (1.0 - EMA_ALPHA) * pb->ema_rate;
    }
}

/* ── public API ─────────────────────────────────────────────────────────── */

/*
 * pb_init — initialise a ProgressBar.
 *
 *   pb:    caller-allocated struct
 *   mode:  PB_DETERMINATE | PB_INDETERMINATE
 *   total: total item count (ignored in INDETERMINATE)
 *   label: optional short prefix string (copied), or NULL
 */
void pb_init(ProgressBar *pb, PBMode mode, long total, const char *label)
{
    memset(pb, 0, sizeof *pb);
    pb->mode    = mode;
    pb->total   = (total > 0) ? total : 1;
    pb->ind_dir = 1;
    pb->t_start = mono_now();
    pb->t_prev  = pb->t_start;
    pb->label   = label ? strdup(label) : NULL;
}

void pb_free(ProgressBar *pb)
{
    free(pb->label);   /* free(NULL) is safe */
    pb->label = NULL;
}

/*
 * pb_draw — render one frame to stdout using '\r' for in-place redraw.
 *
 * Determinate layout (tw=80, no label):
 *   [################---------]  120/200  60.0%  12.3/s  ETA 00:06
 *
 * Indeterminate layout (tw=80, label="scan"):
 *   scan       / [   >>>>      ]  12.3/s  00:14
 *
 * This function also advances the indeterminate animation so that
 * callers that render without calling pb_update (e.g. pb_finish) still
 * get a correct final frame.
 */
void pb_draw(ProgressBar *pb)
{
    struct timespec tnow    = mono_now();
    double elapsed          = ts_elapsed(pb->t_start, tnow);
    int    tw               = query_terminal_width();

    /* ── build info suffix (right side) ──────────────────────────────── */
    char rate_str[16], time_str[16], info[72];

    fmt_rate(rate_str, sizeof rate_str, pb->ema_rate);

    if (pb->mode == PB_DETERMINATE) {
        double pct = 100.0 * (double)pb->current / (double)pb->total;
        double eta = (pb->ema_seeded && pb->ema_rate > 0.0)
                   ? (double)(pb->total - pb->current) / pb->ema_rate
                   : -1.0;
        fmt_duration(time_str, sizeof time_str, eta);
        snprintf(info, sizeof info,
                 "  %ld/%ld  %5.1f%%  %s  ETA %s",
                 pb->current, pb->total, pct, rate_str, time_str);
    } else {
        fmt_duration(time_str, sizeof time_str, elapsed);
        snprintf(info, sizeof info, "  %s  %s", rate_str, time_str);
    }

    /* ── build label prefix (left side) ─────────────────────────────── */
    char prefix[32] = "";
    int  prefix_len = 0;
    if (pb->label && pb->label[0]) {
        snprintf(prefix, sizeof prefix, "%-10s ", pb->label);
        prefix_len = (int)strlen(prefix);
    }

    /* ── for INDETERMINATE: spinner eats 2 chars before the [ ───────── */
    int spinner_w = (pb->mode == PB_INDETERMINATE) ? 2 : 0;

    /* ── compute bar interior width ──────────────────────────────────── */
    int info_len = (int)strlen(info);
    int bar_w    = tw - prefix_len - spinner_w - 2 /* [ ] */ - info_len;
    if (bar_w < BAR_MIN_WIDTH)
        bar_w = BAR_MIN_WIDTH;

    /* ── advance indeterminate animation ─────────────────────────────── */
    if (pb->mode == PB_INDETERMINATE) {
        int chunk = bar_w / 4;
        if (chunk < 3) chunk = 3;

        /* bounce bounds: ind_pos in [0, bar_w - chunk] */
        pb->ind_pos += pb->ind_dir;
        if (pb->ind_pos >= bar_w - chunk) {
            pb->ind_pos = bar_w - chunk;
            pb->ind_dir = -1;
        } else if (pb->ind_pos <= 0) {
            pb->ind_pos = 0;
            pb->ind_dir = 1;
        }
        pb->spin_idx = (pb->spin_idx + 1) % SPINNER_FRAMES;
    }

    /* ── allocate and fill bar buffer ────────────────────────────────── */
    char *bar = malloc((size_t)(bar_w + 1));
    if (!bar) return;   /* graceful skip on OOM */

    if (pb->mode == PB_DETERMINATE) {
        long filled = (long)((double)pb->current / (double)pb->total
                             * (double)bar_w);
        for (int i = 0; i < bar_w; i++)
            bar[i] = (i < filled) ? '#' : '-';

    } else {
        int chunk = bar_w / 4;
        if (chunk < 3) chunk = 3;
        memset(bar, ' ', (size_t)bar_w);
        for (int i = pb->ind_pos;
             i < pb->ind_pos + chunk && i < bar_w;
             i++)
            bar[i] = '>';
    }
    bar[bar_w] = '\0';

    /* ── emit ─────────────────────────────────────────────────────────── */
    if (pb->mode == PB_DETERMINATE) {
        fprintf(stdout, "\r%s[%s]%s", prefix, bar, info);
    } else {
        char spin = SPINNER[pb->spin_idx];
        fprintf(stdout, "\r%s%c [%s]%s", prefix, spin, bar, info);
    }
    fflush(stdout);

    free(bar);
    pb->draw_count++;
}

/*
 * pb_update — advance the bar by delta items and redraw.
 * Call once per item (or batch) from the work loop.
 */
void pb_update(ProgressBar *pb, long delta)
{
    struct timespec tnow = mono_now();
    double dt = ts_elapsed(pb->t_prev, tnow);

    pb->current += delta;
    if (pb->mode == PB_DETERMINATE && pb->current > pb->total)
        pb->current = pb->total;

    ema_feed(pb, (double)delta, dt);

    pb->prev_count = pb->current;
    pb->t_prev     = tnow;

    pb_draw(pb);
}

/*
 * pb_finish — emit the final frame, move to the next line, optionally
 * print a brief summary.
 */
void pb_finish(ProgressBar *pb, bool print_summary)
{
    pb_draw(pb);
    putchar('\n');

    if (print_summary) {
        double elapsed = ts_elapsed(pb->t_start, mono_now());
        char   t[16], r[16];
        fmt_duration(t, sizeof t, elapsed);
        fmt_rate(r, sizeof r, pb->ema_rate);
        printf("  %ld items  %s  avg %s\n", pb->current, t, r);
    }
    fflush(stdout);
}

/* ── demo ───────────────────────────────────────────────────────────────── */

static void sleep_ms(long ms)
{
    struct timespec req = {
        .tv_sec  = ms / 1000L,
        .tv_nsec = (ms % 1000L) * 1000000L,
    };
    nanosleep(&req, NULL);
}

int main(void)
{
    /* ── determinate: 200 items, variable work per item ─────────────── */
    printf("Determinate mode — 200 items\n");

    ProgressBar det;
    pb_init(&det, PB_DETERMINATE, 200, "process");

    for (int i = 0; i < 200; i++) {
        /* simulate variable latency: 8–38 ms, loosely periodic */
        long delay = 8L + (long)(30.0 * ((double)(i % 11) / 10.0));
        sleep_ms(delay);
        pb_update(&det, 1);
    }

    pb_finish(&det, true);
    pb_free(&det);

    sleep_ms(500);

    /* ── indeterminate: 200 items, duration unknown to the bar ──────── */
    printf("\nIndeterminate mode — 200 items\n");

    ProgressBar ind;
    pb_init(&ind, PB_INDETERMINATE, 200, "scan");

    for (int i = 0; i < 200; i++) {
        long delay = 12L + (long)(25.0 * ((double)(i % 7) / 6.0));
        sleep_ms(delay);
        pb_update(&ind, 1);
    }

    pb_finish(&ind, true);
    pb_free(&ind);

    return 0;
}
