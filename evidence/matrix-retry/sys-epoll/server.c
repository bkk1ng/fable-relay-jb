/*
 * server.c — non-blocking TCP server
 *
 * C11, Linux, epoll edge-triggered, single-threaded event loop.
 * Per-connection growable read buffer, line-delimited protocol,
 * partial write handling with an output queue, accept loop draining
 * EAGAIN, idle timeout sweep, SIGINT shutdown.
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o server server.c
 *
 * Run:
 *   ./server <port>
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define MAX_EVENTS       64
#define IDLE_TIMEOUT_SEC 60
#define SWEEP_INTERVAL   10   /* seconds between idle sweeps          */
#define RBUF_INIT        4096
#define RBUF_MAX         (1 << 20)  /* 1 MiB read buffer ceiling      */
#define WBUF_INIT        4096

/* ------------------------------------------------------------------ */
/* Dynamic buffer                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;   /* bytes of live data                               */
    size_t cap;   /* allocated capacity                               */
} Buf;

static int buf_init(Buf *b, size_t cap) {
    b->data = malloc(cap);
    if (!b->data) return -1;
    b->len = 0;
    b->cap = cap;
    return 0;
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* Ensure at least `need` additional bytes can be appended. */
static int buf_ensure(Buf *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t ncap = b->cap;
    while (ncap < b->len + need) ncap *= 2;
    if (ncap > RBUF_MAX) return -1;
    char *nd = realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap  = ncap;
    return 0;
}

/* Consume `n` bytes from the front of the buffer. */
static void buf_consume(Buf *b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

/* Append bytes to buffer. */
static int buf_append(Buf *b, const char *src, size_t n) {
    if (buf_ensure(b, n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output chunk queue (linked list of pending writes)                  */
/* ------------------------------------------------------------------ */

typedef struct Chunk Chunk;
struct Chunk {
    char   *data;
    size_t  len;
    size_t  sent;  /* bytes already written from this chunk           */
    Chunk  *next;
};

typedef struct {
    Chunk *head;
    Chunk *tail;
} WriteQueue;

static int wq_push(WriteQueue *q, const char *data, size_t len) {
    Chunk *c = malloc(sizeof *c);
    if (!c) return -1;
    c->data = malloc(len);
    if (!c->data) { free(c); return -1; }
    memcpy(c->data, data, len);
    c->len  = len;
    c->sent = 0;
    c->next = NULL;
    if (q->tail) q->tail->next = c; else q->head = c;
    q->tail = c;
    return 0;
}

/* Drain as much of the queue as the socket will accept right now.
   Returns 0 on progress/done, -1 on hard error (caller should close). */
static int wq_flush(WriteQueue *q, int fd) {
    while (q->head) {
        Chunk *c = q->head;
        while (c->sent < c->len) {
            ssize_t n = write(fd, c->data + c->sent, c->len - c->sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;  /* real error                            */
            }
            c->sent += (size_t)n;
        }
        q->head = c->next;
        if (!q->head) q->tail = NULL;
        free(c->data);
        free(c);
    }
    return 0;
}

static void wq_free(WriteQueue *q) {
    Chunk *c = q->head;
    while (c) {
        Chunk *nx = c->next;
        free(c->data);
        free(c);
        c = nx;
    }
    q->head = q->tail = NULL;
}

static int wq_empty(const WriteQueue *q) { return q->head == NULL; }

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int        fd;
    Buf        rbuf;
    WriteQueue wq;
    time_t     last_active;
    int        closing;    /* half-close: drain wq then close         */
} Conn;

#define MAX_CONNS 65536

static Conn *conn_table[MAX_CONNS]; /* indexed by fd                  */

static Conn *conn_new(int fd) {
    if (fd < 0 || fd >= MAX_CONNS) return NULL;
    Conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    if (buf_init(&c->rbuf, RBUF_INIT) < 0) { free(c); return NULL; }
    c->fd          = fd;
    c->last_active = time(NULL);
    conn_table[fd] = c;
    return c;
}

static void conn_free(Conn *c) {
    if (!c) return;
    conn_table[c->fd] = NULL;
    close(c->fd);
    buf_free(&c->rbuf);
    wq_free(&c->wq);
    free(c);
}

/* Queue a reply; if write queue was empty try to send immediately. */
static void conn_send(Conn *c, int epfd, const char *msg, size_t len) {
    if (c->closing) return;

    if (wq_empty(&c->wq)) {
        /* Optimistic direct write first. */
        ssize_t n = write(c->fd, msg, len);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->closing = 1;
            return;
        }
        size_t sent = (n > 0) ? (size_t)n : 0;
        if (sent == len) return;  /* all gone                         */
        msg += sent;
        len -= sent;
    }

    if (wq_push(&c->wq, msg, len) < 0) { c->closing = 1; return; }

    /* Arm EPOLLOUT so we get notified when the socket drains. */
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLOUT | EPOLLET,
        .data.fd  = c->fd,
    };
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* ------------------------------------------------------------------ */
/* Protocol — line-delimited, echo each line back with a prefix       */
/* ------------------------------------------------------------------ */

static void process_lines(Conn *c, int epfd) {
    while (1) {
        char *nl = memchr(c->rbuf.data, '\n', c->rbuf.len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - c->rbuf.data) + 1; /* incl \n */

        /* Echo back: "echo: <line>" */
        char hdr[] = "echo: ";
        conn_send(c, epfd, hdr, sizeof hdr - 1);
        conn_send(c, epfd, c->rbuf.data, line_len);

        buf_consume(&c->rbuf, line_len);
    }
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* File descriptor helpers                                             */
/* ------------------------------------------------------------------ */

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* Shutdown flag (set by SIGINT handler)                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Accept loop — drain until EAGAIN (edge-triggered)                  */
/* ------------------------------------------------------------------ */

static void accept_loop(int listenfd, int epfd) {
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof addr;
        int fd = accept4(listenfd, (struct sockaddr *)&addr, &addrlen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            logmsg("accept4: %s", strerror(errno));
            return;
        }

        if (fd >= MAX_CONNS) {
            logmsg("fd %d exceeds MAX_CONNS, closing", fd);
            close(fd);
            continue;
        }

        /* TCP_NODELAY — reduce latency for small messages. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        Conn *c = conn_new(fd);
        if (!c) {
            logmsg("conn_new failed for fd %d", fd);
            close(fd);
            continue;
        }

        struct epoll_event ev = {
            .events  = EPOLLIN | EPOLLET,
            .data.fd = fd,
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            logmsg("epoll_ctl ADD fd %d: %s", fd, strerror(errno));
            conn_free(c);
            continue;
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof ipstr);
        logmsg("connect  fd=%-5d  %s:%d", fd, ipstr, ntohs(addr.sin_port));
    }
}

/* ------------------------------------------------------------------ */
/* Handle EPOLLIN — read until EAGAIN                                 */
/* ------------------------------------------------------------------ */

static void handle_read(Conn *c, int epfd) {
    while (1) {
        if (buf_ensure(&c->rbuf, 4096) < 0) {
            logmsg("fd %d: read buffer full, closing", c->fd);
            c->closing = 1;
            return;
        }
        ssize_t n = read(c->fd,
                         c->rbuf.data + c->rbuf.len,
                         c->rbuf.cap  - c->rbuf.len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            c->closing = 1;
            return;
        }
        if (n == 0) {           /* peer closed                        */
            c->closing = 1;
            return;
        }
        c->rbuf.len    += (size_t)n;
        c->last_active  = time(NULL);
        process_lines(c, epfd);
    }
}

/* ------------------------------------------------------------------ */
/* Handle EPOLLOUT — drain the write queue                            */
/* ------------------------------------------------------------------ */

static void handle_write(Conn *c, int epfd) {
    if (wq_flush(&c->wq, c->fd) < 0) { c->closing = 1; return; }

    if (wq_empty(&c->wq)) {
        /* Disarm EPOLLOUT — no more pending data. */
        struct epoll_event ev = {
            .events  = EPOLLIN | EPOLLET,
            .data.fd = c->fd,
        };
        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
}

/* ------------------------------------------------------------------ */
/* Idle timeout sweep                                                  */
/* ------------------------------------------------------------------ */

static void sweep_idle(int epfd) {
    time_t now    = time(NULL);
    time_t cutoff = now - IDLE_TIMEOUT_SEC;
    for (int fd = 0; fd < MAX_CONNS; fd++) {
        Conn *c = conn_table[fd];
        if (!c) continue;
        if (c->last_active < cutoff) {
            logmsg("timeout  fd=%-5d", fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            conn_free(c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Teardown — close all connections                                   */
/* ------------------------------------------------------------------ */

static void teardown(int epfd) {
    for (int fd = 0; fd < MAX_CONNS; fd++) {
        if (conn_table[fd]) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            conn_free(conn_table[fd]);
        }
    }
    close(epfd);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

    /* SIGINT — graceful shutdown. */
    struct sigaction sa = { .sa_handler = sigint_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Listening socket. */
    int listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listenfd, SOMAXCONN) < 0) {
        perror("listen"); return 1;
    }

    /* epoll instance. */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLET,
        .data.fd = listenfd,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        perror("epoll_ctl"); return 1;
    }

    logmsg("listening on port %d", port);

    struct epoll_event events[MAX_EVENTS];
    time_t last_sweep = time(NULL);

    while (!g_stop) {
        /* Timeout drives the idle sweep. */
        int timeout_ms = SWEEP_INTERVAL * 1000;
        int n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listenfd) {
                accept_loop(listenfd, epfd);
                continue;
            }

            Conn *c = (fd >= 0 && fd < MAX_CONNS) ? conn_table[fd] : NULL;
            if (!c) continue;

            uint32_t ev_flags = events[i].events;

            /* Errors and hangups. */
            if (ev_flags & (EPOLLERR | EPOLLHUP)) {
                c->closing = 1;
            }

            if (!c->closing && (ev_flags & EPOLLIN))  handle_read(c, epfd);
            if (!c->closing && (ev_flags & EPOLLOUT)) handle_write(c, epfd);

            /* Close if flagged, but only after draining the write queue. */
            if (c->closing) {
                /* Best-effort flush before closing. */
                wq_flush(&c->wq, fd);
                logmsg("close    fd=%-5d", fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                conn_free(c);
            }
        }

        /* Periodic idle sweep. */
        time_t now = time(NULL);
        if (now - last_sweep >= SWEEP_INTERVAL) {
            sweep_idle(epfd);
            last_sweep = now;
        }
    }

    logmsg("shutting down");
    close(listenfd);
    teardown(epfd);
    return 0;
}
