/*
 * supervisor.c — C11 Linux process supervisor
 *
 * Starts N child processes specified on the command line, separated by the
 * token "::".  Monitors them via SIGCHLD delivered through a self-pipe,
 * restarts crashed children with exponential backoff, stops restarting after
 * too many consecutive crashes (crash-loop cutoff), forwards SIGTERM to all
 * children, and logs every exit with its status or signal.
 *
 * Usage:
 *   supervisor cmd [arg...] [:: cmd2 [arg...]] ...
 *
 *   supervisor sleep 5 :: /usr/bin/myserver --port 8080
 *
 * Compile:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -o supervisor supervisor.c
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── tunables ────────────────────────────────────────────────────────────── */

#define MAX_CHILDREN      64   /* maximum supervised processes               */
#define BACKOFF_INIT_S     1   /* first restart delay, seconds               */
#define BACKOFF_MAX_S     60   /* ceiling on restart delay, seconds          */
#define CRASH_WINDOW_S    60   /* rolling window for crash-loop detection    */
#define CRASH_LOOP_LIMIT   5   /* crashes within window before giving up     */

/* ── self-pipe (written by signal handlers, read by main loop) ───────────── */

static int sig_pipe[2];   /* [0] = read end, [1] = write end */

/* ── per-child state ─────────────────────────────────────────────────────── */

typedef struct {
    char  **argv;          /* NULL-terminated exec argument vector           */
    pid_t   pid;           /* current pid; 0 means not running               */
    int     total_starts;  /* total times this child has been started        */
    int     window_crashes;/* crashes counted in the current window          */
    time_t  window_start;  /* epoch second when the current window opened    */
    int     backoff_s;     /* current backoff interval in seconds            */
    time_t  retry_at;      /* earliest epoch second at which we may restart  */
    bool    stopped;       /* true = permanently stopped, never restart      */
} Child;

static Child children[MAX_CHILDREN];
static int   nchildren;

/* Set by SIGTERM handler; checked at the top of every main-loop iteration. */
static volatile sig_atomic_t got_sigterm;

/* ── logging ─────────────────────────────────────────────────────────────── */

static void vlog(const char *level, const char *fmt, va_list ap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", &tm);

    fprintf(stderr, "[%s.%03ldZ] supervisor %s: ",
            tbuf, ts.tv_nsec / 1000000L, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("INFO ", fmt, ap);
    va_end(ap);
}

static void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("WARN ", fmt, ap);
    va_end(ap);
}

static void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("ERROR", fmt, ap);
    va_end(ap);
}

/* ── signal handlers ─────────────────────────────────────────────────────── */

/*
 * Write one byte to the self-pipe.  Async-signal-safe: write(2) is in the
 * list; we retry only on EINTR (which cannot loop here, but be explicit).
 */
static void pipe_notify(char tag)
{
    ssize_t n;
    do { n = write(sig_pipe[1], &tag, 1); } while (n < 0 && errno == EINTR);
}

static void sigchld_handler(int sig) { (void)sig; pipe_notify('c'); }
static void sigterm_handler(int sig)  { (void)sig; got_sigterm = 1; pipe_notify('t'); }

/* ── fd helpers ──────────────────────────────────────────────────────────── */

static void fd_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) fl = 0;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void fd_set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0) fl = 0;
    fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

/* ── child lifecycle ─────────────────────────────────────────────────────── */

static const char *child_name(const Child *c) { return c->argv[0]; }

/* Fork and exec one child.  Returns 0 on success, -1 if fork fails. */
static int start_child(Child *c)
{
    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork for %s: %s", child_name(c), strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /*
         * Child process: reset all signal dispositions to defaults and close
         * the supervisor's self-pipe before exec so the child does not
         * inherit unexpected file descriptors.
         */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        close(sig_pipe[0]);
        close(sig_pipe[1]);

        execvp(c->argv[0], c->argv);
        /* exec only returns on failure */
        fprintf(stderr, "supervisor: execvp %s: %s\n",
                c->argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent: record the new pid. */
    c->pid = pid;
    c->total_starts++;
    log_info("started '%s' pid=%d (start #%d)",
             child_name(c), (int)pid, c->total_starts);
    return 0;
}

/*
 * After a crash, decide whether and when to restart.
 * Updates c->retry_at and c->stopped.
 * Returns true if a future restart has been scheduled.
 */
static bool schedule_restart(Child *c, time_t now)
{
    if (c->stopped) return false;

    /* Roll the crash window if it has expired. */
    if (now - c->window_start > CRASH_WINDOW_S) {
        c->window_start  = now;
        c->window_crashes = 0;
    }
    c->window_crashes++;

    if (c->window_crashes >= CRASH_LOOP_LIMIT) {
        log_error("'%s': crash-loop limit hit (%d crashes in %ds) — "
                  "giving up permanently",
                  child_name(c), c->window_crashes, CRASH_WINDOW_S);
        c->stopped = true;
        return false;
    }

    /* Exponential backoff: 1 → 2 → 4 → … → BACKOFF_MAX_S */
    if (c->backoff_s == 0) {
        c->backoff_s = BACKOFF_INIT_S;
    } else {
        c->backoff_s *= 2;
        if (c->backoff_s > BACKOFF_MAX_S)
            c->backoff_s = BACKOFF_MAX_S;
    }

    c->retry_at = now + c->backoff_s;
    log_info("'%s': restart scheduled in %ds "
             "(crash %d/%d in window, total starts %d)",
             child_name(c), c->backoff_s,
             c->window_crashes, CRASH_LOOP_LIMIT - 1,
             c->total_starts);
    return true;
}

/*
 * Drain waitpid(WNOHANG), log each exit, and schedule restarts for crashes.
 * A child that exits with status 0 is considered done and is not restarted.
 */
static void reap_children(void)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        Child *c = NULL;
        for (int i = 0; i < nchildren; i++) {
            if (children[i].pid == pid) { c = &children[i]; break; }
        }
        if (!c) {
            log_warn("reaped unknown pid=%d", (int)pid);
            continue;
        }

        c->pid = 0;
        time_t now = time(NULL);

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            log_info("'%s' pid=%d exited with status %d",
                     child_name(c), (int)pid, code);
            if (code == 0) {
                /* Clean exit — treat as intentional; do not restart. */
                c->stopped = true;
            } else {
                schedule_restart(c, now);
            }
        } else if (WIFSIGNALED(status)) {
            log_warn("'%s' pid=%d killed by signal %d (%s)",
                     child_name(c), (int)pid,
                     WTERMSIG(status), strsignal(WTERMSIG(status)));
            schedule_restart(c, now);
        }
        /* WIFSTOPPED/WIFCONTINUED are not expected without WUNTRACED. */
    }
}

/* Send SIGTERM to every currently-running child. */
static void forward_sigterm(void)
{
    for (int i = 0; i < nchildren; i++) {
        if (children[i].pid > 0) {
            kill(children[i].pid, SIGTERM);
        }
    }
}

/*
 * Return the number of seconds until the next pending restart, or -1 if
 * none is pending.  Used to set the select(2) timeout.
 */
static long next_wakeup_seconds(void)
{
    time_t now     = time(NULL);
    long   soonest = -1;

    for (int i = 0; i < nchildren; i++) {
        const Child *c = &children[i];
        if (c->pid != 0 || c->stopped) continue;   /* running or dead-end */

        long wait = (long)(c->retry_at - now);
        if (wait < 0) wait = 0;
        if (soonest < 0 || wait < soonest) soonest = wait;
    }
    return soonest;
}

/* Start any child whose retry_at has passed and that is not currently running. */
static void fire_pending_restarts(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < nchildren; i++) {
        Child *c = &children[i];
        if (c->pid != 0 || c->stopped) continue;
        if (now >= c->retry_at) {
            start_child(c);
        }
    }
}

/* Returns true when every supervised child has permanently stopped. */
static bool all_stopped(void)
{
    for (int i = 0; i < nchildren; i++) {
        if (!children[i].stopped) return false;
    }
    return true;
}

/* ── argument parsing ────────────────────────────────────────────────────── */

/*
 * Split the remaining argv into child specifications delimited by "::".
 *
 *   supervisor sleep 10 :: echo hello :: /usr/bin/myapp --flag
 *
 * Modifies children[] and sets nchildren.
 * Returns 0 on success, -1 on error.
 */
static int parse_children(int argc, char **argv)
{
    int   n     = 0;
    int   start = 0;
    time_t now  = time(NULL);

    for (int i = 0; i <= argc; i++) {
        bool delim = (i == argc) || (strcmp(argv[i], "::") == 0);
        if (!delim) continue;

        int len = i - start;
        if (len == 0) { start = i + 1; continue; }   /* skip empty segments */

        if (n >= MAX_CHILDREN) {
            fprintf(stderr, "supervisor: too many children (max %d)\n",
                    MAX_CHILDREN);
            return -1;
        }

        char **av = calloc((size_t)(len + 1), sizeof *av);
        if (!av) { perror("calloc"); return -1; }
        for (int j = 0; j < len; j++) av[j] = argv[start + j];
        /* av[len] is already NULL from calloc */

        children[n] = (Child){
            .argv         = av,
            .pid          = 0,
            .window_start = now,
        };
        n++;
        start = i + 1;
    }

    nchildren = n;
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Skip argv[0] (program name). */
    argc--;
    argv++;

    if (argc == 0) {
        fprintf(stderr,
                "usage: supervisor cmd [arg...] [:: cmd2 [arg...]] ...\n");
        return 1;
    }

    if (parse_children(argc, argv) != 0 || nchildren == 0) {
        fprintf(stderr, "supervisor: no children to supervise\n");
        return 1;
    }

    /* ── self-pipe setup ── */
    if (pipe(sig_pipe) != 0) { perror("pipe"); return 1; }
    fd_set_nonblock(sig_pipe[0]);
    fd_set_nonblock(sig_pipe[1]);
    fd_set_cloexec(sig_pipe[0]);
    fd_set_cloexec(sig_pipe[1]);

    /* ── signal setup ── */
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sa.sa_handler = sigchld_handler;
    if (sigaction(SIGCHLD, &sa, NULL) != 0) { perror("sigaction SIGCHLD"); return 1; }

    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = sigterm_handler;
    if (sigaction(SIGTERM, &sa, NULL) != 0) { perror("sigaction SIGTERM"); return 1; }

    /* Supervisor ignores SIGINT; children will receive it from the terminal
     * directly (same process group) and may handle it themselves. */
    signal(SIGINT, SIG_IGN);

    /* ── initial launch ── */
    log_info("starting %d child(ren)", nchildren);
    for (int i = 0; i < nchildren; i++) {
        start_child(&children[i]);
    }

    /* ── event loop ── */
    for (;;) {
        /* SIGTERM: forward to children and wait for them to finish. */
        if (got_sigterm) {
            log_info("SIGTERM received — forwarding to all children");
            forward_sigterm();
            /* Blocking wait; children should exit promptly after SIGTERM. */
            int status;
            while (waitpid(-1, &status, 0) > 0 || errno == EINTR)
                ;
            log_info("all children finished — exiting");
            return 0;
        }

        /* If every child has permanently stopped, there is nothing left to do. */
        if (all_stopped()) {
            log_info("all children permanently stopped — exiting");
            return 0;
        }

        /* Build the read fd_set for select(). */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sig_pipe[0], &rset);

        /* Set a timeout so we wake up in time to fire pending restarts. */
        struct timeval  tv  = {0, 0};
        struct timeval *tvp = NULL;
        long secs = next_wakeup_seconds();
        if (secs >= 0) {
            tv.tv_sec = secs;
            tvp = &tv;
        }

        int r = select(sig_pipe[0] + 1, &rset, NULL, NULL, tvp);

        if (r < 0 && errno == EINTR) {
            /* Interrupted by a signal handler; loop back to check got_sigterm. */
            continue;
        }

        /* Drain the self-pipe regardless of which signal(s) arrived. */
        if (r > 0 && FD_ISSET(sig_pipe[0], &rset)) {
            char buf[64];
            while (read(sig_pipe[0], buf, sizeof buf) > 0)
                ;
        }

        /* Reap finished children and schedule restarts. */
        reap_children();

        /* Start any children whose backoff timer has expired. */
        fire_pending_restarts();
    }
}
