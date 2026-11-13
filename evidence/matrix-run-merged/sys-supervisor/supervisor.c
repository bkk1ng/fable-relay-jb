/* supervisor.c — C11, Linux
 *
 * Starts N children from child_configs[], reaps via SIGCHLD self-pipe,
 * restarts crashed children with exponential backoff and a crash-loop
 * cutoff, forwards SIGTERM/SIGINT to all children, and logs exit status
 * and terminating signal for every child that dies.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -o supervisor supervisor.c
 * Run:    ./supervisor
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Config — edit child_configs[] to taste                              */
/* ------------------------------------------------------------------ */

#define MAX_CHILDREN  64
#define MAX_ARGS      32

/* Each row is a NULL-terminated argv for one child process. */
static char *child_configs[][MAX_ARGS + 1] = {
    { "/bin/sleep", "5",  NULL },
    { "/bin/sleep", "10", NULL },
};
#define N_CHILDREN ((int)(sizeof child_configs / sizeof child_configs[0]))

/* Restart policy */
#define MAX_RESTARTS  5   /* give up after this many consecutive crashes  */
#define BACKOFF_BASE  1   /* initial backoff seconds                       */
#define BACKOFF_CAP   60  /* maximum backoff seconds                       */
#define GOOD_RUN_SEC  10  /* uptime that resets the consecutive crash count*/

/* ------------------------------------------------------------------ */
/* Child state                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *argv[MAX_ARGS + 1];  /* null-terminated argv                   */
    pid_t  pid;                 /* 0 = not running                        */
    int    crashes;             /* consecutive crash count                 */
    int    backoff;             /* delay before next restart (seconds)    */
    time_t started_at;          /* wall clock when last exec'd             */
    time_t restart_at;          /* 0 = not scheduled; else epoch of restart*/
    int    abandoned;           /* crash-loop cutoff reached              */
} Child;

static Child children[MAX_CHILDREN];
static int   n_children;

/* ------------------------------------------------------------------ */
/* Self-pipe and signal flags                                           */
/* ------------------------------------------------------------------ */

static int pipe_fd[2];
static volatile sig_atomic_t terminating;

static void sigchld_handler(int sig)
{
    (void)sig;
    /* One byte wakes the main loop; O_NONBLOCK prevents any blocking. */
    char b = 'c';
    while (write(pipe_fd[1], &b, 1) == -1 && errno == EINTR)
        ;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    terminating = 1;
    char b = 't';
    while (write(pipe_fd[1], &b, 1) == -1 && errno == EINTR)
        ;
}

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

/* All logging goes to stderr.  Called only from the main loop, never
   from a signal handler, so fprintf is fine. */
static void log_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void log_child_exit(int idx, pid_t pid, int status)
{
    char buf[256];
    if (WIFEXITED(status)) {
        snprintf(buf, sizeof buf,
                 "[supervisor] child[%d] pid=%d exited status=%d",
                 idx, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(buf, sizeof buf,
                 "[supervisor] child[%d] pid=%d killed by signal %d (%s)%s",
                 idx, (int)pid, WTERMSIG(status),
                 strsignal(WTERMSIG(status)),
                 WCOREDUMP(status) ? " (core dumped)" : "");
    } else {
        snprintf(buf, sizeof buf,
                 "[supervisor] child[%d] pid=%d stopped/continued status=0x%x",
                 idx, (int)pid, (unsigned)status);
    }
    log_msg(buf);
}

/* ------------------------------------------------------------------ */
/* Child management                                                     */
/* ------------------------------------------------------------------ */

static void start_child(int idx)
{
    Child *c = &children[idx];
    if (c->abandoned) return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("[supervisor] fork");
        return;
    }
    if (pid == 0) {
        /* Restore default dispositions in the child. */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        execvp(c->argv[0], c->argv);
        perror(c->argv[0]);
        _exit(127);
    }

    c->pid        = pid;
    c->started_at = time(NULL);
    c->restart_at = 0;

    char buf[256];
    snprintf(buf, sizeof buf,
             "[supervisor] child[%d] pid=%d started: %s",
             idx, (int)pid, c->argv[0]);
    log_msg(buf);
}

/* Returns 1 if status represents a crash (non-zero exit or signal). */
static int is_crash(int status)
{
    if (WIFSIGNALED(status)) return 1;
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) return 1;
    return 0;
}

static void schedule_restart(int idx)
{
    Child *c = &children[idx];
    char  buf[256];

    /* A run long enough to be considered healthy resets the crash counter. */
    time_t ran_for = time(NULL) - c->started_at;
    if (ran_for >= GOOD_RUN_SEC) {
        c->crashes = 0;
        c->backoff  = BACKOFF_BASE;
    }

    c->crashes++;
    if (c->crashes > MAX_RESTARTS) {
        c->abandoned = 1;
        snprintf(buf, sizeof buf,
                 "[supervisor] child[%d] abandoned after %d consecutive crashes",
                 idx, MAX_RESTARTS);
        log_msg(buf);
        return;
    }

    int delay  = c->backoff;
    c->backoff = (c->backoff * 2 <= BACKOFF_CAP) ? c->backoff * 2 : BACKOFF_CAP;

    c->restart_at = time(NULL) + delay;
    snprintf(buf, sizeof buf,
             "[supervisor] child[%d] will restart in %ds (crash #%d/%d)",
             idx, delay, c->crashes, MAX_RESTARTS);
    log_msg(buf);
}

/* Non-blocking pass: reap every dead child. */
static void reap_children(void)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int found = 0;
        for (int i = 0; i < n_children; i++) {
            if (children[i].pid != pid) continue;
            log_child_exit(i, pid, status);
            children[i].pid = 0;
            if (!terminating && is_crash(status))
                schedule_restart(i);
            found = 1;
            break;
        }
        if (!found) {
            char buf[128];
            snprintf(buf, sizeof buf,
                     "[supervisor] reaped unknown pid=%d status=0x%x",
                     (int)pid, (unsigned)status);
            log_msg(buf);
        }
    }
}

/* Send sig to every live child. */
static void forward_signal(int sig)
{
    char buf[128];
    snprintf(buf, sizeof buf,
             "[supervisor] forwarding signal %d (%s) to all children",
             sig, strsignal(sig));
    log_msg(buf);
    for (int i = 0; i < n_children; i++) {
        if (children[i].pid > 0)
            kill(children[i].pid, sig);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* --- self-pipe ------------------------------------------------- */
    if (pipe(pipe_fd) == -1) { perror("pipe"); return 1; }

    /* Both ends non-blocking: writes from the handler never stall,
       and draining reads in the loop return EAGAIN when empty. */
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(pipe_fd[i], F_GETFL);
        if (fl < 0 || fcntl(pipe_fd[i], F_SETFL, fl | O_NONBLOCK) < 0) {
            perror("fcntl O_NONBLOCK");
            return 1;
        }
    }

    /* --- signal handlers ------------------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) { perror("sigaction SIGCHLD"); return 1; }

    sa.sa_handler = sigterm_handler;
    if (sigaction(SIGTERM, &sa, NULL) < 0) { perror("sigaction SIGTERM"); return 1; }
    if (sigaction(SIGINT,  &sa, NULL) < 0) { perror("sigaction SIGINT");  return 1; }

    /* --- build child table ----------------------------------------- */
    n_children = N_CHILDREN < MAX_CHILDREN ? N_CHILDREN : MAX_CHILDREN;
    for (int i = 0; i < n_children; i++) {
        Child *c = &children[i];
        memset(c, 0, sizeof *c);
        for (int j = 0; j <= MAX_ARGS; j++) {
            c->argv[j] = child_configs[i][j];
            if (!c->argv[j]) break;
        }
        c->backoff = BACKOFF_BASE;
    }

    /* --- initial launch -------------------------------------------- */
    log_msg("[supervisor] starting");
    for (int i = 0; i < n_children; i++)
        start_child(i);

    /* --- event loop ------------------------------------------------- */
    for (;;) {
        /*
         * Compute the timeout: the nearest scheduled restart, if any.
         * With no pending restarts we block indefinitely on the pipe.
         */
        struct timeval  tv;
        struct timeval *tvp = NULL;
        time_t now = time(NULL);

        for (int i = 0; i < n_children; i++) {
            if (!children[i].restart_at) continue;
            time_t diff = children[i].restart_at - now;
            if (diff < 0) diff = 0;
            if (tvp == NULL || diff < (time_t)tv.tv_sec) {
                tv.tv_sec  = diff;
                tv.tv_usec = 0;
                tvp = &tv;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_fd[0], &rfds);

        int r = select(pipe_fd[0] + 1, &rfds, NULL, NULL, tvp);
        if (r < 0 && errno != EINTR) { perror("select"); break; }

        /* Drain the pipe fully before processing — a burst of SIGCHLD
           signals may have produced multiple bytes. */
        {
            char drain[64];
            while (read(pipe_fd[0], drain, sizeof drain) > 0)
                ;
        }

        /* Handle shutdown request (SIGTERM or SIGINT). */
        if (terminating) {
            forward_signal(SIGTERM);
            /* Blocking reap; log every exit so the caller has a record. */
            int   wstatus;
            pid_t wpid;
            while ((wpid = waitpid(-1, &wstatus, 0)) > 0) {
                for (int i = 0; i < n_children; i++) {
                    if (children[i].pid == wpid) {
                        log_child_exit(i, wpid, wstatus);
                        children[i].pid = 0;
                        break;
                    }
                }
            }
            log_msg("[supervisor] exiting");
            return 0;
        }

        /* Reap any children that have exited. */
        reap_children();

        /* Fire scheduled restarts whose deadline has passed. */
        now = time(NULL);
        for (int i = 0; i < n_children; i++) {
            Child *c = &children[i];
            if (c->pid == 0 && c->restart_at > 0 &&
                now >= c->restart_at && !c->abandoned)
            {
                start_child(i);
            }
        }
    }

    return 1;
}
