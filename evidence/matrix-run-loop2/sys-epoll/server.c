/*
 * server.c — non-blocking TCP server, C11, Linux, epoll edge-triggered
 *
 * Features:
 *   - Edge-triggered epoll (EPOLLET)
 *   - Per-connection growable read buffer
 *   - Line-delimited protocol (echoes each complete line back)
 *   - Partial write handling via per-connection output queue
 *   - Accept loop draining until EAGAIN
 *   - Idle timeout sweep (IDLE_TIMEOUT_SEC)
 *   - SIGINT / SIGTERM graceful shutdown
 *
 * Build:  cc -std=c11 -O2 -Wall -Wextra -o server server.c
 * Usage:  ./server <port>
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */
#define MAX_EVENTS        64
#define INITIAL_BUF_SIZE  4096
#define MAX_BUF_SIZE      (1 << 20)   /* 1 MiB hard cap per connection     */
#define IDLE_TIMEOUT_SEC  60          /* seconds before idle disconnect     */
#define TIMEOUT_INTERVAL  10          /* how often to sweep for idle conns  */
#define BACKLOG           128

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */
typedef struct conn {
    int      fd;
    time_t   last_active;

    /* read buffer — grows as needed */
    char    *rbuf;
    size_t   rbuf_len;   /* bytes currently in buffer */
    size_t   rbuf_cap;   /* allocated capacity        */

    /* write queue — holds data not yet flushed */
    char    *wbuf;
    size_t   wbuf_len;   /* bytes waiting to be sent  */
    size_t   wbuf_cap;
    size_t   wbuf_off;   /* offset of unsent data     */

    struct conn *next;   /* intrusive free-list / active-list */
} conn_t;

/* -------------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */
static volatile sig_atomic_t g_running = 1;
static conn_t *g_active_head = NULL;   /* singly-linked list of live conns */

/* -------------------------------------------------------------------------
 * Signal handler
 * ---------------------------------------------------------------------- */
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* -------------------------------------------------------------------------
 * File-descriptor helpers
 * ---------------------------------------------------------------------- */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* -------------------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------------- */
static conn_t *conn_new(int fd)
{
    conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->fd          = fd;
    c->last_active = time(NULL);

    c->rbuf_cap = INITIAL_BUF_SIZE;
    c->rbuf     = malloc(c->rbuf_cap);
    if (!c->rbuf) { free(c); return NULL; }

    c->wbuf_cap = INITIAL_BUF_SIZE;
    c->wbuf     = malloc(c->wbuf_cap);
    if (!c->wbuf) { free(c->rbuf); free(c); return NULL; }

    /* prepend to active list */
    c->next      = g_active_head;
    g_active_head = c;

    return c;
}

static void conn_remove_from_list(conn_t *target)
{
    conn_t **pp = &g_active_head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void conn_close(int epfd, conn_t *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_remove_from_list(c);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
}

/* -------------------------------------------------------------------------
 * Buffer growth
 * ---------------------------------------------------------------------- */
static int rbuf_ensure(conn_t *c, size_t needed)
{
    if (c->rbuf_cap - c->rbuf_len >= needed) return 0;
    size_t newcap = c->rbuf_cap;
    while (newcap - c->rbuf_len < needed) newcap *= 2;
    if (newcap > MAX_BUF_SIZE) return -1;
    char *p = realloc(c->rbuf, newcap);
    if (!p) return -1;
    c->rbuf     = p;
    c->rbuf_cap = newcap;
    return 0;
}

static int wbuf_append(conn_t *c, const char *data, size_t len)
{
    /* compact if there is wasted space at the front */
    if (c->wbuf_off > 0 && c->wbuf_len == 0) {
        c->wbuf_off = 0;
    }
    if (c->wbuf_off > 0 && c->wbuf_off > c->wbuf_cap / 2) {
        memmove(c->wbuf, c->wbuf + c->wbuf_off, c->wbuf_len);
        c->wbuf_off = 0;
    }

    size_t used = c->wbuf_off + c->wbuf_len;
    if (c->wbuf_cap - used < len) {
        size_t newcap = c->wbuf_cap;
        while (newcap - used < len) newcap *= 2;
        if (newcap > MAX_BUF_SIZE) return -1;
        char *p = realloc(c->wbuf, newcap);
        if (!p) return -1;
        c->wbuf     = p;
        c->wbuf_cap = newcap;
    }
    memcpy(c->wbuf + c->wbuf_off + c->wbuf_len, data, len);
    c->wbuf_len += len;
    return 0;
}

/* -------------------------------------------------------------------------
 * Write flusher — call whenever EPOLLOUT fires or after enqueuing data.
 * Returns -1 if the connection should be closed.
 * ---------------------------------------------------------------------- */
static int conn_flush(int epfd, conn_t *c)
{
    while (c->wbuf_len > 0) {
        ssize_t n = write(c->fd,
                          c->wbuf + c->wbuf_off,
                          c->wbuf_len);
        if (n > 0) {
            c->wbuf_off += (size_t)n;
            c->wbuf_len -= (size_t)n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* kernel buffer full — arm EPOLLOUT so we resume later */
            struct epoll_event ev = {
                .events   = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
                .data.ptr = c
            };
            epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
            return 0;
        } else {
            return -1;
        }
    }

    /* queue drained — stop watching for writability */
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLET | EPOLLRDHUP,
        .data.ptr = c
    };
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    return 0;
}

/* -------------------------------------------------------------------------
 * Line processor — called after each read burst.
 * Echoes every complete '\n'-terminated line back to the client.
 * Returns -1 if the connection should be closed.
 * ---------------------------------------------------------------------- */
static int conn_process_lines(int epfd, conn_t *c)
{
    while (1) {
        char *nl = memchr(c->rbuf, '\n', c->rbuf_len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - c->rbuf) + 1; /* include '\n' */

        if (wbuf_append(c, c->rbuf, line_len) != 0) return -1;

        /* consume processed bytes */
        size_t remaining = c->rbuf_len - line_len;
        memmove(c->rbuf, c->rbuf + line_len, remaining);
        c->rbuf_len = remaining;
    }

    return conn_flush(epfd, c);
}

/* -------------------------------------------------------------------------
 * Read handler — drain until EAGAIN (required for EPOLLET)
 * ---------------------------------------------------------------------- */
static int conn_do_read(int epfd, conn_t *c)
{
    while (1) {
        if (rbuf_ensure(c, 4096) != 0) return -1;

        ssize_t n = read(c->fd,
                         c->rbuf + c->rbuf_len,
                         c->rbuf_cap - c->rbuf_len);
        if (n > 0) {
            c->rbuf_len    += (size_t)n;
            c->last_active  = time(NULL);
        } else if (n == 0) {
            return -1;  /* peer closed */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
    }
    return conn_process_lines(epfd, c);
}

/* -------------------------------------------------------------------------
 * Accept loop — drain until EAGAIN (required for EPOLLET on listen fd)
 * ---------------------------------------------------------------------- */
static void accept_loop(int epfd, int listenfd)
{
    while (1) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept4(listenfd,
                         (struct sockaddr *)&peer, &plen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR)                          continue;
            perror("accept4");
            break;
        }

        /* TCP_NODELAY — lower latency for line-oriented traffic */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        conn_t *c = conn_new(fd);
        if (!c) {
            fprintf(stderr, "conn_new: out of memory\n");
            close(fd);
            continue;
        }

        struct epoll_event ev = {
            .events   = EPOLLIN | EPOLLET | EPOLLRDHUP,
            .data.ptr = c
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            perror("epoll_ctl ADD");
            conn_close(epfd, c);
        }
    }
}

/* -------------------------------------------------------------------------
 * Idle timeout sweep
 * ---------------------------------------------------------------------- */
static void sweep_idle(int epfd)
{
    time_t now     = time(NULL);
    conn_t *c      = g_active_head;
    while (c) {
        conn_t *next = c->next;
        if (now - c->last_active >= IDLE_TIMEOUT_SEC) {
            fprintf(stderr, "idle timeout fd=%d\n", c->fd);
            conn_close(epfd, c);
        }
        c = next;
    }
}

/* -------------------------------------------------------------------------
 * Listening socket
 * ---------------------------------------------------------------------- */
static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, BACKLOG) != 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
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

    /* signals */
    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int listenfd = make_listener(port);
    if (listenfd == -1) return 1;

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) { perror("epoll_create1"); close(listenfd); return 1; }

    /* watch the listen fd with EPOLLET so accept_loop drains properly */
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLET,
        .data.ptr = NULL   /* NULL == listen fd sentinel */
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) != 0) {
        perror("epoll_ctl listen"); return 1;
    }

    fprintf(stderr, "listening on port %d\n", port);

    struct epoll_event events[MAX_EVENTS];
    time_t last_sweep = time(NULL);

    while (g_running) {
        int timeout_ms = TIMEOUT_INTERVAL * 1000;
        int n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);

        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            uint32_t  evmask = events[i].events;
            conn_t   *c      = events[i].data.ptr;

            if (c == NULL) {
                /* listen fd */
                if (evmask & EPOLLIN) accept_loop(epfd, listenfd);
                continue;
            }

            /* peer half-close or error */
            if (evmask & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                conn_close(epfd, c);
                continue;
            }

            if (evmask & EPOLLOUT) {
                if (conn_flush(epfd, c) != 0) {
                    conn_close(epfd, c);
                    continue;
                }
            }

            if (evmask & EPOLLIN) {
                if (conn_do_read(epfd, c) != 0) {
                    conn_close(epfd, c);
                    continue;
                }
            }
        }

        /* idle sweep on each timeout interval */
        time_t now = time(NULL);
        if (now - last_sweep >= TIMEOUT_INTERVAL) {
            sweep_idle(epfd);
            last_sweep = now;
        }
    }

    /* graceful shutdown — close all connections */
    fprintf(stderr, "shutting down\n");
    while (g_active_head) conn_close(epfd, g_active_head);
    close(listenfd);
    close(epfd);
    return 0;
}
