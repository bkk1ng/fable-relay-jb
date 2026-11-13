/*
 * server.c — single-threaded edge-triggered epoll TCP server
 *
 * C11 / Linux
 * Features: non-blocking accept loop, ET epoll, per-connection growable
 *           buffers, line-delimited protocol (echo), partial-write output
 *           queue, idle timeout sweep via timerfd, SIGINT/SIGTERM shutdown
 *           via signalfd.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O2 -o server server.c
 * Run:
 *   ./server 8080
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── tunables ───────────────────────────────────────────────────────────── */
#define MAX_EVENTS       64
#define INIT_BUF_SIZE    4096u
#define IDLE_TIMEOUT_S   60          /* close connection after N idle seconds */
#define SWEEP_INTERVAL_S 10          /* idle-sweep timer period               */
#define FD_TABLE_SIZE    65536       /* fd→conn map; sized to max fd          */

/* ── per-connection state ───────────────────────────────────────────────── */
typedef struct conn {
    int    fd;
    time_t last_active;

    /* input: flat growable buffer */
    char  *ibuf;
    size_t ilen;        /* bytes consumed */
    size_t icap;        /* allocated capacity */

    /* output: growable buffer + drain cursor */
    char  *obuf;
    size_t ooff;        /* index of next byte to write */
    size_t olen;        /* bytes remaining from ooff   */
    size_t ocap;        /* allocated capacity           */
} conn_t;

/* ── globals ────────────────────────────────────────────────────────────── */
static conn_t *fd_table[FD_TABLE_SIZE];
static int     epfd        = -1;
static int     srvfd       = -1;
static int     sigfd       = -1;
static int     tfd         = -1;   /* timerfd for idle sweep */
static bool    running     = true;

/*
 * We store three special fds in epoll alongside conn_t pointers.
 * Use small sentinel values that malloc will never return.
 */
#define TAG_SERVER ((void *)(uintptr_t)1)
#define TAG_SIGNAL ((void *)(uintptr_t)2)
#define TAG_TIMER  ((void *)(uintptr_t)3)

/* ── epoll helpers ──────────────────────────────────────────────────────── */
static void ep_add(int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        perror("epoll_ctl ADD");
}

static void ep_mod(int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0)
        perror("epoll_ctl MOD");
}

/* ── connection lifecycle ───────────────────────────────────────────────── */
static conn_t *conn_new(int fd)
{
    conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd          = fd;
    c->last_active = time(NULL);
    c->ibuf        = malloc(INIT_BUF_SIZE);
    c->icap        = INIT_BUF_SIZE;
    c->obuf        = malloc(INIT_BUF_SIZE);
    c->ocap        = INIT_BUF_SIZE;
    if (!c->ibuf || !c->obuf) {
        free(c->ibuf); free(c->obuf); free(c);
        return NULL;
    }
    return c;
}

static void conn_close(conn_t *c)
{
    printf("close   fd=%d\n", c->fd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    fd_table[c->fd] = NULL;
    free(c->ibuf);
    free(c->obuf);
    free(c);
}

/* ── output queue ───────────────────────────────────────────────────────── */

/*
 * Slide pending bytes back to the front of the buffer.
 * Called before every append so the capacity check is accurate.
 */
static void obuf_compact(conn_t *c)
{
    if (c->ooff == 0 || c->olen == 0) {
        c->ooff = 0;
        return;
    }
    memmove(c->obuf, c->obuf + c->ooff, c->olen);
    c->ooff = 0;
}

static bool obuf_append(conn_t *c, const char *data, size_t len)
{
    obuf_compact(c);
    size_t need = c->olen + len;
    if (need > c->ocap) {
        size_t newcap = c->ocap;
        while (newcap < need) newcap *= 2;
        char *p = realloc(c->obuf, newcap);
        if (!p) return false;
        c->obuf = p;
        c->ocap = newcap;
    }
    memcpy(c->obuf + c->olen, data, len);
    c->olen += len;
    return true;
}

/*
 * Drain the output buffer until EAGAIN or empty.
 * Returns true when the buffer is fully drained.
 */
static bool obuf_flush(conn_t *c)
{
    while (c->olen > 0) {
        ssize_t n = write(c->fd, c->obuf + c->ooff, c->olen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;   /* kernel buffer full; wait for EPOLLOUT */
            return true;        /* I/O error; caller should close */
        }
        if (n == 0) return true;
        c->ooff += (size_t)n;
        c->olen -= (size_t)n;
    }
    obuf_compact(c);
    return true;
}

/*
 * Queue data for delivery.
 * When the buffer was previously empty, attempt a direct flush and arm
 * EPOLLOUT only if the kernel buffer fills up.
 */
static void conn_send(conn_t *c, const char *data, size_t len)
{
    bool was_empty = (c->olen == 0);

    if (!obuf_append(c, data, len)) {
        conn_close(c);
        return;
    }

    if (was_empty) {
        if (!obuf_flush(c)) {
            /* Partial write: arm EPOLLOUT to be notified when space frees up */
            ep_mod(c->fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP, c);
        }
        /* Fully drained: EPOLLOUT stays disarmed, nothing to do */
    }
    /*
     * If was_empty == false, EPOLLOUT is already armed from a previous
     * partial write; the new data will be flushed when that fires.
     */
}

/* ── line-delimited protocol ────────────────────────────────────────────── */
/*
 * Extract and process every complete '\n'-terminated line from the input
 * buffer.  Protocol: echo each line back to the sender.
 */
static void process_lines(conn_t *c)
{
    while (c->ilen > 0) {
        char *nl = memchr(c->ibuf, '\n', c->ilen);
        if (!nl) break;

        size_t line_len = (size_t)(nl - c->ibuf) + 1; /* include '\n' */
        conn_send(c, c->ibuf, line_len);

        /* Consume the line */
        size_t remaining = c->ilen - line_len;
        memmove(c->ibuf, c->ibuf + line_len, remaining);
        c->ilen = remaining;
    }
}

/* ── read handler ───────────────────────────────────────────────────────── */
/*
 * Edge-triggered: drain until EAGAIN before returning to epoll_wait.
 */
static void conn_do_read(conn_t *c)
{
    for (;;) {
        /* Grow input buffer when it is full */
        if (c->ilen == c->icap) {
            size_t newcap = c->icap * 2;
            char  *p      = realloc(c->ibuf, newcap);
            if (!p) { conn_close(c); return; }
            c->ibuf = p;
            c->icap = newcap;
        }

        ssize_t n = read(c->fd, c->ibuf + c->ilen, c->icap - c->ilen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* fully drained */
            conn_close(c);
            return;
        }
        if (n == 0) {       /* peer closed connection */
            conn_close(c);
            return;
        }
        c->ilen        += (size_t)n;
        c->last_active  = time(NULL);
    }
    process_lines(c);
}

/* ── write handler ──────────────────────────────────────────────────────── */
static void conn_do_write(conn_t *c)
{
    if (obuf_flush(c))
        ep_mod(c->fd, EPOLLIN | EPOLLET | EPOLLRDHUP, c); /* disarm EPOLLOUT */
}

/* ── accept loop ────────────────────────────────────────────────────────── */
/*
 * Edge-triggered on the server socket: drain accept() until EAGAIN.
 */
static void do_accept(void)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof addr;
        int cfd = accept4(srvfd, (struct sockaddr *)&addr, &addrlen,
                          SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR)                          continue;
            perror("accept4");
            break;
        }
        if (cfd >= FD_TABLE_SIZE) {
            fprintf(stderr, "fd %d exceeds FD_TABLE_SIZE — closing\n", cfd);
            close(cfd);
            continue;
        }

        conn_t *c = conn_new(cfd);
        if (!c) { close(cfd); continue; }

        fd_table[cfd] = c;
        ep_add(cfd, EPOLLIN | EPOLLET | EPOLLRDHUP, c);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
        printf("accept  fd=%d  %s:%u\n", cfd, ip, ntohs(addr.sin_port));
    }
}

/* ── idle timeout sweep ─────────────────────────────────────────────────── */
static void do_idle_sweep(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        conn_t *c = fd_table[i];
        if (!c) continue;
        if (now - c->last_active >= IDLE_TIMEOUT_S) {
            printf("timeout fd=%d  idle=%lds\n",
                   c->fd, (long)(now - c->last_active));
            conn_close(c);
        }
    }
}

/* ── cleanup ────────────────────────────────────────────────────────────── */
static void cleanup_all(void)
{
    for (int i = 0; i < FD_TABLE_SIZE; i++)
        if (fd_table[i]) conn_close(fd_table[i]);
    if (srvfd >= 0) close(srvfd);
    if (sigfd >= 0) close(sigfd);
    if (tfd   >= 0) close(tfd);
    if (epfd  >= 0) close(epfd);
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

    /* ── server socket ── */
    srvfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (srvfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons((uint16_t)port),
    };
    if (bind(srvfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srvfd, SOMAXCONN) < 0) {
        perror("listen"); return 1;
    }

    /* ── epoll instance ── */
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    ep_add(srvfd, EPOLLIN | EPOLLET, TAG_SERVER);

    /* ── signalfd: catch SIGINT and SIGTERM cleanly ── */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask"); return 1;
    }
    sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) { perror("signalfd"); return 1; }
    ep_add(sigfd, EPOLLIN, TAG_SIGNAL);

    /* ── timerfd: periodic idle sweep ── */
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) { perror("timerfd_create"); return 1; }
    struct itimerspec its = {
        .it_value    = { .tv_sec = SWEEP_INTERVAL_S },
        .it_interval = { .tv_sec = SWEEP_INTERVAL_S },
    };
    timerfd_settime(tfd, 0, &its, NULL);
    ep_add(tfd, EPOLLIN, TAG_TIMER);

    printf("listening on :%d  (idle timeout %ds, sweep every %ds)\n",
           port, IDLE_TIMEOUT_S, SWEEP_INTERVAL_S);

    /* ── event loop ── */
    struct epoll_event events[MAX_EVENTS];
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            void    *ptr     = events[i].data.ptr;
            uint32_t evflags = events[i].events;

            if (ptr == TAG_SERVER) {
                do_accept();

            } else if (ptr == TAG_SIGNAL) {
                struct signalfd_siginfo si;
                read(sigfd, &si, sizeof si);
                printf("\ncaught signal %u — shutting down\n", si.ssi_signo);
                running = false;
                break;

            } else if (ptr == TAG_TIMER) {
                /* Drain the timerfd counter before sweeping */
                uint64_t expirations;
                read(tfd, &expirations, sizeof expirations);
                do_idle_sweep();

            } else {
                conn_t *c      = ptr;
                int     saved_fd = c->fd;   /* save before any close */

                if (evflags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    conn_close(c);
                    continue;
                }
                if (evflags & EPOLLIN)
                    conn_do_read(c);
                /*
                 * Re-check the table: conn_do_read may have closed the
                 * connection (EOF or error), so only call write if the
                 * conn_t is still live.
                 */
                if (fd_table[saved_fd] == c && (evflags & EPOLLOUT))
                    conn_do_write(c);
            }
        }
    }

    cleanup_all();
    printf("done.\n");
    return 0;
}
