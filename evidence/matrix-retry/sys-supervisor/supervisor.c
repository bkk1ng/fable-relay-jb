/*
 * supervisor.c — C11, Linux
 *
 * Process supervisor: starts N children defined in a config file (one command
 * per line), reaps them via SIGCHLD + self-pipe, restarts crashed children
 * with exponential backoff and a crash-loop cutoff, forwards SIGTERM/SIGINT,
 * and logs child exit status and signals.
 *
 * Config file format:
 *   # comment
 *   /path/to/prog arg1 arg2
 *   another-prog
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -o supervisor supervisor.c
 *
 * Run:
 *   ./supervisor children.conf
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */
#define MAX_CHILDREN      64
#define MAX_ARGS          32
#define MAX_LINE          512
#define BACKOFF_INIT_MS   1000LL       /* initial restart delay */
#define BACKOFF_MAX_MS    60000LL      /* cap at 60 s */
#define MAX_RESTARTS      10           /* crash-loop cutoff: give up after N */
#define STABLE_UPTIME_S   30           /* uptime threshold to reset backoff */

/* -------------------------------------------------------------------------
 * Self-pipe file descriptors (global so signal handlers can reach them)
 * ---------------------------------------------------------------------- */
static int g_pipe_rd = -1;
static int g_pipe_wr = -1;

/* -------------------------------------------------------------------------
 * Signal flags
 * ---------------------------------------------------------------------- */
static volatile sig_atomic_t g_running      = 1;  /* cleared on SIGTERM/SIGINT */
static volatile sig_atomic_t g_need_forward = 0;  /* set once to forward SIGTERM */

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */
typedef enum {
    STATE_RUNNING,
    STATE_BACKOFF,   /* waiting before next restart */
    STATE_DEAD,      /* permanently stopped */
} ChildState;

typedef struct {
    char  *argv[MAX_ARGS + 1]; /* null-terminated */
    int    argc;
    char   cmd_str[MAX_LINE];  /* for log messages */
} ChildConfig;

typedef struct {
    ChildConfig     *cfg;
    pid_t            pid;
    ChildState       state;
    int              restart_count;
    long long        backoff_ms;
    struct timespec  next_restart;  /* CLOCK_MONOTONIC: when backoff expires */
    struct timespec  start_time;    /* CLOCK_MONOTONIC: when last launched */
} Child;

static Child g_children[MAX_CHILDREN];
static int   g_nchildren = 0;

/* -------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------- */
static void log_msg(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ldZ] supervisor: ", tbuf, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* -------------------------------------------------------------------------
 * Signal handlers (async-signal-safe)
 * ---------------------------------------------------------------------- */
static void handle_sigchld(int sig)
{
    (void)sig;
    /* A single byte wakes poll(); overflow is fine — we drain and loop. */
    char c = 1;
    (void)write(g_pipe_wr, &c, 1);
}

static void handle_sigterm(int sig)
{
    (void)sig;
    g_running      = 0;
    g_need_forward = 1;
    char c = 2;
    (void)write(g_pipe_wr, &c, 1);
}

/* -------------------------------------------------------------------------
 * Time helpers
 * ---------------------------------------------------------------------- */
static struct timespec timespec_add_ms(struct timespec base, long long ms)
{
    base.tv_sec  += ms / 1000;
    base.tv_nsec += (ms % 1000) * 1000000L;
    if (base.tv_nsec >= 1000000000L) {
        base.tv_sec++;
        base.tv_nsec -= 1000000000L;
    }
    return base;
}

/* Returns a - b in milliseconds. */
static long long timespec_diff_ms(const struct timespec *a,
                                   const struct timespec *b)
{
    return (long long)(a->tv_sec  - b->tv_sec)  * 1000LL
         + (a->tv_nsec - b->tv_nsec) / 1000000LL;
}

/* -------------------------------------------------------------------------
 * Child lifecycle
 * ---------------------------------------------------------------------- */
static int spawn_child(Child *c)
{
    pid_t pid = fork();
    if (pid < 0) {
        log_msg("fork failed for '%s': %s", c->cfg->cmd_str, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* Child: close the self-pipe and reset inherited signal dispositions. */
        close(g_pipe_rd);
        close(g_pipe_wr);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        execvp(c->cfg->argv[0], c->cfg->argv);
        fprintf(stderr, "supervisor: execvp '%s': %s\n",
                c->cfg->argv[0], strerror(errno));
        _exit(127);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    c->pid        = pid;
    c->state      = STATE_RUNNING;
    c->start_time = now;
    log_msg("started '%s' pid=%d restart_count=%d",
            c->cfg->cmd_str, pid, c->restart_count);
    return 0;
}

static void schedule_restart(Child *c)
{
    if (c->restart_count >= MAX_RESTARTS) {
        log_msg("'%s' exceeded max restarts (%d), giving up",
                c->cfg->cmd_str, MAX_RESTARTS);
        c->state = STATE_DEAD;
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    c->next_restart = timespec_add_ms(now, c->backoff_ms);
    c->state        = STATE_BACKOFF;
    log_msg("'%s' scheduled restart in %lldms (attempt %d/%d)",
            c->cfg->cmd_str, c->backoff_ms,
            c->restart_count + 1, MAX_RESTARTS);

    c->backoff_ms = c->backoff_ms * 2;
    if (c->backoff_ms > BACKOFF_MAX_MS)
        c->backoff_ms = BACKOFF_MAX_MS;

    c->restart_count++;
}

static void handle_child_exit(pid_t pid, int status)
{
    for (int i = 0; i < g_nchildren; i++) {
        Child *c = &g_children[i];
        if (c->pid != pid)
            continue;

        c->pid = -1;

        /* Log what happened. */
        if (WIFEXITED(status)) {
            log_msg("'%s' pid=%d exited status=%d",
                    c->cfg->cmd_str, pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_msg("'%s' pid=%d killed by signal %d (%s)",
                    c->cfg->cmd_str, pid,
                    WTERMSIG(status), strsignal(WTERMSIG(status)));
        }

        /* If the supervisor is shutting down, don't restart anyone. */
        if (!g_running) {
            c->state = STATE_DEAD;
            return;
        }

        /* Clean exit (status 0): do not restart. */
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            log_msg("'%s' exited cleanly, not restarting", c->cfg->cmd_str);
            c->state         = STATE_DEAD;
            c->backoff_ms    = BACKOFF_INIT_MS;
            c->restart_count = 0;
            return;
        }

        /* If the child ran long enough it was stable; reset crash counters. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long uptime_s = (long long)(now.tv_sec - c->start_time.tv_sec);
        if (uptime_s >= STABLE_UPTIME_S) {
            c->backoff_ms    = BACKOFF_INIT_MS;
            c->restart_count = 0;
        }

        schedule_restart(c);
        return;
    }
    log_msg("reaped unknown pid=%d (status=%d)", pid, status);
}

static void reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        handle_child_exit(pid, status);
}

/* -------------------------------------------------------------------------
 * Backoff scheduling
 * ---------------------------------------------------------------------- */

/*
 * Returns milliseconds until the earliest pending restart, or -1 if none.
 * Returns 0 if at least one restart is already overdue.
 */
static int next_timeout_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long min_ms = -1;

    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].state != STATE_BACKOFF)
            continue;
        long long diff = timespec_diff_ms(&g_children[i].next_restart, &now);
        if (diff <= 0)
            return 0;
        if (min_ms < 0 || diff < min_ms)
            min_ms = diff;
    }
    if (min_ms > (long long)INT_MAX)
        return INT_MAX;
    return (int)min_ms;
}

static void maybe_restart_backoff(void)
{
    if (!g_running)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < g_nchildren; i++) {
        Child *c = &g_children[i];
        if (c->state != STATE_BACKOFF)
            continue;
        if (timespec_diff_ms(&c->next_restart, &now) <= 0)
            spawn_child(c);
    }
}

/* Mark BACKOFF children dead during shutdown (they were never running). */
static void cancel_backoff_children(void)
{
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].state == STATE_BACKOFF) {
            log_msg("'%s' cancelled (shutdown during backoff)",
                    g_children[i].cfg->cmd_str);
            g_children[i].state = STATE_DEAD;
        }
    }
}

static bool all_dead(void)
{
    for (int i = 0; i < g_nchildren; i++)
        if (g_children[i].state != STATE_DEAD)
            return false;
    return true;
}

static void forward_signal_to_children(int sig)
{
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].state == STATE_RUNNING && g_children[i].pid > 0)
            kill(g_children[i].pid, sig);
    }
}

/* -------------------------------------------------------------------------
 * Config parsing
 * ---------------------------------------------------------------------- */
static int parse_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "supervisor: open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing whitespace / newline. */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
                           || line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#')
            continue;

        if (g_nchildren >= MAX_CHILDREN) {
            fprintf(stderr, "supervisor: too many children (max %d)\n",
                    MAX_CHILDREN);
            break;
        }

        ChildConfig *cfg = calloc(1, sizeof(*cfg));
        if (!cfg) { perror("calloc"); fclose(f); return -1; }
        strncpy(cfg->cmd_str, line, sizeof(cfg->cmd_str) - 1);

        /* Tokenise into argv. */
        char *copy = strdup(line);
        if (!copy) { perror("strdup"); free(cfg); fclose(f); return -1; }
        char *saveptr = NULL;
        char *tok     = strtok_r(copy, " \t", &saveptr);
        while (tok && cfg->argc < MAX_ARGS) {
            cfg->argv[cfg->argc++] = strdup(tok);
            tok = strtok_r(NULL, " \t", &saveptr);
        }
        cfg->argv[cfg->argc] = NULL;
        free(copy);

        if (cfg->argc == 0) { free(cfg); continue; }

        Child *c = &g_children[g_nchildren++];
        *c = (Child){
            .cfg          = cfg,
            .pid          = -1,
            .state        = STATE_BACKOFF,  /* triggers immediate start */
            .backoff_ms   = BACKOFF_INIT_MS,
            .restart_count = 0,
        };
        /* Schedule for "right now". */
        clock_gettime(CLOCK_MONOTONIC, &c->next_restart);
    }

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config-file>\n", argv[0]);
        return 1;
    }

    /* Self-pipe: O_NONBLOCK on both ends, O_CLOEXEC to not leak into children. */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("pipe2");
        return 1;
    }
    g_pipe_rd = pipefd[0];
    g_pipe_wr = pipefd[1];

    /* Install signal handlers. */
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) { perror("sigaction SIGCHLD"); return 1; }

    sa.sa_handler = handle_sigterm;
    sa.sa_flags   = 0;
    if (sigaction(SIGTERM, &sa, NULL) < 0) { perror("sigaction SIGTERM"); return 1; }
    if (sigaction(SIGINT,  &sa, NULL) < 0) { perror("sigaction SIGINT");  return 1; }

    /* Ignore SIGPIPE (write to a gone child's stdio etc.). */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    if (parse_config(argv[1]) < 0)
        return 1;

    if (g_nchildren == 0) {
        fprintf(stderr, "supervisor: no children defined in '%s'\n", argv[1]);
        return 1;
    }

    log_msg("supervising %d child(ren)", g_nchildren);

    /* Kick off all children whose next_restart <= now (all of them, initially). */
    maybe_restart_backoff();

    struct pollfd pfd = { .fd = g_pipe_rd, .events = POLLIN };

    while (!all_dead()) {
        /*
         * During normal operation: use the backoff timeout so we wake up to
         * restart a child on schedule.  During shutdown: poll with a short
         * timeout to quickly notice when children die.
         */
        int timeout_ms;
        if (g_running) {
            timeout_ms = next_timeout_ms();
            if (timeout_ms < 0)
                timeout_ms = 5000; /* nothing pending, check every 5 s */
        } else {
            timeout_ms = 500;
        }

        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* Drain the self-pipe regardless of which signal wrote to it. */
        if (r > 0 && (pfd.revents & POLLIN)) {
            char buf[256];
            while (read(g_pipe_rd, buf, sizeof(buf)) > 0) {}
        }

        /* Handle shutdown request (SIGTERM / SIGINT). */
        if (g_need_forward) {
            g_need_forward = 0;
            log_msg("shutdown requested; forwarding SIGTERM to children");
            cancel_backoff_children();
            forward_signal_to_children(SIGTERM);
        }

        /* Reap any children that have exited. */
        reap_children();

        /* Restart any that have come out of backoff (only if still running). */
        maybe_restart_backoff();
    }

    log_msg("all children done, exiting");
    close(g_pipe_rd);
    close(g_pipe_wr);
    return 0;
}
