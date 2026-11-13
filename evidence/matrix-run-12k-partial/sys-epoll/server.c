/*
 * server.c — single-threaded, edge-triggered epoll TCP server
 *
 * Protocol  : line-delimited (\n terminated messages)
 * Build     : gcc -std=c11 -O2 -Wall -Wextra -o server server.c
 * Run       : ./server <port>
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
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */
#define MAX_EVENTS       64
#define INITIAL_BUF_CAP  4096
#define IDLE_TIMEOUT_SEC 60      /* close connections silent for this long  */
#define TIMEOUT_SWEEP_MS 10000   /* how often we walk the connection list   */

/* -------------------------------------------------------------------------
 * Growable byte buffer
 * ---------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t   len;   /* bytes currently stored                              */
    size_t   cap;   /* allocated capacity                                  */
} buf_t;

static int buf_ensure(buf_t *b, size_t extra)
{
    size_t need = b->len + extra;
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : INITIAL_BUF_CAP;
    while (ncap < need) ncap *= 2;
    uint8_t *p = realloc(b->data, ncap);
    if (!p) return -1;
    b->data = p;
    b->cap  = ncap;
    return 0;
}

static void buf_consume(buf_t *b, size_t n)
{
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */
typedef struct conn conn_t;
struct conn {
    int        fd;
    buf_t      rbuf;      /* incoming data                                 */
    buf_t      wbuf;      /* pending outbound data (output queue)          */
    time_t     last_active;
    conn_t    *prev;
    conn_t    *next;
};

/* Intrusive doubly-linked list helpers */
static conn_t *conn_list_head = NULL;

static conn_t *conn_alloc(int fd)
{
    conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd          = fd;
    c->last_active = time(NULL);
    /* prepend */
    c->next = conn_list_head;
    if (conn_list_head) conn_list_head->prev = c;
    conn_list_head = c;
    return c;
}

static void conn_unlink(conn_t *c)
{
    if (c->prev) c->prev->next = c->next;
    else         conn_list_head = c->next;
    if (c->next) c->next->prev = c->prev;
    c->prev = c->next = NULL;
}

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */
static volatile sig_atomic_t g_stop = 0;
static int                   g_epfd = -1;

/* -------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------- */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_conn(conn_t *c)
{
    printf("conn fd=%d closed\n", c->fd);
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_unlink(c);
    buf_free(&c->rbuf);
    buf_free(&c->wbuf);
    free(c);
}

/*
 * Rearm the epoll event for a connection.
 * Include EPOLLOUT only when there is pending write data.
 */
static int conn_rearm(conn_t *c)
{
    struct epoll_event ev = {
        .data.ptr = c,
        .events   = EPOLLIN | EPOLLET | EPOLLRDHUP
                    | (c->wbuf.len ? EPOLLOUT : 0)
    };
    return epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* -------------------------------------------------------------------------
 * Line-delimited protocol handler
 * Processes every complete \n-terminated line in the read buffer.
 * Returns -1 if the connection should be closed.
 * ---------------------------------------------------------------------- */
static int process_lines(conn_t *c)
{
    for (;;) {
        uint8_t *nl = memchr(c->rbuf.data, '\n', c->rbuf.len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - c->rbuf.data) + 1; /* includes \n */

        /* --- application logic: echo the line back ------------------- */
        if (buf_ensure(&c->wbuf, line_len) < 0) return -1;
        memcpy(c->wbuf.data + c->wbuf.len, c->rbuf.data, line_len);
        c->wbuf.len += line_len;
        /* ------------------------------------------------------------- */

        buf_consume(&c->rbuf, line_len);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Flush the output queue.  With edge-triggered epoll we must keep writing
 * until EAGAIN; any unsent bytes stay in wbuf and we set EPOLLOUT.
 * Returns -1 if the connection should be closed.
 * ---------------------------------------------------------------------- */
static int flush_wbuf(conn_t *c)
{
    while (c->wbuf.len > 0) {
        ssize_t n = write(c->fd, c->wbuf.data, c->wbuf.len);
        if (n > 0) {
            buf_consume(&c->wbuf, (size_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;   /* socket full — will retry when EPOLLOUT fires      */
        } else {
            return -1;   /* peer reset or other hard error               */
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Drain the read side until EAGAIN (required for edge-triggered).
 * Returns -1 if the connection should be closed.
 * ---------------------------------------------------------------------- */
static int drain_rfd(conn_t *c)
{
    for (;;) {
        if (buf_ensure(&c->rbuf, 4096) < 0) return -1;

        ssize_t n = read(c->fd,
                         c->rbuf.data + c->rbuf.len,
                         c->rbuf.cap  - c->rbuf.len);
        if (n > 0) {
            c->rbuf.len    += (size_t)n;
            c->last_active  = time(NULL);
            if (process_lines(c) < 0) return -1;
        } else if (n == 0) {
            return -1;   /* EOF                                            */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * Accept loop — drain until EAGAIN (edge-triggered listen socket)
 * ---------------------------------------------------------------------- */
static void accept_loop(int listenfd)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof addr;

        int fd = accept4(listenfd, (struct sockaddr *)&addr, &addrlen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR)                          continue;
            perror("accept4");
            return;
        }

        /* Disable Nagle for lower latency on small messages */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        conn_t *c = conn_alloc(fd);
        if (!c) { close(fd); continue; }

        struct epoll_event ev = {
            .data.ptr = c,
            .events   = EPOLLIN | EPOLLET | EPOLLRDHUP
        };
        if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl add");
            conn_unlink(c);
            free(c);
            close(fd);
            continue;
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof ipstr);
        printf("new conn fd=%d from %s:%d\n",
               fd, ipstr, ntohs(addr.sin_port));
    }
}

/* -------------------------------------------------------------------------
 * Idle timeout sweep
 * ---------------------------------------------------------------------- */
static void sweep_idle(void)
{
    time_t now     = time(NULL);
    conn_t *c      = conn_list_head;
    while (c) {
        conn_t *next = c->next;
        if (now - c->last_active > IDLE_TIMEOUT_SEC) {
            printf("conn fd=%d idle timeout\n", c->fd);
            close_conn(c);
        }
        c = next;
    }
}

/* -------------------------------------------------------------------------
 * Graceful shutdown — drain output queues, then close everything
 * ---------------------------------------------------------------------- */
static void shutdown_all(void)
{
    conn_t *c = conn_list_head;
    while (c) {
        conn_t *next = c->next;
        flush_wbuf(c);   /* best-effort flush */
        close_conn(c);
        c = next;
    }
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

    /* SIGINT for clean shutdown */
    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0) { perror("sigaction"); return 1; }
    /* Ignore SIGPIPE so broken peer writes surface as EPIPE errno */
    signal(SIGPIPE, SIG_IGN);

    /* Listening socket */
    int listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0);
    if (listenfd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listenfd, SOMAXCONN) < 0) {
        perror("listen"); return 1;
    }

    /* epoll instance */
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) { perror("epoll_create1"); return 1; }

    /* Watch the listen socket — edge-triggered so we drain in a loop */
    struct epoll_event ev = {
        .data.fd = listenfd,
        .events  = EPOLLIN | EPOLLET
    };
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        perror("epoll_ctl listen"); return 1;
    }

    printf("listening on port %d\n", port);

    struct epoll_event events[MAX_EVENTS];
    time_t last_sweep = time(NULL);

    /* ---- event loop -------------------------------------------------- */
    while (!g_stop) {
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, TIMEOUT_SWEEP_MS);
        if (n < 0) {
            if (errno == EINTR) continue;   /* likely SIGINT              */
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            uint32_t ev_flags = events[i].events;

            if (events[i].data.fd == listenfd) {
                accept_loop(listenfd);
                continue;
            }

            conn_t *c = events[i].data.ptr;

            /* Peer closed or error */
            if (ev_flags & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                close_conn(c);
                continue;
            }

            int dead = 0;

            if (ev_flags & EPOLLIN)  dead |= (drain_rfd(c)   < 0);
            if (!dead)
            if (ev_flags & EPOLLOUT) dead |= (flush_wbuf(c)  < 0);

            if (dead) { close_conn(c); continue; }

            /* After processing, flush anything process_lines queued */
            if (!dead && c->wbuf.len > 0)
                dead |= (flush_wbuf(c) < 0);

            if (dead) { close_conn(c); continue; }

            /* Rearm: add EPOLLOUT only if output queue is non-empty */
            if (conn_rearm(c) < 0) {
                close_conn(c);
            }
        }

        /* Periodic idle sweep */
        time_t now = time(NULL);
        if (now - last_sweep >= (TIMEOUT_SWEEP_MS / 1000)) {
            sweep_idle();
            last_sweep = now;
        }
    }

    printf("\nshutting down\n");
    shutdown_all();
    close(listenfd);
    close(g_epfd);
    return 0;
}
