/*
 * logread.c — memory-mapped binary log scanner
 *
 * Record layout (little-endian):
 *   [uint32_t body_len][body_len bytes of body]
 *   body[0] is the severity byte when body_len >= 1
 *
 * Usage: logread <file> <severity 0-255> <last-N>
 *
 * Exits 0 on clean parse, 1 on fatal error, 2 on truncated/corrupt data.
 *
 * Build:  cc -std=c11 -Wall -Wextra -o logread logread.c
 */

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

/* Each record begins with a 4-byte little-endian length. */
#define HEADER_SIZE  4u
/* The minimum body length needed to carry a severity byte. */
#define MIN_BODY_LEN 1u

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Read a little-endian uint32 from a potentially-unaligned pointer. */
static uint32_t read_u32le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <file> <severity 0-255> <last-N>\n", prog);
}

/* ---------------------------------------------------------------------------
 * Dynamic array of matched record descriptors
 *
 * We store a pointer into the mapped region and the body length so that
 * we can print the last N entries in a second pass without re-scanning.
 * ------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *body;  /* points into the mapped region */
    uint32_t       len;   /* body_len from the record header */
} MatchedRec;

typedef struct {
    MatchedRec *data;
    size_t      count;
    size_t      cap;
} MatchVec;

/* Returns 0 on success, -1 on allocation failure. */
static int matchvec_push(MatchVec *v, const uint8_t *body, uint32_t len)
{
    if (v->count == v->cap) {
        size_t newcap = (v->cap == 0) ? 128 : v->cap * 2;
        MatchedRec *tmp = realloc(v->data, newcap * sizeof(*tmp));
        if (!tmp) return -1;
        v->data = tmp;
        v->cap  = newcap;
    }
    v->data[v->count].body = body;
    v->data[v->count].len  = len;
    v->count++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];

    /* Parse severity (0-255). */
    char *endp;
    errno = 0;
    long sev_arg = strtol(argv[2], &endp, 10);
    if (errno || *endp != '\0' || sev_arg < 0 || sev_arg > 255) {
        fprintf(stderr, "logread: severity must be an integer 0-255\n");
        return 1;
    }
    uint8_t target_sev = (uint8_t)sev_arg;

    /* Parse last-N (positive). */
    errno = 0;
    long n_arg = strtol(argv[3], &endp, 10);
    if (errno || *endp != '\0' || n_arg <= 0) {
        fprintf(stderr, "logread: last-N must be a positive integer\n");
        return 1;
    }
    size_t last_n = (size_t)n_arg;

    /* Open, stat, then mmap. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "logread: open %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "logread: fstat %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "logread: %s is not a regular file\n", path);
        close(fd);
        return 1;
    }

    size_t file_size = (size_t)st.st_size;

    if (file_size == 0) {
        printf("logread: %s is empty, no records\n", path);
        close(fd);
        return 0;
    }

    /*
     * Map the whole file.  MAP_PRIVATE means writes (which we never do) would
     * be copy-on-write; we only need PROT_READ.  Close fd immediately after —
     * the kernel keeps its own reference until munmap.
     */
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    int saved_errno = errno;
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "logread: mmap %s: %s\n", path, strerror(saved_errno));
        return 1;
    }

    /* Advise sequential access so the kernel can read-ahead efficiently. */
#ifdef MADV_SEQUENTIAL
    madvise(map, file_size, MADV_SEQUENTIAL);
#endif

    const uint8_t *base    = (const uint8_t *)map;
    const uint8_t *cur     = base;
    const uint8_t *map_end = base + file_size;

    /* Per-severity counters. */
    uint64_t sev_counts[256] = {0};
    uint64_t total_records   = 0; /* records with a severity byte */

    /* Matching records for the requested severity. */
    MatchVec matches = {NULL, 0, 0};

    int parse_ok = 1; /* cleared on truncation */

    while (cur < map_end) {
        size_t remaining = (size_t)(map_end - cur);

        /* Validate that we have room for the 4-byte length header. */
        if (remaining < HEADER_SIZE) {
            fprintf(stderr,
                "logread: truncated header at file offset %zu "
                "(%zu byte%s remain, need %u)\n",
                (size_t)(cur - base),
                remaining,
                remaining == 1 ? "" : "s",
                HEADER_SIZE);
            parse_ok = 0;
            break;
        }

        uint32_t body_len = read_u32le(cur);
        cur += HEADER_SIZE;
        remaining -= HEADER_SIZE;

        /* Validate that the full body is mapped. */
        if ((size_t)body_len > remaining) {
            fprintf(stderr,
                "logread: truncated body at file offset %zu "
                "(header claims %" PRIu32 " bytes, only %zu available)\n",
                (size_t)(cur - base),
                body_len,
                remaining);
            parse_ok = 0;
            break;
        }

        /*
         * Only records whose body is at least 1 byte carry a severity value.
         * Zero-length records are silently skipped — they carry no data.
         */
        if (body_len >= MIN_BODY_LEN) {
            uint8_t sev = cur[0];
            sev_counts[sev]++;
            total_records++;

            if (sev == target_sev) {
                if (matchvec_push(&matches, cur, body_len) < 0) {
                    fprintf(stderr, "logread: out of memory\n");
                    free(matches.data);
                    munmap(map, file_size);
                    return 1;
                }
            }
        }

        cur += body_len;
    }

    /* -----------------------------------------------------------------------
     * Print severity summary
     * --------------------------------------------------------------------- */
    printf("=== severity counts ===\n");
    int any_counts = 0;
    for (int i = 0; i < 256; i++) {
        if (sev_counts[i]) {
            printf("  sev %3d : %" PRIu64 " record%s\n",
                   i, sev_counts[i], sev_counts[i] == 1 ? "" : "s");
            any_counts = 1;
        }
    }
    if (!any_counts)
        printf("  (no records with a severity byte found)\n");
    printf("  total   : %" PRIu64 " record%s with severity byte\n\n",
           total_records, total_records == 1 ? "" : "s");

    /* -----------------------------------------------------------------------
     * Print last N records of the requested severity
     * --------------------------------------------------------------------- */
    printf("=== last %zu record%s with severity %u "
           "(%" PRIu64 " total match%s) ===\n",
           last_n,          last_n == 1          ? "" : "s",
           (unsigned)target_sev,
           sev_counts[target_sev],
           sev_counts[target_sev] == 1 ? "" : "es");

    if (matches.count == 0) {
        printf("  (none)\n");
    } else {
        /*
         * start is the index of the first record we print; it is at most
         * (match_count - last_n) so we print at most last_n records.
         */
        size_t start = (matches.count > last_n) ? matches.count - last_n : 0;
        for (size_t i = start; i < matches.count; i++) {
            const uint8_t *body = matches.data[i].body;
            uint32_t       len  = matches.data[i].len;

            /* body[0] = severity, body[1..len-1] = payload */
            printf("  [%zu] sev=%u body_len=%" PRIu32 " payload=",
                   i - start, (unsigned)body[0], len);

            for (uint32_t j = 1; j < len; j++) {
                uint8_t c = body[j];
                if (c >= 0x20 && c < 0x7f)
                    putchar(c);
                else
                    printf("\\x%02x", (unsigned)c);
            }
            putchar('\n');
        }
    }

    /* -----------------------------------------------------------------------
     * Clean up — always reached on the success path; OOM and mmap failures
     * unmap and return early above.
     * --------------------------------------------------------------------- */
    free(matches.data);

    if (munmap(map, file_size) < 0) {
        fprintf(stderr, "logread: munmap: %s\n", strerror(errno));
        return 1;
    }

    return parse_ok ? 0 : 2;
}
