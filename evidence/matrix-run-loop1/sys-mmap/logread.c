/*
 * logread.c — memory-mapped binary log scanner
 *
 * Record format (little-endian):
 *   u32  len      — byte length of body
 *   u8   severity — first byte of body (0=DEBUG,1=INFO,2=WARN,3=ERROR)
 *   u8   body[]   — remainder of body (len bytes total, severity included)
 *
 * Usage:
 *   logread <file> <severity> <last_N>
 *
 *   severity: 0=DEBUG  1=INFO  2=WARN  3=ERROR
 *   last_N:   number of matching records to print (from end of file)
 *
 * Build:
 *   cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -o logread logread.c
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── severity labels ─────────────────────────────────────────────────── */

static const char *severity_name(uint8_t s)
{
    switch (s) {
    case 0: return "DEBUG";
    case 1: return "INFO";
    case 2: return "WARN";
    case 3: return "ERROR";
    default: return "UNKNOWN";
    }
}

/* ── record descriptor (index built during first pass) ───────────────── */

typedef struct {
    const uint8_t *body;   /* pointer into mapping, past the u32 header  */
    uint32_t       len;    /* body length in bytes (severity byte + data) */
    uint8_t        severity;
} Record;

/* ── cleanup helper ──────────────────────────────────────────────────── */

typedef struct {
    void  *map;
    size_t map_size;
    int    fd;
} MapCtx;

static void cleanup(MapCtx *ctx)
{
    if (ctx->map != MAP_FAILED && ctx->map != NULL)
        munmap(ctx->map, ctx->map_size);
    if (ctx->fd >= 0)
        close(ctx->fd);
}

/* ── first pass: count records per severity, build index ─────────────── */

/*
 * Returns a heap-allocated array of Record descriptors for every valid record
 * in the mapping, stored in file order.  *out_count receives the number of
 * valid records found.  On allocation failure returns NULL.
 *
 * The mapping window is [base, base+map_size).  Every access is bounds-checked
 * before a dereference: if the file is truncated or malformed we stop early
 * rather than reading past the mapped region.
 */
static Record *build_index(const uint8_t *base, size_t map_size,
                            size_t *out_count,
                            size_t severity_counts[4])
{
    /* Worst-case record count: every record is 1 byte body + 4 byte header. */
    size_t capacity = (map_size / 5) + 1;
    Record *idx = malloc(capacity * sizeof(Record));
    if (!idx)
        return NULL;

    size_t count   = 0;
    size_t offset  = 0;

    memset(severity_counts, 0, 4 * sizeof(size_t));

    while (offset + sizeof(uint32_t) <= map_size) {
        /* Read length field (little-endian u32). */
        uint32_t len;
        memcpy(&len, base + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        /* A zero-length body is structurally invalid; treat as end-of-log. */
        if (len == 0) {
            fprintf(stderr, "logread: zero-length record at offset %zu — "
                    "stopping scan\n", offset - sizeof(uint32_t));
            break;
        }

        /* Bounds check: does the declared body fit inside the mapping? */
        if ((size_t)len > map_size - offset) {
            fprintf(stderr, "logread: truncated record at offset %zu "
                    "(declared %"PRIu32" bytes, only %zu available) — "
                    "stopping scan\n",
                    offset - sizeof(uint32_t), len, map_size - offset);
            break;
        }

        const uint8_t *body = base + offset;
        uint8_t sev = body[0]; /* first byte of body is severity */

        /* Grow index if needed (amortised doubling). */
        if (count == capacity) {
            size_t new_cap = capacity * 2;
            Record *tmp = realloc(idx, new_cap * sizeof(Record));
            if (!tmp) {
                free(idx);
                return NULL;
            }
            idx      = tmp;
            capacity = new_cap;
        }

        idx[count].body     = body;
        idx[count].len      = len;
        idx[count].severity = sev;
        count++;

        if (sev < 4)
            severity_counts[sev]++;

        offset += (size_t)len;
    }

    *out_count = count;
    return idx;
}

/* ── print a single record body as text ──────────────────────────────── */

static void print_record(const Record *r, size_t record_number)
{
    /*
     * Body layout: byte[0] = severity, byte[1..len-1] = text payload.
     * We print the text payload.  If len == 1 there is no text beyond the
     * severity byte — still valid, just empty.
     */
    uint32_t text_len = r->len > 1u ? r->len - 1u : 0u;

    printf("[%s] #%zu  (%"PRIu32" byte%s body)  ",
           severity_name(r->severity),
           record_number,
           r->len,
           r->len == 1 ? "" : "s");

    if (text_len > 0)
        fwrite(r->body + 1, 1, text_len, stdout);

    putchar('\n');
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <file> <severity 0-3> <last_N>\n"
                "  severity: 0=DEBUG  1=INFO  2=WARN  3=ERROR\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    /* ── parse arguments ── */

    const char *path = argv[1];

    char *endptr;
    long sev_arg = strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || sev_arg < 0 || sev_arg > 3) {
        fprintf(stderr, "logread: severity must be 0, 1, 2, or 3\n");
        return EXIT_FAILURE;
    }
    uint8_t target_sev = (uint8_t)sev_arg;

    long n_arg = strtol(argv[3], &endptr, 10);
    if (*endptr != '\0' || n_arg < 0) {
        fprintf(stderr, "logread: last_N must be a non-negative integer\n");
        return EXIT_FAILURE;
    }
    size_t last_n = (size_t)n_arg;

    /* ── open and stat ── */

    MapCtx ctx = { .map = MAP_FAILED, .map_size = 0, .fd = -1 };

    ctx.fd = open(path, O_RDONLY | O_CLOEXEC);
    if (ctx.fd < 0) {
        fprintf(stderr, "logread: open(%s): %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstat(ctx.fd, &st) < 0) {
        fprintf(stderr, "logread: fstat(%s): %s\n", path, strerror(errno));
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "logread: %s is not a regular file\n", path);
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    if (st.st_size == 0) {
        printf("logread: %s is empty\n", path);
        cleanup(&ctx);
        return EXIT_SUCCESS;
    }

    ctx.map_size = (size_t)st.st_size;

    /* ── memory-map ── */

    ctx.map = mmap(NULL, ctx.map_size, PROT_READ, MAP_PRIVATE, ctx.fd, 0);
    if (ctx.map == MAP_FAILED) {
        fprintf(stderr, "logread: mmap(%s): %s\n", path, strerror(errno));
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    /* fd no longer needed after a successful mmap */
    close(ctx.fd);
    ctx.fd = -1;

    /* ── build index ── */

    size_t total_count = 0;
    size_t severity_counts[4];
    Record *idx = build_index((const uint8_t *)ctx.map, ctx.map_size,
                              &total_count, severity_counts);
    if (!idx) {
        fprintf(stderr, "logread: out of memory building record index\n");
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    /* ── print severity summary ── */

    printf("=== %s : %zu record%s total ===\n",
           path, total_count, total_count == 1 ? "" : "s");
    for (int s = 0; s < 4; s++) {
        printf("  %-7s  %zu\n", severity_name((uint8_t)s), severity_counts[s]);
    }
    putchar('\n');

    /* ── collect matching records (file order, severity filter) ── */

    size_t match_count = severity_counts[target_sev < 4 ? target_sev : 0];

    if (match_count == 0) {
        printf("No %s records found.\n", severity_name(target_sev));
        free(idx);
        cleanup(&ctx);
        return EXIT_SUCCESS;
    }

    /*
     * We want the last last_n matching records in file order.
     * Build a secondary index of pointers into idx[] for the target severity,
     * then print the tail of that list.
     */
    const Record **matches = malloc(match_count * sizeof(Record *));
    if (!matches) {
        fprintf(stderr, "logread: out of memory for match list\n");
        free(idx);
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    size_t mi = 0;
    for (size_t i = 0; i < total_count; i++) {
        if (idx[i].severity == target_sev)
            matches[mi++] = &idx[i];
    }
    /* mi == match_count by construction */

    size_t print_count = (last_n < match_count) ? last_n : match_count;
    size_t start       = match_count - print_count;

    printf("--- last %zu of %zu %s record%s ---\n",
           print_count, match_count,
           severity_name(target_sev),
           match_count == 1 ? "" : "s");

    for (size_t i = start; i < match_count; i++) {
        /* record_number is 1-based position among all records */
        size_t rec_num = (size_t)(matches[i] - idx) + 1;
        print_record(matches[i], rec_num);
    }

    /* ── clean up ── */

    free(matches);
    free(idx);
    cleanup(&ctx);

    return EXIT_SUCCESS;
}
