/*
 * supervisor.c — C11/Linux process supervisor
 *
 * Reads a config file (one command per line), spawns each as a child process,
 * reaps children via SIGCHLD delivered through a self-pipe, restarts crashed
 * children with exponential backoff, enforces a crash-loop cutoff, forwards
 * SIGTERM to all children on shutdown, and logs child exit status/signal to
 * stderr with timestamps.
 *
 * Usage:
 *   supervisor <config-file>
 *
 * Config file format:
 *   # lines starting with # are comments
 *   /usr/bin/myservice --flag value
 *   ./worker --port 8080 --threads 4
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -o supervisor supervisor.c
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define MAX_CHILDREN       64
#define MAX_ARGS           63     /* per child; argv[MAX_ARGS] is always NULL */
#define BACKOFF_INIT_SECS   1u
#define BACKOFF_MAX_SECS   64u
#define CRASH_LOOP_MAX      5     /* give up after this many crashes in window */
#define CRASH_WINDOW_SECS  60     /* rolling window for crash counting (seconds) */
#define CFG_LINE_MAX      1024

/* -------------------------------------------------------------------------
 * Child descriptor
 * ---------------------------------------------------------------------- */

typedef struct {
    char    *argv[MAX_ARGS + 1]; /* null-terminated argv for execvp */
    int      argc;
    pid_t    pid;                /* 0 = not currently running */
    int      dead;               /* 1 = crash-loop cutoff reached; no more restarts */
    int      crashes;            /* crash count in current window */
    time_t   window_start;       /* wall time of first crash in this window */
    unsigned backoff;            /* seconds to wait before next restart */
    time_t   restart_at;         /* wall time for next restart; 0 = start immediately */
    time_t   started_at;         /* wall time child was last spawned (for window reset) */
} child_t;

/* -------------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */

static child_t              g_children[MAX_CHILDREN];
static int                  g_nchildren = 0;
static int                  g_pipe[2]   = {-1, -1}; /* self-pipe [read, write] */
static volatile sig_atomic_t g_term     = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Log with a wall-clock timestamp prefix. Not async-signal-safe; call only
 * from the main loop, never from signal handlers. */
static void log_msg(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void log_msg(const char *fmt, ...)
{
    char   tbuf[32];
    time_t now = time(NULL);
    struct tm tm_now;

    if (localtime_r(&now, &tm_now) != NULL)
        strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", &tm_now);
    else
        snprintf(tbuf, sizeof tbuf, "?");

    fprintf(stderr, "[%s] supervisor: ", tbuf);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

/* Write one byte to the self-pipe; silently ignores EAGAIN (pipe full). */
static void pipe_signal(void)
{
    char    b = 0;
    ssize_t r;
    do {
        r = write(g_pipe[1], &b, 1);
    } while (r < 0 && errno == EINTR);
    /* EAGAIN is intentional: pipe full means the main loop will wake anyway. */
}

/* Drain every pending byte from the read end of the self-pipe. */
static void pipe_drain(void)
{
    char    buf[64];
    ssize_t r;
    while ((r = read(g_pipe[0], buf, sizeof buf)) > 0)
        ;
    /* EAGAIN / EWOULDBLOCK means we're done; any other error is ignored. */
}

/* -------------------------------------------------------------------------
 * Signal handlers (async-signal-safe only)
 * ---------------------------------------------------------------------- */

static void sigchld_handler(int sig)
{
    (void)sig;
    pipe_signal();
}

static void sigterm_handler(int sig)
{
    (void)sig;
    g_term = 1;
    pipe_signal();
}

/* -------------------------------------------------------------------------
 * Child lifecycle
 * ---------------------------------------------------------------------- */

/* Spawn child i.  Resets the child's pid and started_at on success. */
static void spawn(int i)
{
    child_t *c   = &g_children[i];
    pid_t    pid = fork();

    if (pid < 0) {
        log_msg("fork failed for child[%d] (%s): %s", i, c->argv[0], strerror(errno));
        /* Back off and try again later. */
        c->restart_at = time(NULL) + (time_t)c->backoff;
        return;
    }

    if (pid == 0) {
        /* Child: restore default signal dispositions, then exec. */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        /* Close the self-pipe — it has O_CLOEXEC but be explicit. */
        close(g_pipe[0]);
        close(g_pipe[1]);

        execvp(c->argv[0], c->argv);
        /* execvp only returns on error. */
        fprintf(stderr, "supervisor: execvp %s: %s\n", c->argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent */
    c->pid        = pid;
    c->started_at = time(NULL);
    c->restart_at = 0;
    log_msg("child[%d] pid=%d started: %s", i, (int)pid, c->argv[0]);
}

/* Called when a child with index i has exited with waitpid status. */
static void handle_exit(int i, pid_t pid, int status)
{
    child_t *c   = &g_children[i];
    time_t   now = time(NULL);

    /* Log exit reason. */
    if (WIFEXITED(status)) {
        log_msg("child[%d] pid=%d (%s) exited with status %d",
                i, (int)pid, c->argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_msg("child[%d] pid=%d (%s) killed by signal %d (%s)",
                i, (int)pid, c->argv[0], WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        log_msg("child[%d] pid=%d (%s) exited (unknown status 0x%x)",
                i, (int)pid, c->argv[0], (unsigned)status);
    }

    c->pid = 0;

    /* During shutdown, do not schedule restarts. */
    if (g_term)
        return;

    /* Clean exit (status 0) is treated as intentional; do not restart. */
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return;

    /* If the child ran long enough, reset its crash window and backoff so a
     * one-off crash does not permanently burn through the quota. */
    if (c->crashes > 0 && (now - c->window_start) >= CRASH_WINDOW_SECS) {
        c->crashes = 0;
        c->backoff  = BACKOFF_INIT_SECS;
    }

    /* Open a new window on first crash. */
    if (c->crashes == 0) {
        c->window_start = now;
        c->backoff      = BACKOFF_INIT_SECS;
    }

    c->crashes++;

    if (c->crashes >= CRASH_LOOP_MAX) {
        log_msg("child[%d] (%s) hit crash-loop cutoff (%d crashes in %lds), disabled",
                i, c->argv[0], c->crashes, (long)(now - c->window_start));
        c->dead = 1;
        return;
    }

    c->restart_at = now + (time_t)c->backoff;
    log_msg("child[%d] (%s) scheduled restart in %us (crash #%d of %d)",
            i, c->argv[0], c->backoff, c->crashes, CRASH_LOOP_MAX - 1);

    /* Double backoff, capped at maximum. */
    if (c->backoff < BACKOFF_MAX_SECS / 2)
        c->backoff *= 2;
    else
        c->backoff = BACKOFF_MAX_SECS;
}

/* Reap all available children without blocking. */
static void reap_all(void)
{
    int    status;
    pid_t  pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int found = 0;
        for (int i = 0; i < g_nchildren; i++) {
            if (g_children[i].pid == pid) {
                handle_exit(i, pid, status);
                found = 1;
                break;
            }
        }
        if (!found) {
            /* Shouldn't happen under normal operation. */
            log_msg("reaped unknown pid=%d status=0x%x", (int)pid, (unsigned)status);
        }
    }
}

/* Send SIGTERM to every running child. */
static void forward_sigterm(void)
{
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].pid > 0) {
            if (kill(g_children[i].pid, SIGTERM) < 0 && errno != ESRCH)
                log_msg("kill(%d, SIGTERM): %s", (int)g_children[i].pid, strerror(errno));
        }
    }
}

/* Return 1 if every child is either dead/disabled or not running. */
static int all_stopped(void)
{
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].pid > 0)
            return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Config parsing
 * ---------------------------------------------------------------------- */

/* Split a mutable line into tokens.  Tokens are stored as pointers into
 * heap-allocated copies (strdup) so the line buffer can be reused. */
static int parse_line(char *line, child_t *c)
{
    /* Strip trailing newline / carriage return. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    /* Skip blank lines and comments. */
    char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '#')
        return 0;

    memset(c, 0, sizeof *c);
    c->backoff = BACKOFF_INIT_SECS;

    char *token;
    char *rest = line;
    while ((token = strtok_r(rest, " \t", &rest)) != NULL) {
        if (c->argc >= MAX_ARGS) {
            fprintf(stderr, "supervisor: too many arguments (max %d), ignoring extras\n",
                    MAX_ARGS);
            break;
        }
        c->argv[c->argc] = strdup(token);
        if (!c->argv[c->argc]) {
            perror("strdup");
            /* Free what we've already allocated. */
            for (int j = 0; j < c->argc; j++) free(c->argv[j]);
            return -1;
        }
        c->argc++;
    }
    c->argv[c->argc] = NULL;
    return c->argc > 0 ? 1 : 0;
}

static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "supervisor: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[CFG_LINE_MAX];
    int  lineno = 0;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (g_nchildren >= MAX_CHILDREN) {
            fprintf(stderr, "supervisor: config: too many children (max %d)\n", MAX_CHILDREN);
            break;
        }
        int r = parse_line(line, &g_children[g_nchildren]);
        if (r < 0) {
            fclose(f);
            return -1;
        }
        if (r > 0)
            g_nchildren++;
    }

    if (ferror(f)) {
        fprintf(stderr, "supervisor: read %s: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * Main loop
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: supervisor <config-file>\n");
        return 1;
    }

    /* Load config before we do anything irreversible. */
    if (load_config(argv[1]) < 0)
        return 1;

    if (g_nchildren == 0) {
        fprintf(stderr, "supervisor: no children defined in %s\n", argv[1]);
        return 1;
    }

    /* Create self-pipe with O_NONBLOCK + O_CLOEXEC on both ends. */
    if (pipe(g_pipe) < 0) {
        perror("supervisor: pipe");
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(g_pipe[i], F_GETFL);
        if (flags < 0 || fcntl(g_pipe[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("supervisor: fcntl O_NONBLOCK");
            return 1;
        }
        flags = fcntl(g_pipe[i], F_GETFD);
        if (flags < 0 || fcntl(g_pipe[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            perror("supervisor: fcntl FD_CLOEXEC");
            return 1;
        }
    }

    /* Install signal handlers. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("supervisor: sigaction SIGCHLD");
        return 1;
    }
    sa.sa_handler = sigterm_handler;
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("supervisor: sigaction SIGTERM");
        return 1;
    }
    /* Also catch SIGINT for interactive use. */
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("supervisor: sigaction SIGINT");
        return 1;
    }

    log_msg("starting, managing %d child(ren)", g_nchildren);

    /* Initial spawn of all children. */
    for (int i = 0; i < g_nchildren; i++)
        spawn(i);

    /* ------------------------------------------------------------------ *
     * Main select loop                                                     *
     * ------------------------------------------------------------------ */
    while (1) {
        time_t now = time(NULL);

        /* If we're shutting down, forward SIGTERM and wait for children. */
        if (g_term) {
            static int forwarded = 0;
            if (!forwarded) {
                log_msg("SIGTERM received, forwarding to %d child(ren)", g_nchildren);
                forward_sigterm();
                forwarded = 1;
            }
            reap_all();
            if (all_stopped()) {
                log_msg("all children stopped, exiting");
                break;
            }
        } else {
            /* Spawn any child whose restart timer has elapsed. */
            for (int i = 0; i < g_nchildren; i++) {
                child_t *c = &g_children[i];
                if (c->pid == 0 && !c->dead && c->restart_at <= now)
                    spawn(i);
            }
        }

        /* Compute select(2) timeout: minimum time until the next scheduled
         * restart, or NULL (block indefinitely) if none are pending. */
        struct timeval  tv_buf;
        struct timeval *tv     = NULL;
        time_t          min_at = 0;

        if (!g_term) {
            for (int i = 0; i < g_nchildren; i++) {
                child_t *c = &g_children[i];
                if (c->pid == 0 && !c->dead && c->restart_at > 0) {
                    if (min_at == 0 || c->restart_at < min_at)
                        min_at = c->restart_at;
                }
            }
        } else {
            /* Poll rapidly while draining exiting children. */
            min_at = now + 1;
        }

        if (min_at != 0) {
            time_t delta = min_at - now;
            if (delta < 0) delta = 0;
            tv_buf.tv_sec  = delta;
            tv_buf.tv_usec = 0;
            tv = &tv_buf;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_pipe[0], &rfds);

        int r = select(g_pipe[0] + 1, &rfds, NULL, NULL, tv);
        if (r < 0) {
            if (errno == EINTR)
                continue; /* signal interrupted select; loop and re-evaluate */
            perror("supervisor: select");
            break;
        }

        if (r > 0 && FD_ISSET(g_pipe[0], &rfds)) {
            pipe_drain();
            reap_all();
        }
        /* If r == 0 (timeout), the loop will re-evaluate and spawn. */
    }

    /* Free config memory. */
    for (int i = 0; i < g_nchildren; i++)
        for (int j = 0; j < g_children[i].argc; j++)
            free(g_children[i].argv[j]);

    return 0;
}
