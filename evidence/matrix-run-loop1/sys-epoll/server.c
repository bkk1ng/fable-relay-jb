/*
 * server.c — single-threaded epoll edge-triggered TCP server
 *
 * C11, Linux, epoll(7)
 *
 * Features:
 *   - Non-blocking accept loop draining until EAGAIN
 *   - Edge-triggered epoll for read and write readiness
 *   - Per-connection growable input buffer, line-delimited protocol
 *   - Output queue with partial-write handling
 *   - Idle timeout sweep (SIGALRM-driven)
 *   - Clean SIGINT shutdown
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o server server.c
 *
 * Usage:
 *   ./server <port>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define MAX_EVENTS          64
#define INITIAL_BUF_SIZE    4096
#define MAX_BUF_SIZE        (1 << 20)   /* 1 MiB hard cap per connection  */
#define MAX_LINE_LEN        65536       /* reject lines longer than this   */
#define IDLE_TIMEOUT_SEC    60          /* close idle connections after 60s */
#define SWEEP_INTERVAL_SEC  15          /* idle sweep cadence              */
#define BACKLOG             128

/* -------------------------------------------------------------------------
 * Growable byte buffer
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;    /* bytes currently stored                             */
    size_t   cap;    /* allocated capacity                                 */
    size_t   start;  /* read offset (ring-buffer style for outq)           */
} Buffer;

static bool buf_init(Buffer *b, size_t initial_cap)
{
    b->data = malloc(initial_cap);
    if (!b->data) return false;
    b->len   = 0;
    b->cap   = initial_cap;
    b->start = 0;
    return true;
}

static void buf_free(Buffer *b)
{
    free(b->data);
    b->data  = NULL;
    b->len   = 0;
    b->cap   = 0;
    b->start = 0;
}

/* Append bytes to end; returns false on allocation failure or hard cap. */
static bool buf_append(Buffer *b, const uint8_t *src, size_t n)
{
    if (b->len + n > MAX_BUF_SIZE) return false;

    /* Compact before reallocating if there is dead space at the front. */
    if (b->start > 0 && b->start + b->len + n > b->cap) {
        memmove(b->data, b->data + b->start, b->len);
        b->start = 0;
    }

    if (b->start + b->len + n > b->cap) {
        size_t need = b->start + b->len + n;
        size_t newcap = b->cap;
        while (newcap < need) newcap *= 2;
        uint8_t *nd = realloc(b->data, newcap);
        if (!nd) return false;
        b->data = nd;
        b->cap  = newcap;
    }

    memcpy(b->data + b->start + b->len, src, n);
    b->len += n;
    return true;
}

/* Consume n bytes from the front. */
static void buf_consume(Buffer *b, size_t n)
{
    if (n >= b->len) {
        b->len   = 0;
        b->start = 0;
    } else {
        b->start += n;
        b->len   -= n;
    }
}

/* Pointer to the first valid byte. */
static inline uint8_t *buf_head(const Buffer *b)
{
    return b->data + b->start;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef struct Conn Conn;
struct Conn {
    int      fd;
    time_t   last_active;

    Buffer   inbuf;   /* received bytes not yet processed                  */
    Buffer   outbuf;  /* bytes pending write                               */

    bool     write_armed;   /* EPOLLOUT currently registered               */
    bool     closing;       /* half-closed; drain outbuf then close        */

    Conn    *next;    /* intrusive linked list (all_conns)                 */
    Conn    *prev;
};

/* -------------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_quit        = 0;
static volatile sig_atomic_t g_do_sweep    = 0;

static int     g_epfd   = -1;
static Conn   *g_conns  = NULL;   /* doubly-linked list of live connections */
static int     g_listen_fd = -1;

/* -------------------------------------------------------------------------
 * Signal handlers
 * ---------------------------------------------------------------------- */

static void handle_sigint(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void handle_sigalrm(int sig)
{
    (void)sig;
    g_do_sweep = 1;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void conn_list_add(Conn *c)
{
    c->prev = NULL;
    c->next = g_conns;
    if (g_conns) g_conns->prev = c;
    g_conns = c;
}

static void conn_list_remove(Conn *c)
{
    if (c->prev) c->prev->next = c->next;
    else         g_conns       = c->next;
    if (c->next) c->next->prev = c->prev;
}

/* -------------------------------------------------------------------------
 * epoll arm/disarm helpers
 * ---------------------------------------------------------------------- */

static int epoll_arm(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/* Add EPOLLOUT to watch the connection for write-readiness. */
static void arm_write(Conn *c)
{
    if (c->write_armed) return;
    uint32_t ev = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    if (epoll_arm(g_epfd, c->fd, ev, c) == 0)
        c->write_armed = true;
}

/* Remove EPOLLOUT once the output queue drains. */
static void disarm_write(Conn *c)
{
    if (!c->write_armed) return;
    uint32_t ev = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (epoll_arm(g_epfd, c->fd, ev, c) == 0)
        c->write_armed = false;
}

/* -------------------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------------- */

static void conn_close(Conn *c)
{
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_list_remove(c);
    buf_free(&c->inbuf);
    buf_free(&c->outbuf);
    free(c);
}

static Conn *conn_new(int fd)
{
    Conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    if (!buf_init(&c->inbuf,  INITIAL_BUF_SIZE) ||
        !buf_init(&c->outbuf, INITIAL_BUF_SIZE)) {
        buf_free(&c->inbuf);
        buf_free(&c->outbuf);
        free(c);
        return NULL;
    }

    c->fd          = fd;
    c->last_active = time(NULL);
    c->write_armed = false;
    c->closing     = false;

    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLET | EPOLLRDHUP,
        .data.ptr = c,
    };
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        buf_free(&c->inbuf);
        buf_free(&c->outbuf);
        free(c);
        return NULL;
    }

    conn_list_add(c);
    return c;
}

/* -------------------------------------------------------------------------
 * Protocol handler — called once per complete line
 *
 * Replace this with real application logic.  The echo below is intentional:
 * it demonstrates how to enqueue a response into the output buffer.
 * ---------------------------------------------------------------------- */

static bool handle_line(Conn *c, const uint8_t *line, size_t len)
{
    /* Echo the line back with a trailing newline. */
    if (!buf_append(&c->outbuf, line, len))          return false;
    if (!buf_append(&c->outbuf, (uint8_t *)"\n", 1)) return false;
    return true;
}

/* -------------------------------------------------------------------------
 * Flush the output queue; returns false on hard error.
 * ---------------------------------------------------------------------- */

static bool flush_outbuf(Conn *c)
{
    while (c->outbuf.len > 0) {
        ssize_t n = write(c->fd,
                          buf_head(&c->outbuf),
                          c->outbuf.len);
        if (n > 0) {
            buf_consume(&c->outbuf, (size_t)n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Socket not ready; re-arm EPOLLOUT and wait. */
                arm_write(c);
                return true;
            }
            if (errno == EINTR) continue;
            return false;   /* real write error */
        }
    }

    if (c->outbuf.len == 0) {
        disarm_write(c);
        if (c->closing) return false;   /* signal caller to close */
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Read available data and process complete lines.
 * ---------------------------------------------------------------------- */

static void handle_readable(Conn *c)
{
    uint8_t tmp[4096];

    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof tmp);
        if (n > 0) {
            c->last_active = time(NULL);

            if (!buf_append(&c->inbuf, tmp, (size_t)n)) {
                /* Buffer full or OOM — close the connection. */
                conn_close(c);
                return;
            }

            /* Process complete lines (LF-delimited). */
            for (;;) {
                uint8_t *head = buf_head(&c->inbuf);
                uint8_t *nl   = memchr(head, '\n', c->inbuf.len);
                if (!nl) break;

                size_t line_len = (size_t)(nl - head);   /* excludes '\n' */

                if (line_len > MAX_LINE_LEN) {
                    conn_close(c);
                    return;
                }

                /* Strip trailing CR for CRLF clients. */
                size_t payload_len = line_len;
                if (payload_len > 0 && head[payload_len - 1] == '\r')
                    payload_len--;

                if (!handle_line(c, head, payload_len)) {
                    conn_close(c);
                    return;
                }

                buf_consume(&c->inbuf, line_len + 1);   /* +1 for '\n' */
            }

            /* Attempt to flush any queued output immediately. */
            if (!flush_outbuf(c)) {
                conn_close(c);
                return;
            }

        } else if (n == 0) {
            /* EOF: peer closed the write side.  Drain output then close. */
            c->closing = true;
            if (c->outbuf.len == 0) {
                conn_close(c);
            }
            return;

        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  /* done */
            if (errno == EINTR) continue;
            conn_close(c);
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * Accept loop — drain until EAGAIN (required with edge-triggered epoll)
 * ---------------------------------------------------------------------- */

static void accept_loop(int listen_fd)
{
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof addr;

        int fd = accept4(listen_fd, (struct sockaddr *)&addr, &addrlen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR)  continue;
            /* EMFILE/ENFILE/ENOBUFS: log and keep going. */
            perror("accept4");
            return;
        }

        /* Enable TCP_NODELAY to minimise latency on interactive workloads. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        Conn *c = conn_new(fd);
        if (!c) {
            close(fd);
            fprintf(stderr, "conn_new: out of memory\n");
        }
    }
}

/* -------------------------------------------------------------------------
 * Idle-connection sweep
 * ---------------------------------------------------------------------- */

static void idle_sweep(void)
{
    time_t now     = time(NULL);
    Conn  *c       = g_conns;

    while (c) {
        Conn *next = c->next;   /* snapshot before possible free */
        if (now - c->last_active >= IDLE_TIMEOUT_SEC) {
            conn_close(c);
        }
        c = next;
    }
}

/* -------------------------------------------------------------------------
 * Graceful shutdown — flush then close all connections and the listener
 * ---------------------------------------------------------------------- */

static void shutdown_server(void)
{
    Conn *c = g_conns;
    while (c) {
        Conn *next = c->next;
        conn_close(c);
        c = next;
    }

    if (g_listen_fd != -1) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_epfd != -1) {
        close(g_epfd);
        g_epfd = -1;
    }
}

/* -------------------------------------------------------------------------
 * Listener setup
 * ---------------------------------------------------------------------- */

static int create_listener(const char *port)
{
    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags    = AI_PASSIVE | AI_NUMERICSERV,
    };
    struct addrinfo *res, *rp;

    int err = getaddrinfo(NULL, port, &hints, &res);
    if (err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC,
                    rp->ai_protocol);
        if (fd == -1) continue;

        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

        /* Disable IPv4-mapped addresses on dual-stack systems so we get
         * two separate listening sockets (one per family) if desired.    */
        if (rp->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);

        if (set_nonblocking(fd) == -1) {
            close(fd);
            fd = -1;
            continue;
        }

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(fd, BACKLOG) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
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

    /* Signals */
    struct sigaction sa_int  = { .sa_handler = handle_sigint  };
    struct sigaction sa_alrm = { .sa_handler = handle_sigalrm };
    sigaction(SIGINT,  &sa_int,  NULL);
    sigaction(SIGTERM, &sa_int,  NULL);
    sigaction(SIGALRM, &sa_alrm, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_listen_fd = create_listener(argv[1]);
    if (g_listen_fd == -1) {
        fprintf(stderr, "failed to bind on port %s\n", argv[1]);
        return 1;
    }

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd == -1) {
        perror("epoll_create1");
        close(g_listen_fd);
        return 1;
    }

    /* Register the listener with edge-triggered epoll. */
    struct epoll_event lev = {
        .events   = EPOLLIN | EPOLLET,
        .data.fd  = g_listen_fd,
    };
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &lev) == -1) {
        perror("epoll_ctl listen");
        close(g_epfd);
        close(g_listen_fd);
        return 1;
    }

    /* Schedule the first idle sweep. */
    alarm(SWEEP_INTERVAL_SEC);

    printf("listening on port %s\n", argv[1]);

    struct epoll_event events[MAX_EVENTS];

    while (!g_quit) {
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, -1);

        if (n == -1) {
            if (errno == EINTR) {
                /* Check sweep flag before looping. */
                if (g_do_sweep) {
                    g_do_sweep = 0;
                    idle_sweep();
                    alarm(SWEEP_INTERVAL_SEC);
                }
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            uint32_t ev = events[i].events;

            if (events[i].data.fd == g_listen_fd) {
                /* New connection(s). */
                accept_loop(g_listen_fd);
                continue;
            }

            Conn *c = events[i].data.ptr;

            if (ev & (EPOLLHUP | EPOLLERR)) {
                conn_close(c);
                continue;
            }

            if (ev & EPOLLRDHUP) {
                /* Peer closed write side — drain then close. */
                c->closing = true;
            }

            if (ev & EPOLLIN) {
                handle_readable(c);
                /* c may be freed at this point; do not touch it after. */
                continue;
            }

            if (ev & EPOLLOUT) {
                if (!flush_outbuf(c))
                    conn_close(c);
            }
        }

        /* Process deferred sweep if alarm fired during epoll_wait. */
        if (g_do_sweep) {
            g_do_sweep = 0;
            idle_sweep();
            alarm(SWEEP_INTERVAL_SEC);
        }
    }

    printf("shutting down\n");
    shutdown_server();
    return 0;
}
