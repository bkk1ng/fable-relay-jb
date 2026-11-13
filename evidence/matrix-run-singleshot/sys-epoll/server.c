/* server.c — single-threaded edge-triggered epoll TCP server, C11, Linux
 *
 * Features:
 *   - Non-blocking accept loop draining until EAGAIN
 *   - EPOLLET (edge-triggered) per-connection read/write
 *   - Per-connection growable read buffer
 *   - Line-delimited protocol (handle_line — replace with your own logic)
 *   - Output queue with partial-write handling
 *   - Idle timeout sweep via timerfd
 *   - Clean SIGINT/SIGTERM shutdown via signalfd
 *
 * Build:  cc -std=c11 -O2 -Wall -Wextra -o server server.c
 * Run:    ./server <port>
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */
#define BACKLOG           512
#define MAX_EVENTS        64
#define RBUF_INIT         4096          /* initial per-conn read-buf capacity */
#define RBUF_MAX          (1u << 20)    /* 1 MiB — hard limit per line        */
#define IDLE_TIMEOUT_SEC  60            /* drop connections idle this long     */
#define IDLE_CHECK_SEC    15            /* sweep interval                      */

/* -------------------------------------------------------------------------
 * Write queue node — one heap allocation per enqueued chunk
 * ---------------------------------------------------------------------- */
typedef struct wnode {
    uint8_t       *data;
    size_t         len;
    size_t         off;     /* bytes already drained from this node           */
    struct wnode  *next;
} wnode_t;

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */
typedef struct conn {
    int         fd;

    /* read buffer */
    char       *rbuf;
    size_t      rlen;       /* bytes currently in buffer                      */
    size_t      rcap;       /* buffer capacity                                */

    /* write queue */
    wnode_t    *whead;
    wnode_t    *wtail;

    /* idle tracking */
    time_t      last_active;

    /* intrusive doubly-linked list of all live connections (idle sweep)     */
    struct conn *prev;
    struct conn *next;
} conn_t;

/* -------------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */
static int      epfd      = -1;
static int      srvfd     = -1;
static int      sigfd     = -1;
static int      tfd       = -1;
static conn_t **ctable;             /* fd → conn_t*, allocated once at start  */
static int      ctable_sz;
static conn_t  *conn_list;          /* head of live-connection intrusive list  */
static int      running   = 1;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static void conn_close(conn_t *c);
static int  conn_flush(conn_t *c);
static int  conn_send(conn_t *c, const void *data, size_t len);

/* -------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */
static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

/* -------------------------------------------------------------------------
 * Connection table — flat array indexed by fd
 * ---------------------------------------------------------------------- */
static void ctable_init(void)
{
    struct rlimit rl;
    ctable_sz = (getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
                 rl.rlim_cur != RLIM_INFINITY)
                ? (int)rl.rlim_cur
                : 65536;
    ctable = calloc((size_t)ctable_sz, sizeof *ctable);
    if (!ctable) die("calloc ctable");
}

static inline conn_t *ctable_get(int fd)
{
    return (fd >= 0 && fd < ctable_sz) ? ctable[fd] : NULL;
}

static inline void ctable_set(int fd, conn_t *c)
{
    if (fd >= 0 && fd < ctable_sz) ctable[fd] = c;
}

/* -------------------------------------------------------------------------
 * Intrusive list helpers
 * ---------------------------------------------------------------------- */
static void list_push(conn_t *c)
{
    c->prev = NULL;
    c->next = conn_list;
    if (conn_list) conn_list->prev = c;
    conn_list = c;
}

static void list_remove(conn_t *c)
{
    if (c->prev) c->prev->next = c->next;
    else         conn_list     = c->next;
    if (c->next) c->next->prev = c->prev;
    c->prev = c->next = NULL;
}

/* -------------------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------------- */
static conn_t *conn_new(int fd)
{
    conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    c->fd   = fd;
    c->rcap = RBUF_INIT;
    c->rbuf = malloc(c->rcap);
    if (!c->rbuf) { free(c); return NULL; }

    c->last_active = time(NULL);
    ctable_set(fd, c);
    list_push(c);
    return c;
}

static void conn_close(conn_t *c)
{
    if (!c) return;

    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    ctable_set(c->fd, NULL);
    list_remove(c);

    free(c->rbuf);

    for (wnode_t *n = c->whead, *nx; n; n = nx) {
        nx = n->next;
        free(n->data);
        free(n);
    }
    free(c);
}

/* -------------------------------------------------------------------------
 * Write queue
 * ---------------------------------------------------------------------- */

/* Append a copy of [data, data+len) to the tail of the write queue. */
static int conn_enqueue(conn_t *c, const void *data, size_t len)
{
    if (!len) return 0;

    wnode_t *node = malloc(sizeof *node);
    if (!node) return -1;

    node->data = malloc(len);
    if (!node->data) { free(node); return -1; }

    memcpy(node->data, data, len);
    node->len  = len;
    node->off  = 0;
    node->next = NULL;

    if (c->wtail) c->wtail->next = node;
    else          c->whead       = node;
    c->wtail = node;
    return 0;
}

/*
 * Drain the write queue until it empties or the socket returns EAGAIN.
 * Arms/disarms EPOLLOUT as needed.
 * Returns 0 on success, -1 on hard error.
 */
static int conn_flush(conn_t *c)
{
    while (c->whead) {
        wnode_t *n   = c->whead;
        ssize_t  ret = write(c->fd, n->data + n->off, n->len - n->off);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* still have data — keep EPOLLOUT armed */
                struct epoll_event ev = {
                    .events  = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                    .data.fd = c->fd
                };
                epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
                return 0;
            }
            return -1;
        }
        n->off += (size_t)ret;
        if (n->off >= n->len) {
            c->whead = n->next;
            if (!c->whead) c->wtail = NULL;
            free(n->data);
            free(n);
        }
    }
    /* queue drained — remove EPOLLOUT to avoid spurious wakeups */
    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLRDHUP | EPOLLET,
        .data.fd = c->fd
    };
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    return 0;
}

/*
 * Public send: attempt an immediate write, enqueue whatever could not be
 * flushed, arm EPOLLOUT if the queue is now non-empty.
 * Returns 0 on success, -1 on error.
 */
static int conn_send(conn_t *c, const void *data, size_t len)
{
    /* Preserve ordering: if there is already queued data, go straight to
     * the queue rather than writing out of order. */
    if (c->whead) return conn_enqueue(c, data, len);

    const uint8_t *p    = data;
    size_t         sent = 0;

    while (sent < len) {
        ssize_t ret = write(c->fd, p + sent, len - sent);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        sent += (size_t)ret;
    }

    if (sent < len) {
        if (conn_enqueue(c, p + sent, len - sent) < 0) return -1;
        struct epoll_event ev = {
            .events  = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
            .data.fd = c->fd
        };
        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Protocol handler
 *
 * Called once per complete line (CR stripped, no trailing newline).
 * Default: echo the line back with CRLF.  Replace this with your logic.
 * Returns 0 on success, -1 to close the connection.
 * ---------------------------------------------------------------------- */
static int handle_line(conn_t *c, const char *line, size_t len)
{
    /* Echo: line + CRLF */
    char *resp = malloc(len + 2);
    if (!resp) return -1;
    memcpy(resp, line, len);
    resp[len]     = '\r';
    resp[len + 1] = '\n';
    int ret = conn_send(c, resp, len + 2);
    free(resp);
    return ret;
}

/* -------------------------------------------------------------------------
 * Read path — drain socket into growable buffer, dispatch complete lines
 * ---------------------------------------------------------------------- */
static int conn_do_read(conn_t *c)
{
    for (;;) {
        /* Grow read buffer if full. */
        if (c->rlen == c->rcap) {
            if (c->rcap >= RBUF_MAX) return -1;   /* line too long, drop   */
            size_t newcap = (c->rcap * 2 < RBUF_MAX) ? c->rcap * 2 : RBUF_MAX;
            char *nb = realloc(c->rbuf, newcap);
            if (!nb) return -1;
            c->rbuf = nb;
            c->rcap = newcap;
        }

        ssize_t n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* done  */
            return -1;
        }
        if (n == 0) return -1;  /* peer closed */

        c->rlen        += (size_t)n;
        c->last_active  = time(NULL);

        /* Dispatch every complete line. */
        char *start = c->rbuf;
        char *fence = c->rbuf + c->rlen;
        char *nl;

        while ((nl = memchr(start, '\n', (size_t)(fence - start)))) {
            size_t linelen = (size_t)(nl - start);
            if (linelen > 0 && start[linelen - 1] == '\r') linelen--;  /* strip CR */

            if (handle_line(c, start, linelen) < 0) return -1;
            start = nl + 1;
        }

        /* Compact: slide any partial line to the front of the buffer. */
        size_t remaining = (size_t)(fence - start);
        if (remaining && start != c->rbuf) memmove(c->rbuf, start, remaining);
        c->rlen = remaining;
    }
}

/* -------------------------------------------------------------------------
 * Accept loop — drain until EAGAIN (required by EPOLLET)
 * ---------------------------------------------------------------------- */
static void do_accept(void)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof addr;
        int cfd = accept4(srvfd, (struct sockaddr *)&addr, &addrlen,
                          SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            perror("accept4");
            return;
        }

        /* Disable Nagle for the line protocol. */
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        conn_t *c = conn_new(cfd);
        if (!c) { close(cfd); continue; }

        struct epoll_event ev = {
            .events  = EPOLLIN | EPOLLRDHUP | EPOLLET,
            .data.fd = cfd
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) conn_close(c);
    }
}

/* -------------------------------------------------------------------------
 * Idle sweep — close connections that have been silent too long
 * ---------------------------------------------------------------------- */
static void do_idle_sweep(void)
{
    time_t cutoff = time(NULL) - IDLE_TIMEOUT_SEC;
    for (conn_t *c = conn_list, *next; c; c = next) {
        next = c->next;
        if (c->last_active < cutoff) conn_close(c);
    }
}

/* -------------------------------------------------------------------------
 * Signal handler (via signalfd)
 * ---------------------------------------------------------------------- */
static void do_signal(void)
{
    struct signalfd_siginfo si;
    ssize_t ret = read(sigfd, &si, sizeof si);
    if (ret != (ssize_t)sizeof si) return;
    if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
        fprintf(stderr, "\nShutting down (signal %u).\n", si.ssi_signo);
        running = 0;
    }
}

/* -------------------------------------------------------------------------
 * Server socket
 * ---------------------------------------------------------------------- */
static int make_server_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) die("socket");

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) die("bind");
    if (listen(fd, BACKLOG) < 0) die("listen");
    return fd;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int portval = atoi(argv[1]);
    if (portval <= 0 || portval > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    uint16_t port = (uint16_t)portval;

    ctable_init();

    /* Block SIGINT/SIGTERM — receive them via signalfd instead. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) die("sigprocmask");
    sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) die("signalfd");

    /* Timer for idle sweep. */
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) die("timerfd_create");
    struct itimerspec its = {
        .it_interval = { .tv_sec = IDLE_CHECK_SEC, .tv_nsec = 0 },
        .it_value    = { .tv_sec = IDLE_CHECK_SEC, .tv_nsec = 0 },
    };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) die("timerfd_settime");

    srvfd = make_server_socket(port);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) die("epoll_create1");

    struct epoll_event ev;

    ev.events  = EPOLLIN | EPOLLET;     /* edge-triggered for accept */
    ev.data.fd = srvfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, srvfd, &ev) < 0) die("epoll_ctl srvfd");

    ev.events  = EPOLLIN;               /* level-triggered is fine for these  */
    ev.data.fd = sigfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ev) < 0) die("epoll_ctl sigfd");

    ev.events  = EPOLLIN;
    ev.data.fd = tfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) < 0) die("epoll_ctl tfd");

    fprintf(stderr, "Listening on port %u  (idle timeout %ds, sweep every %ds)\n",
            port, IDLE_TIMEOUT_SEC, IDLE_CHECK_SEC);

    /* ------------------------------------------------------------------ */
    /* Event loop                                                          */
    /* ------------------------------------------------------------------ */
    struct epoll_event events[MAX_EVENTS];

    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait");
        }

        for (int i = 0; i < n; i++) {
            int      fd = events[i].data.fd;
            uint32_t e  = events[i].events;

            if (fd == srvfd) {
                do_accept();

            } else if (fd == sigfd) {
                do_signal();

            } else if (fd == tfd) {
                uint64_t exp;
                (void)read(tfd, &exp, sizeof exp);  /* must drain */
                do_idle_sweep();

            } else {
                conn_t *c = ctable_get(fd);
                if (!c) continue;

                int close_conn = 0;

                if (e & (EPOLLERR | EPOLLHUP)) {
                    close_conn = 1;
                } else {
                    /* Read first so we don't lose data on a simultaneous
                     * EPOLLRDHUP — conn_do_read returns -1 on EOF. */
                    if (e & EPOLLIN)
                        close_conn |= (conn_do_read(c) < 0);

                    if (!close_conn && (e & EPOLLOUT))
                        close_conn |= (conn_flush(c) < 0);

                    if (!close_conn && (e & EPOLLRDHUP))
                        close_conn = 1;
                }

                if (close_conn) conn_close(c);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Shutdown                                                            */
    /* ------------------------------------------------------------------ */
    while (conn_list) conn_close(conn_list);

    close(srvfd);
    close(sigfd);
    close(tfd);
    close(epfd);
    free(ctable);

    fprintf(stderr, "Server stopped.\n");
    return EXIT_SUCCESS;
}
