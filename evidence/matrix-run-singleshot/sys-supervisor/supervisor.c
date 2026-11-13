/*
 * supervisor.c — process supervisor
 * C11, Linux
 *
 * Usage:  supervisor <config-file>
 *
 * Config file format (one child per non-comment line):
 *   # comment
 *   /usr/bin/myservice --flag value
 *   /usr/bin/worker --id 1
 *
 * Behaviour:
 *   - Forks N children defined in the config.
 *   - Reaps children via SIGCHLD delivered through a self-pipe; main loop
 *     uses select(2) so no busy-wait and no race between waitpid and sleep.
 *   - Restarts children that exit with a non-zero status or are killed by a
 *     signal, using exponential backoff (1 s → 2 s → … → BACKOFF_MAX_S).
 *   - Applies a crash-loop cutoff: if a child crashes CRASH_LIMIT times
 *     within CRASH_WINDOW_S seconds, it is marked stopped and not restarted.
 *   - Resets backoff to 1 s when a child runs for at least HEALTHY_RUN_S
 *     seconds before dying.
 *   - Forwards SIGTERM (and SIGINT) to all children, then waits for them.
 *   - Logs child exit status / signal and restart scheduling to stderr.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -o supervisor supervisor.c
 *
 * Run:
 *   ./supervisor /etc/mysupervisor.conf
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */
#define MAX_CHILDREN     64
#define MAX_ARGS         64
#define MAX_LINE         4096
#define BACKOFF_BASE_S   1
#define BACKOFF_MAX_S    64
#define CRASH_LIMIT      5          /* crashes within CRASH_WINDOW_S → stop  */
#define CRASH_WINDOW_S   60         /* rolling window for crash-loop detection */
#define HEALTHY_RUN_S    30         /* run this long → reset backoff to base   */

/* -------------------------------------------------------------------------
 * Child descriptor
 * ---------------------------------------------------------------------- */
typedef struct {
    char   *argv[MAX_ARGS + 1];
    int     argc;

    pid_t   pid;
    int     running;            /* 1 while child process is alive            */
    int     stopped;            /* 1 when crash-loop cutoff has fired        */

    time_t  started_at;         /* wall time of most recent fork()           */
    time_t  restart_after;      /* earliest wall time for next restart;
                                   0 = "do not restart"                      */

    /* Exponential backoff state */
    int     backoff_s;          /* current delay before next restart         */

    /* Crash-loop detection: circular buffer of last CRASH_LIMIT crash times */
    time_t  crash_times[CRASH_LIMIT];
    int     crash_head;         /* index of next write position              */
    int     crash_count;        /* total crashes (saturates at CRASH_LIMIT)  */
} child_t;

/* -------------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */
static child_t  g_children[MAX_CHILDREN];
static int      g_nchildren;

/* Self-pipe: SIGCHLD / SIGTERM write a byte here; select() wakes the loop. */
static int g_pipe_rd = -1;
static int g_pipe_wr = -1;

static volatile sig_atomic_t g_got_sigterm;

/* -------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------- */
static void
log_msg(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", &tm);

    fprintf(stderr, "[%s.%03ld] supervisor: ", tbuf, ts.tv_nsec / 1000000L);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

/* -------------------------------------------------------------------------
 * Signal handlers (async-signal-safe: only write(2) + flag set)
 * ---------------------------------------------------------------------- */
static void
sigchld_handler(int sig)
{
    (void)sig;
    /* A single byte wakes select(); repeated SIGCHLDs coalesce harmlessly. */
    char b = 'c';
    while (write(g_pipe_wr, &b, 1) < 0 && errno == EINTR)
        ;
}

static void
sigterm_handler(int sig)
{
    (void)sig;
    g_got_sigterm = 1;
    char b = 't';
    while (write(g_pipe_wr, &b, 1) < 0 && errno == EINTR)
        ;
}

/* -------------------------------------------------------------------------
 * Config parsing
 * ---------------------------------------------------------------------- */
static int
parse_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "supervisor: cannot open config '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[MAX_LINE];
    int lineno = 0;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        line[strcspn(line, "\n")] = '\0';

        /* Skip comments and blank lines. */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        if (g_nchildren >= MAX_CHILDREN) {
            fprintf(stderr, "supervisor: config line %d ignored "
                    "(MAX_CHILDREN=%d reached)\n", lineno, MAX_CHILDREN);
            continue;
        }

        child_t *c = &g_children[g_nchildren];
        memset(c, 0, sizeof *c);
        c->backoff_s = BACKOFF_BASE_S;

        /* Tokenise in a copy so we can strdup each token independently. */
        char *copy = strdup(p);
        if (!copy) { fclose(f); return -1; }

        int argc = 0;
        char *tok = strtok(copy, " \t");
        while (tok && argc < MAX_ARGS) {
            c->argv[argc] = strdup(tok);
            if (!c->argv[argc]) { free(copy); fclose(f); return -1; }
            argc++;
            tok = strtok(NULL, " \t");
        }
        c->argv[argc] = NULL;
        c->argc = argc;
        free(copy);

        if (argc > 0)
            g_nchildren++;
    }

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * Child lifecycle
 * ---------------------------------------------------------------------- */
static int
start_child(int i)
{
    child_t *c = &g_children[i];

    pid_t pid = fork();
    if (pid < 0) {
        log_msg("fork failed for child[%d] (%s): %s",
                i, c->argv[0], strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process: restore default signal disposition and close pipe. */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        close(g_pipe_rd);
        close(g_pipe_wr);

        execvp(c->argv[0], c->argv);
        fprintf(stderr, "supervisor: execvp '%s': %s\n",
                c->argv[0], strerror(errno));
        _exit(127);
    }

    c->pid        = pid;
    c->running    = 1;
    c->started_at = time(NULL);
    log_msg("started child[%d] pid=%d cmd=%s", i, pid, c->argv[0]);
    return 0;
}

/*
 * Record a crash and decide whether to schedule a restart.
 * Returns 1 if the child should be restarted, 0 if the cutoff fired.
 */
static int
record_crash(int i)
{
    child_t *c = &g_children[i];
    time_t now = time(NULL);

    /* Optionally reset backoff if the child ran long enough this time. */
    if (c->started_at > 0 && (now - c->started_at) >= HEALTHY_RUN_S) {
        c->backoff_s = BACKOFF_BASE_S;
        log_msg("child[%d] (%s) ran for %lds — resetting backoff",
                i, c->argv[0], (long)(now - c->started_at));
    }

    /* Insert into circular crash-time buffer. */
    c->crash_times[c->crash_head] = now;
    c->crash_head = (c->crash_head + 1) % CRASH_LIMIT;
    if (c->crash_count < CRASH_LIMIT)
        c->crash_count++;

    /* Crash-loop check: are all CRASH_LIMIT recent crashes within the window? */
    if (c->crash_count == CRASH_LIMIT) {
        /*
         * The oldest crash is at crash_head (the slot we're about to
         * overwrite next time, i.e. the one written CRASH_LIMIT steps ago).
         */
        int oldest_idx = c->crash_head; /* head already advanced past newest */
        time_t oldest  = c->crash_times[oldest_idx];
        if ((now - oldest) <= CRASH_WINDOW_S) {
            log_msg("child[%d] (%s) crash-looping: %d crashes in %lds "
                    "— giving up",
                    i, c->argv[0], CRASH_LIMIT, (long)(now - oldest));
            c->stopped = 1;
            return 0;
        }
    }

    /* Schedule restart with current backoff. */
    c->restart_after = now + c->backoff_s;
    log_msg("child[%d] (%s) will restart in %ds (backoff)",
            i, c->argv[0], c->backoff_s);

    c->backoff_s *= 2;
    if (c->backoff_s > BACKOFF_MAX_S)
        c->backoff_s = BACKOFF_MAX_S;

    return 1;
}

/*
 * Reap all dead children without blocking.
 * Returns the number of children still running.
 */
static int
reap_children(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find which slot owns this pid. */
        int idx = -1;
        for (int i = 0; i < g_nchildren; i++) {
            if (g_children[i].pid == pid) { idx = i; break; }
        }
        if (idx < 0) {
            log_msg("reaped unexpected pid=%d (ignored)", pid);
            continue;
        }

        child_t *c = &g_children[idx];
        c->running = 0;
        c->pid     = -1;

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            log_msg("child[%d] (%s) exited status=%d",
                    idx, c->argv[0], code);
            if (code == 0) {
                /* Clean exit: supervisor does not restart. */
                c->restart_after = 0;
            } else {
                record_crash(idx);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            log_msg("child[%d] (%s) killed by signal %d (%s)",
                    idx, c->argv[0], sig, strsignal(sig));
            record_crash(idx);
        }
    }

    int running = 0;
    for (int i = 0; i < g_nchildren; i++)
        if (g_children[i].running) running++;
    return running;
}

static void
forward_signal_all(int sig)
{
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].running && g_children[i].pid > 0)
            kill(g_children[i].pid, sig);
    }
}

static void
wait_all_blocking(void)
{
    for (int i = 0; i < g_nchildren; i++) {
        child_t *c = &g_children[i];
        if (!c->running || c->pid <= 0) continue;
        int status;
        if (waitpid(c->pid, &status, 0) > 0) {
            c->running = 0;
            c->pid     = -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: supervisor <config-file>\n");
        return 1;
    }

    /* ---- Self-pipe ---- */
    int pfds[2];
    if (pipe(pfds) < 0) { perror("pipe"); return 1; }
    g_pipe_rd = pfds[0];
    g_pipe_wr = pfds[1];

    /* Write end: non-blocking so the handler never stalls. */
    {
        int fl = fcntl(g_pipe_wr, F_GETFL);
        fcntl(g_pipe_wr, F_SETFL, fl | O_NONBLOCK);
    }
    /* Both ends: close-on-exec so children do not inherit them. */
    fcntl(g_pipe_rd, F_SETFD, FD_CLOEXEC);
    fcntl(g_pipe_wr, F_SETFD, FD_CLOEXEC);

    /* ---- Signal handlers ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) { perror("sigaction SIGCHLD"); return 1; }

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    if (sigaction(SIGTERM, &sa, NULL) < 0) { perror("sigaction SIGTERM"); return 1; }
    if (sigaction(SIGINT,  &sa, NULL) < 0) { perror("sigaction SIGINT");  return 1; }

    /* ---- Parse config ---- */
    if (parse_config(argv[1]) < 0) return 1;
    if (g_nchildren == 0) {
        fprintf(stderr, "supervisor: no children configured\n");
        return 1;
    }
    log_msg("configured %d child(ren)", g_nchildren);

    /* ---- Initial start ---- */
    for (int i = 0; i < g_nchildren; i++)
        start_child(i);

    /* ---- Main event loop ---- */
    for (;;) {
        /*
         * Compute select() timeout: the minimum time until any pending
         * restart becomes due (or 10 s if nothing is pending).
         */
        time_t now = time(NULL);
        time_t min_wait = 10;

        for (int i = 0; i < g_nchildren; i++) {
            child_t *c = &g_children[i];
            if (!c->running && !c->stopped && c->restart_after > 0) {
                time_t w = c->restart_after - now;
                if (w < 0) w = 0;
                if (w < min_wait) min_wait = w;
            }
        }

        struct timeval tv = { .tv_sec = min_wait, .tv_usec = 0 };
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(g_pipe_rd, &rset);

        int nready = select(g_pipe_rd + 1, &rset, NULL, NULL, &tv);
        if (nready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Drain the pipe — we act on the actual child state, not the bytes. */
        if (nready > 0 && FD_ISSET(g_pipe_rd, &rset)) {
            char buf[64];
            (void)read(g_pipe_rd, buf, sizeof buf);
        }

        /* Handle shutdown. */
        if (g_got_sigterm) {
            log_msg("shutdown signal received — forwarding SIGTERM to children");
            forward_signal_all(SIGTERM);
            wait_all_blocking();
            break;
        }

        /* Reap dead children and schedule restarts. */
        reap_children();

        now = time(NULL);
        for (int i = 0; i < g_nchildren; i++) {
            child_t *c = &g_children[i];
            if (!c->running && !c->stopped
                    && c->restart_after > 0 && c->restart_after <= now) {
                c->restart_after = 0;
                start_child(i);
            }
        }
    }

    log_msg("exiting");
    return 0;
}
