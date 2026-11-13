/*
 * logread.c — memory-mapped binary log scanner
 *
 * Record format (little-endian):
 *   u32  len      — byte length of body (excluding this header)
 *   u8   body[len]
 *
 * Body layout (minimum 1 byte):
 *   body[0]  — severity byte
 *   body[1…] — message payload
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O2 -o logread logread.c
 *
 * Usage:
 *   logread <file> <severity> <last-N>
 *
 *   severity : integer 0–255 to match against body[0]
 *   last-N   : number of matching records to print (from the end)
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

/* ------------------------------------------------------------------ types */

typedef struct {
    const uint8_t *body;   /* pointer into the mapped region */
    uint32_t       len;    /* body length in bytes            */
} Record;

/* ------------------------------------------------------------------ helpers */

/*
 * read_u32_le — read a 4-byte little-endian uint32 from an unaligned pointer.
 * Using a byte-by-byte read avoids undefined behaviour on strict-alignment
 * architectures and satisfies C11 strict aliasing rules.
 */
static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ scanning */

/*
 * scan_records — walk the mapped region and collect every valid record.
 *
 * Returns a heap-allocated array of Record descriptors and writes the count
 * to *out_count.  Returns NULL (with *out_count == 0) if the file is empty
 * or contains no complete records.
 *
 * The caller owns the returned array and must free() it.
 * The body pointers inside each Record remain valid for the lifetime of the
 * mapping.
 */
static Record *scan_records(const uint8_t *map, size_t map_len,
                             size_t *out_count)
{
    *out_count = 0;

    /* Conservative upper bound: even a 1-byte body costs 5 bytes on disk. */
    size_t cap     = (map_len / 5) + 1;
    Record *recs   = malloc(cap * sizeof(Record));
    if (!recs) {
        perror("malloc");
        return NULL;
    }

    size_t count  = 0;
    size_t offset = 0;

    while (offset < map_len) {
        /* Need at least 4 bytes for the length prefix. */
        if (map_len - offset < sizeof(uint32_t)) {
            fprintf(stderr,
                    "warning: %zu trailing byte(s) at offset %zu — truncated header, skipping\n",
                    map_len - offset, offset);
            break;
        }

        uint32_t body_len = read_u32_le(map + offset);
        offset += sizeof(uint32_t);

        /* Validate that the body fits within the remaining mapped region. */
        if (body_len == 0 || (size_t)body_len > map_len - offset) {
            fprintf(stderr,
                    "warning: invalid body length %" PRIu32
                    " at record %zu (offset %zu) — stopping scan\n",
                    body_len, count, offset - sizeof(uint32_t));
            break;
        }

        /* Grow the array if needed (amortised doubling). */
        if (count == cap) {
            size_t  new_cap  = cap * 2;
            Record *new_recs = realloc(recs, new_cap * sizeof(Record));
            if (!new_recs) {
                perror("realloc");
                /* Return what we have so far. */
                *out_count = count;
                return recs;
            }
            recs = new_recs;
            cap  = new_cap;
        }

        recs[count].body = map + offset;
        recs[count].len  = body_len;
        count++;

        offset += body_len;
    }

    *out_count = count;
    return recs;
}

/* ------------------------------------------------------------------ printing */

/*
 * print_record — write one record to stdout.
 *
 * Format:  [<index>] sev=<severity> len=<body_len>  <hex bytes of payload>
 * The severity byte (body[0]) is separated from the message payload (body[1…]).
 */
static void print_record(size_t index, const Record *r)
{
    printf("[%zu] sev=%u len=%" PRIu32 "  payload=",
           index, (unsigned)r->body[0], r->len - 1);

    /* Print payload bytes as hex; cap display at 64 bytes to keep output sane. */
    uint32_t display = r->len - 1;
    int      clipped = 0;
    if (display > 64) {
        display  = 64;
        clipped  = 1;
    }
    for (uint32_t i = 1; i <= display; i++) {
        printf("%02x", r->body[i]);
        if (i < display) putchar(' ');
    }
    if (clipped) printf(" …");
    putchar('\n');
}

/* ------------------------------------------------------------------ entry */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <file> <severity> <last-N>\n"
            "  severity  0-255 severity byte to filter on\n"
            "  last-N    number of matching records to print (0 = all)\n",
            prog);
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];

    /* Parse severity. */
    char    *end;
    long     sev_long = strtol(argv[2], &end, 10);
    if (*end != '\0' || sev_long < 0 || sev_long > 255) {
        fprintf(stderr, "error: severity must be an integer 0–255\n");
        return EXIT_FAILURE;
    }
    uint8_t severity = (uint8_t)sev_long;

    /* Parse last-N. */
    long n_long = strtol(argv[3], &end, 10);
    if (*end != '\0' || n_long < 0) {
        fprintf(stderr, "error: last-N must be a non-negative integer\n");
        return EXIT_FAILURE;
    }
    size_t last_n = (size_t)n_long;

    /* Open the log file. */
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Stat to get file size. */
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    if (st.st_size == 0) {
        printf("file is empty\n");
        close(fd);
        return EXIT_SUCCESS;
    }

    size_t file_size = (size_t)st.st_size;

    /* Map the whole file read-only. */
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    /* fd is no longer needed once the mapping is established. */
    close(fd);
    fd = -1;

    /* Advise sequential access so the kernel can prefetch efficiently. */
    madvise(map, file_size, MADV_SEQUENTIAL);

    /* ---- scan ---- */
    size_t  total;
    Record *all = scan_records((const uint8_t *)map, file_size, &total);
    if (!all && total == 0) {
        munmap(map, file_size);
        return EXIT_FAILURE;
    }

    /* ---- count by severity ---- */
    /* We need a full pass to count every severity. Use a 256-bucket histogram
     * stored as size_t to avoid overflow on very large files. */
    size_t sev_count[256] = {0};
    for (size_t i = 0; i < total; i++) {
        sev_count[all[i].body[0]]++;
    }

    printf("total records   : %zu\n", total);
    printf("severity counts :\n");
    for (int s = 0; s < 256; s++) {
        if (sev_count[s] > 0)
            printf("  sev %3d → %zu\n", s, sev_count[s]);
    }
    putchar('\n');

    /* ---- collect matching records ---- */
    size_t  match_cap  = sev_count[severity] + 1; /* +1 avoids zero-alloc */
    Record *matches    = malloc(match_cap * sizeof(Record));
    if (!matches) {
        perror("malloc");
        free(all);
        munmap(map, file_size);
        return EXIT_FAILURE;
    }

    size_t match_count = 0;
    for (size_t i = 0; i < total; i++) {
        if (all[i].body[0] == severity) {
            matches[match_count++] = all[i];
        }
    }

    free(all);
    all = NULL;

    printf("records with severity %u : %zu\n", (unsigned)severity, match_count);

    if (match_count == 0) {
        free(matches);
        munmap(map, file_size);
        return EXIT_SUCCESS;
    }

    /* ---- print last N ---- */
    size_t start = 0;
    if (last_n > 0 && last_n < match_count)
        start = match_count - last_n;

    printf("printing %zu record(s) (indices %zu–%zu):\n\n",
           match_count - start, start, match_count - 1);

    for (size_t i = start; i < match_count; i++)
        print_record(i, &matches[i]);

    /* ---- clean up on every path ---- */
    free(matches);
    munmap(map, file_size);

    return EXIT_SUCCESS;
}
