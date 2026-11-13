/*
 * supervisor.c — C11/Linux process supervisor
 *
 * Starts N children from a config table, reaps via SIGCHLD + self-pipe,
 * restarts crashed children with exponential backoff, stops restarting
 * after MAX_RESTARTS attempts (crash-loop cutoff), forwards SIGTERM/SIGINT,
 * and logs child exit status / termination signal.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -o supervisor supervisor.c
 *
 * Run:
 *   ./supervisor
 */

#define _GNU_SOURCE          /* strsignal, clock_gettime on older glibc */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── tunables ──────────────────────────────────────────────────────── */

#define MAX_CHILDREN    16
#define MAX_RESTARTS    10       /* per-child crash-loop cutoff          */
#define BACKOFF_BASE_MS 500L     /* first restart delay (ms)             */
#define BACKOFF_MAX_MS  30000L   /* maximum restart delay (ms)           */
#define CRASH_WINDOW_S  10L      /* uptime below this keeps backoff      */

/* ── child config table ────────────────────────────────────────────── *
 *
 * Each row is a NULL-terminated argv array.  Edit to taste.
 * The first element is passed to execvp(), so it is searched on PATH.
 */
static const char *child_configs[][16] = {
    { "/bin/sh", "-c", "echo 'worker-0 up'; sleep 3; exit 1", NULL },
    { "/bin/sh", "-c", "echo 'worker-1 up'; sleep 60",        NULL },
};

#define NUM_CHILDREN ((int)(sizeof(child_configs) / sizeof(child_configs[0])))

/* ── types ─────────────────────────────────────────────────────────── */

typedef struct {
    const char    **argv;
    pid_t           pid;            /* >0 running, -1 not running        */
    int             restart_count;
    int             dead;           /* 1 = cutoff reached, never respawn */
    struct timespec next_restart;   /* CLOCK_MONOTONIC: earliest respawn */
    struct timespec last_start;     /* CLOCK_MONOTONIC: last fork time   */
} Child;

/* ── globals ───────────────────────────────────────────────────────── */

static Child    children[MAX_CHILDREN];
static int      num_children;
static int      selfpipe[2];            /* [0] read, [1] write           */

static volatile sig_atomic_t g_terminate;

/* ── logging ───────────────────────────────────────────────────────── */

static void log_msg(const char *fmt, ...)
{
    char        buf[512];
    struct timespec ts;
    struct tm   tm_info;
    va_list     ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm_info);

    int n = (int)strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  ".%03ldZ  ", ts.tv_nsec / 1000000L);

    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
}

/* ── timespec helpers ──────────────────────────────────────────────── */

static void ts_add_ms(struct timespec *ts, long ms)
{
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Returns >0 if a>b, <0 if a<b, 0 if equal. */
static int ts_cmp(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec  != b->tv_sec)  return a->tv_sec  > b->tv_sec  ? 1 : -1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec > b->tv_nsec ? 1 : -1;
    return 0;
}

/* Milliseconds between a and b (a - b); clamps to 0 if negative. */
static long ts_diff_ms(const struct timespec *a, const struct timespec *b)
{
    long ms = (a->tv_sec  - b->tv_sec)  * 1000L
            + (a->tv_nsec - b->tv_nsec) / 1000000L;
    return ms > 0 ? ms : 0;
}

/* ── backoff ───────────────────────────────────────────────────────── */

static long backoff_ms(int n)
{
    long ms = BACKOFF_BASE_MS;
    for (int i = 0; i < n; i++) {
        ms *= 2;
        if (ms >= BACKOFF_MAX_MS) return BACKOFF_MAX_MS;
    }
    return ms;
}

/* ── signal handlers (async-signal-safe only) ──────────────────────── */

static void sigchld_handler(int sig)
{
    (void)sig;
    char c = 'c';
    ssize_t r;
    do { r = write(selfpipe[1], &c, 1); } while (r < 0 && errno == EINTR);
}

static void sigterm_handler(int sig)
{
    (void)sig;
    g_terminate = 1;
    char c = 't';
    ssize_t r;
    do { r = write(selfpipe[1], &c, 1); } while (r < 0 && errno == EINTR);
}

/* ── spawn ─────────────────────────────────────────────────────────── */

static int spawn_child(int idx)
{
    Child *c = &children[idx];

    pid_t pid = fork();
    if (pid < 0) {
        log_msg("supervisor: fork[%d] failed: %s", idx, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* child process — restore default signal disposition */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        /* close inherited pipe ends */
        close(selfpipe[0]);
        close(selfpipe[1]);

        execvp(c->argv[0], (char *const *)c->argv);
        /* only reached on execvp failure */
        fprintf(stderr, "supervisor: execvp(%s): %s\n",
                c->argv[0], strerror(errno));
        _exit(127);
    }

    c->pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &c->last_start);
    log_msg("supervisor: spawned child[%d] pid=%d argv[0]=%s",
            idx, (int)pid, c->argv[0]);
    return 0;
}

/* ── exit handling ─────────────────────────────────────────────────── */

static void handle_exit(int idx, int wstatus)
{
    Child *c = &children[idx];
    c->pid = -1;

    if (WIFEXITED(wstatus)) {
        log_msg("supervisor: child[%d] exited with status %d",
                idx, WEXITSTATUS(wstatus));
    } else if (WIFSIGNALED(wstatus)) {
        log_msg("supervisor: child[%d] killed by signal %d (%s)",
                idx, WTERMSIG(wstatus), strsignal(WTERMSIG(wstatus)));
    } else {
        log_msg("supervisor: child[%d] unknown wait status 0x%x", idx, wstatus);
    }

    if (c->dead)
        return;

    /* Reset backoff if child lived long enough to be considered healthy. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long uptime_s = (long)(now.tv_sec - c->last_start.tv_sec);
    if (uptime_s >= CRASH_WINDOW_S)
        c->restart_count = 0;

    if (c->restart_count >= MAX_RESTARTS) {
        log_msg("supervisor: child[%d] hit restart limit (%d); abandoning",
                idx, MAX_RESTARTS);
        c->dead = 1;
        return;
    }

    long delay = backoff_ms(c->restart_count);
    c->next_restart = now;
    ts_add_ms(&c->next_restart, delay);
    c->restart_count++;

    log_msg("supervisor: child[%d] restart %d/%d in %ld ms",
            idx, c->restart_count, MAX_RESTARTS, delay);
}

/* ── reap loop ─────────────────────────────────────────────────────── */

static void reap_children(void)
{
    for (;;) {
        int wstatus;
        pid_t pid = waitpid(-1, &wstatus, WNOHANG);
        if (pid <= 0)
            break;
        int found = 0;
        for (int i = 0; i < num_children; i++) {
            if (children[i].pid == pid) {
                handle_exit(i, wstatus);
                found = 1;
                break;
            }
        }
        if (!found)
            log_msg("supervisor: reaped unknown pid=%d status=0x%x",
                    (int)pid, wstatus);
    }
}

/* ── restart scheduler ─────────────────────────────────────────────── */

static void schedule_restarts(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int all_dead = 1;
    for (int i = 0; i < num_children; i++) {
        Child *c = &children[i];
        if (c->dead)
            continue;
        all_dead = 0;
        if (c->pid > 0)
            continue; /* running */
        if (ts_cmp(&now, &c->next_restart) >= 0)
            spawn_child(i);
    }

    if (all_dead)
        log_msg("supervisor: all children exhausted restarts; idle");
}

/* Returns how long until the next pending restart, or NULL if none. */
static struct timeval *next_timeout(struct timeval *tv)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long min_ms = LONG_MAX;
    int  found  = 0;

    for (int i = 0; i < num_children; i++) {
        Child *c = &children[i];
        if (c->dead || c->pid > 0)
            continue;
        found = 1;
        long ms = ts_diff_ms(&c->next_restart, &now);
        if (ms < min_ms)
            min_ms = ms;
    }

    if (!found)
        return NULL; /* nothing pending — block indefinitely */

    tv->tv_sec  = min_ms / 1000;
    tv->tv_usec = (min_ms % 1000) * 1000;
    return tv;
}

/* ── shutdown helpers ──────────────────────────────────────────────── */

static void forward_sigterm(void)
{
    for (int i = 0; i < num_children; i++)
        if (children[i].pid > 0)
            kill(children[i].pid, SIGTERM);
}

static void wait_all(void)
{
    for (int i = 0; i < num_children; i++) {
        if (children[i].pid > 0) {
            int wstatus;
            waitpid(children[i].pid, &wstatus, 0);
            handle_exit(i, wstatus);
        }
    }
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
    /* validate config size */
    num_children = NUM_CHILDREN;
    if (num_children > MAX_CHILDREN) {
        fprintf(stderr, "supervisor: too many children configured (max %d)\n",
                MAX_CHILDREN);
        return 1;
    }

    /* initialise child table */
    struct timespec zero = { 0, 0 };
    for (int i = 0; i < num_children; i++) {
        children[i] = (Child){
            .argv          = child_configs[i],
            .pid           = -1,
            .restart_count = 0,
            .dead          = 0,
            .next_restart  = zero,
            .last_start    = zero,
        };
    }

    /* self-pipe: both ends non-blocking so the handler never blocks */
    if (pipe(selfpipe) < 0) { perror("pipe"); return 1; }
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(selfpipe[i], F_GETFL);
        if (fl < 0 || fcntl(selfpipe[i], F_SETFL, fl | O_NONBLOCK) < 0) {
            perror("fcntl O_NONBLOCK"); return 1;
        }
    }

    /* signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) { perror("sigaction SIGCHLD"); return 1; }

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    if (sigaction(SIGTERM, &sa, NULL) < 0) { perror("sigaction SIGTERM"); return 1; }
    if (sigaction(SIGINT,  &sa, NULL) < 0) { perror("sigaction SIGINT");  return 1; }

    signal(SIGPIPE, SIG_IGN);

    /* initial spawn */
    log_msg("supervisor: starting %d child(ren)", num_children);
    for (int i = 0; i < num_children; i++)
        spawn_child(i);

    /* main loop */
    while (!g_terminate) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(selfpipe[0], &rset);

        struct timeval  tv;
        struct timeval *tvp = next_timeout(&tv);

        int rc = select(selfpipe[0] + 1, &rset, NULL, NULL, tvp);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* drain the pipe — we don't care how many bytes are there */
        if (rc > 0 && FD_ISSET(selfpipe[0], &rset)) {
            char drain[64];
            while (read(selfpipe[0], drain, sizeof(drain)) > 0)
                ;
        }

        reap_children();
        schedule_restarts();
    }

    /* clean shutdown */
    log_msg("supervisor: shutting down, forwarding SIGTERM to children");
    forward_sigterm();
    wait_all();
    log_msg("supervisor: done");
    return 0;
}
