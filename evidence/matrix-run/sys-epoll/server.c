/*
 * server.c — single-threaded epoll (edge-triggered) TCP server, C11, Linux
 *
 * Protocol  : line-delimited; server echoes each line back.
 * Usage     : ./server <port>
 * Build     : gcc -std=c11 -O2 -Wall -Wextra -o server server.c
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* ── tunables ──────────────────────────────────────────────── */

#define MAX_EVENTS      64
#define MAX_CONNS       65536       /* fd-indexed table ceiling  */
#define IBUF_INIT       4096        /* input buffer initial size */
#define OBUF_INIT       4096        /* output buffer initial size */
#define IDLE_TIMEOUT_S  60          /* close after N idle seconds */
#define SWEEP_INTERVAL_S 10         /* idle-sweep timer period    */
#define LISTEN_BACKLOG  128

/* ── per-connection state ──────────────────────────────────── */

typedef struct {
    int     fd;
    time_t  last_active;

    /* input (read-side) growable buffer */
    char   *ibuf;
    size_t  ibuf_len;   /* bytes stored */
    size_t  ibuf_cap;   /* allocated capacity */

    /* output queue; pending bytes = [obuf_off, obuf_len) */
    char   *obuf;
    size_t  obuf_len;   /* bytes appended */
    size_t  obuf_off;   /* bytes already written */
    size_t  obuf_cap;   /* allocated capacity */
} conn_t;

/* ── globals ───────────────────────────────────────────────── */

static conn_t  *conns[MAX_CONNS];   /* NULL if slot unused */
static int      epfd    = -1;
static int      running = 1;

/* ── epoll arm/rearm helpers ───────────────────────────────── */

static void arm_rd(int fd)
{
    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLET | EPOLLRDHUP,
        .data.fd = fd
    };
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void arm_rdwr(int fd)
{
    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
        .data.fd = fd
    };
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/* ── connection lifecycle ──────────────────────────────────── */

static conn_t *conn_alloc(int fd)
{
    if (fd < 0 || fd >= MAX_CONNS) return NULL;

    conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    c->ibuf = malloc(IBUF_INIT);
    c->obuf = malloc(OBUF_INIT);
    if (!c->ibuf || !c->obuf) {
        free(c->ibuf);
        free(c->obuf);
        free(c);
        return NULL;
    }

    c->fd          = fd;
    c->last_active = time(NULL);
    c->ibuf_cap    = IBUF_INIT;
    c->obuf_cap    = OBUF_INIT;

    conns[fd] = c;
    return c;
}

static void conn_free(conn_t *c)
{
    if (!c) return;

    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);

    if (c->fd >= 0 && c->fd < MAX_CONNS)
        conns[c->fd] = NULL;

    free(c->ibuf);
    free(c->obuf);
    free(c);
}

/* ── input buffer ──────────────────────────────────────────── */

/* Ensure there is room for at least `need` more bytes. */
static int ibuf_reserve(conn_t *c, size_t need)
{
    if (c->ibuf_len + need <= c->ibuf_cap) return 0;

    size_t newcap = c->ibuf_cap;
    while (newcap < c->ibuf_len + need) newcap *= 2;

    char *p = realloc(c->ibuf, newcap);
    if (!p) return -1;
    c->ibuf     = p;
    c->ibuf_cap = newcap;
    return 0;
}

/* ── output queue ──────────────────────────────────────────── */

/*
 * Append `len` bytes to the output queue.
 * Compacts the buffer first when the drained prefix exceeds half the capacity.
 */
static int obuf_append(conn_t *c, const char *data, size_t len)
{
    /* compact: slide unsent bytes to the front */
    if (c->obuf_off > 0 && c->obuf_off >= c->obuf_cap / 2) {
        size_t pending = c->obuf_len - c->obuf_off;
        memmove(c->obuf, c->obuf + c->obuf_off, pending);
        c->obuf_len = pending;
        c->obuf_off = 0;
    }

    if (c->obuf_len + len > c->obuf_cap) {
        size_t newcap = c->obuf_cap;
        while (newcap < c->obuf_len + len) newcap *= 2;
        char *p = realloc(c->obuf, newcap);
        if (!p) return -1;
        c->obuf     = p;
        c->obuf_cap = newcap;
    }

    memcpy(c->obuf + c->obuf_len, data, len);
    c->obuf_len += len;
    return 0;
}

/*
 * Flush as much of the output queue as the kernel will accept.
 * Arms EPOLLOUT if write() returns EAGAIN (partial write).
 * Disarms EPOLLOUT once the queue empties.
 * Returns -1 on unrecoverable write error; 0 otherwise.
 */
static int conn_flush(conn_t *c)
{
    while (c->obuf_off < c->obuf_len) {
        ssize_t n = write(c->fd,
                          c->obuf + c->obuf_off,
                          c->obuf_len - c->obuf_off);
        if (n < 0) {
            if (errno == EINTR)                        continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                arm_rdwr(c->fd);   /* wake us when writable again */
                return 0;
            }
            return -1;
        }
        c->obuf_off += (size_t)n;
    }

    /* queue fully drained — no need for EPOLLOUT anymore */
    c->obuf_len = 0;
    c->obuf_off = 0;
    arm_rd(c->fd);
    return 0;
}

/* ── protocol ──────────────────────────────────────────────── */

/*
 * Called with one complete, NUL-terminated line (CR stripped).
 * Default behaviour: echo the line back with a trailing newline.
 * Returns -1 if the connection should be torn down.
 */
static int handle_line(conn_t *c, const char *line, size_t len)
{
    if (obuf_append(c, line, len) < 0) return -1;
    if (obuf_append(c, "\n",  1 ) < 0) return -1;
    return conn_flush(c);
}

/*
 * Scan ibuf for complete lines and dispatch them.
 * Returns -1 if the connection should be torn down.
 */
static int process_input(conn_t *c)
{
    while (c->ibuf_len > 0) {
        char *nl = memchr(c->ibuf, '\n', c->ibuf_len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - c->ibuf);
        if (line_len > 0 && c->ibuf[line_len - 1] == '\r')
            line_len--;

        /* Temporarily NUL-terminate for handle_line */
        char saved        = c->ibuf[line_len];
        c->ibuf[line_len] = '\0';

        int rc = handle_line(c, c->ibuf, line_len);

        c->ibuf[line_len] = saved;
        if (rc < 0) return -1;

        size_t consumed = (size_t)(nl - c->ibuf) + 1;
        memmove(c->ibuf, c->ibuf + consumed, c->ibuf_len - consumed);
        c->ibuf_len -= consumed;
    }
    return 0;
}

/* ── read path ─────────────────────────────────────────────── */

/*
 * Edge-triggered: drain the socket completely until EAGAIN.
 * Returns -1 when the connection should be torn down.
 */
static int conn_read(conn_t *c)
{
    for (;;) {
        if (ibuf_reserve(c, 4096) < 0) return -1;

        ssize_t n = read(c->fd,
                         c->ibuf + c->ibuf_len,
                         c->ibuf_cap - c->ibuf_len);
        if (n < 0) {
            if (errno == EINTR)                        continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  /* drained */
            return -1;
        }
        if (n == 0) return -1;  /* clean peer close */

        c->ibuf_len   += (size_t)n;
        c->last_active = time(NULL);

        if (process_input(c) < 0) return -1;
    }
    return 0;
}

/* ── accept loop ───────────────────────────────────────────── */

/*
 * Edge-triggered listen socket: accept all pending connections
 * before returning to epoll_wait.
 */
static void drain_accept(int listenfd)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof peer;

        int fd = accept4(listenfd,
                         (struct sockaddr *)&peer, &peerlen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)                        continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept4");
            break;
        }

        if (fd >= MAX_CONNS) {
            fprintf(stderr, "fd %d exceeds MAX_CONNS — dropping\n", fd);
            close(fd);
            continue;
        }

        conn_t *c = conn_alloc(fd);
        if (!c) {
            close(fd);
            continue;
        }

        struct epoll_event ev = {
            .events  = EPOLLIN | EPOLLET | EPOLLRDHUP,
            .data.fd = fd
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl ADD");
            conn_free(c);
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        fprintf(stderr, "connect  fd=%-5d %s:%d\n",
                fd, ip, ntohs(peer.sin_port));
    }
}

/* ── idle timeout sweep ────────────────────────────────────── */

static void idle_sweep(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CONNS; i++) {
        conn_t *c = conns[i];
        if (!c) continue;
        if (now - c->last_active > IDLE_TIMEOUT_S) {
            fprintf(stderr, "idle     fd=%-5d (timeout)\n", c->fd);
            conn_free(c);
        }
    }
}

/* ── main ──────────────────────────────────────────────────── */

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

    /* ── signalfd for SIGINT / SIGTERM ── */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }
    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) { perror("signalfd"); return 1; }

    /* ── timerfd for idle sweep ── */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) { perror("timerfd_create"); return 1; }
    struct itimerspec its = {
        .it_interval = { .tv_sec = SWEEP_INTERVAL_S, .tv_nsec = 0 },
        .it_value    = { .tv_sec = SWEEP_INTERVAL_S, .tv_nsec = 0 }
    };
    timerfd_settime(tfd, 0, &its, NULL);

    /* ── listening socket ── */
    int listenfd = socket(AF_INET,
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0);
    if (listenfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons((uint16_t)port)
    };
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listenfd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    /* ── epoll instance ── */
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;

    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listenfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    ev.events  = EPOLLIN;
    ev.data.fd = sigfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ev);

    ev.events  = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

    fprintf(stderr, "listening on port %d  (idle=%ds sweep=%ds)\n",
            port, IDLE_TIMEOUT_S, SWEEP_INTERVAL_S);

    /* ── event loop ── */
    struct epoll_event events[MAX_EVENTS];

    while (running) {
        int nev = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nev < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nev; i++) {
            int      fd      = events[i].data.fd;
            uint32_t evmask  = events[i].events;

            /* ── signal ── */
            if (fd == sigfd) {
                struct signalfd_siginfo si;
                read(sigfd, &si, sizeof si);
                fprintf(stderr, "signal %u — shutting down\n", si.ssi_signo);
                running = 0;
                break;
            }

            /* ── idle sweep timer ── */
            if (fd == tfd) {
                uint64_t expirations;
                read(tfd, &expirations, sizeof expirations);
                idle_sweep();
                continue;
            }

            /* ── new connection ── */
            if (fd == listenfd) {
                drain_accept(listenfd);
                continue;
            }

            /* ── existing connection ── */
            conn_t *c = (fd >= 0 && fd < MAX_CONNS) ? conns[fd] : NULL;
            if (!c) continue;

            if (evmask & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                conn_free(c);
                continue;
            }

            /* Flush output queue first so we free buffer space before reading. */
            if (evmask & EPOLLOUT) {
                if (conn_flush(c) < 0) {
                    conn_free(c);
                    continue;
                }
            }

            if (evmask & EPOLLIN) {
                if (conn_read(c) < 0) {
                    conn_free(c);
                    continue;
                }
            }
        }
    }

    /* ── graceful shutdown ── */
    fprintf(stderr, "closing %d connection(s)\n",
            (int)(sizeof conns / sizeof *conns));  /* upper bound message */

    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i]) conn_free(conns[i]);
    }

    close(listenfd);
    close(sigfd);
    close(tfd);
    close(epfd);

    fprintf(stderr, "shutdown complete\n");
    return 0;
}
