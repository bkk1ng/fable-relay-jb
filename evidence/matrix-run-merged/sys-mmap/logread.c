/*
 * logread.c — memory-mapped log scanner
 * C11 / POSIX (200809L)
 *
 * Record format (little-endian):
 *   [ uint32_t body_len ][ body: severity(1 byte) + payload(body_len-1 bytes) ]
 *
 * Usage:
 *   logread [-n count] [-s severity] <logfile>
 *
 *   -n count     print the last <count> matching records (default: 10)
 *   -s severity  filter by severity byte 0-255 (omit for all severities)
 *
 * Exit codes:
 *   0  success, all records well-formed
 *   1  usage / argument error
 *   2  I/O, system, or malformed-record error
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O2 -o logread logread.c
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

#define DEFAULT_TAIL 10

/* -------------------------------------------------------------------------
 * Ring buffer — retains references to the last N records seen so we never
 * materialise more than N entries in memory regardless of file size.
 * ----------------------------------------------------------------------- */

typedef struct {
    size_t   file_off;  /* byte offset of the uint32_t len field */
    uint32_t body_len;  /* value of that length field             */
} rec_ref_t;

typedef struct {
    rec_ref_t *slots;
    size_t     cap;
    size_t     head;   /* index of the oldest slot */
    size_t     count;  /* number of valid slots (<= cap) */
} ring_t;

static int ring_init(ring_t *r, size_t cap)
{
    r->slots = malloc(cap * sizeof *r->slots);
    if (!r->slots) return -1;
    r->cap   = cap;
    r->head  = 0;
    r->count = 0;
    return 0;
}

static void ring_push(ring_t *r, rec_ref_t ref)
{
    size_t idx = (r->head + r->count) % r->cap;
    r->slots[idx] = ref;
    if (r->count < r->cap) {
        r->count++;
    } else {
        /* ring full — drop oldest */
        r->head = (r->head + 1) % r->cap;
    }
}

/* Return the i-th entry in insertion order (0 = oldest). */
static rec_ref_t ring_get(const ring_t *r, size_t i)
{
    return r->slots[(r->head + i) % r->cap];
}

static void ring_free(ring_t *r)
{
    free(r->slots);
    r->slots = NULL;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Portable unaligned little-endian uint32 read — avoids UB on strict-
 * alignment architectures and passes -fsanitize=undefined cleanly.      */
static uint32_t read_u32le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void die(const char *msg)
{
    perror(msg);
    exit(2);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-n count] [-s severity] <logfile>\n"
        "  -n count     last N matching records to print (default %d)\n"
        "  -s severity  severity byte 0-255 (default: all)\n",
        argv0, DEFAULT_TAIL);
    exit(1);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    long    tail_n   = DEFAULT_TAIL;
    int     filter   = 0;
    uint8_t sev_want = 0;
    int     opt;

    while ((opt = getopt(argc, argv, "n:s:")) != -1) {
        switch (opt) {
        case 'n': {
            char *end;
            errno  = 0;
            tail_n = strtol(optarg, &end, 10);
            if (errno || *end != '\0' || tail_n <= 0) {
                fprintf(stderr, "logread: -n requires a positive integer\n");
                usage(argv[0]);
            }
            break;
        }
        case 's': {
            char *end;
            errno  = 0;
            long v = strtol(optarg, &end, 0);  /* 0x prefix accepted */
            if (errno || *end != '\0' || v < 0 || v > 255) {
                fprintf(stderr, "logread: -s requires an integer 0-255\n");
                usage(argv[0]);
            }
            sev_want = (uint8_t)v;
            filter   = 1;
            break;
        }
        default:
            usage(argv[0]);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "logread: missing <logfile>\n");
        usage(argv[0]);
    }

    const char *path = argv[optind];

    /* ---- open and stat ---- */
    int fd = open(path, O_RDONLY);
    if (fd == -1) die("open");

    struct stat st;
    if (fstat(fd, &st) == -1) { close(fd); die("fstat"); }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "logread: %s: not a regular file\n", path);
        close(fd);
        exit(2);
    }

    size_t file_size = (size_t)st.st_size;

    if (file_size == 0) {
        close(fd);
        printf("logread: %s: file is empty\n", path);
        return 0;
    }

    /* ---- mmap ---- */
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  /* fd no longer needed after a successful mmap */
    if (map == MAP_FAILED) die("mmap");

    /* Hint sequential read-ahead; non-fatal if unsupported. */
#ifdef POSIX_MADV_SEQUENTIAL
    (void)posix_madvise(map, file_size, POSIX_MADV_SEQUENTIAL);
#elif defined(MADV_SEQUENTIAL)
    (void)madvise(map, file_size, MADV_SEQUENTIAL);
#endif

    const uint8_t *base = (const uint8_t *)map;

    /* ---- allocate ring buffer ---- */
    ring_t ring;
    if (ring_init(&ring, (size_t)tail_n) != 0) {
        munmap(map, file_size);
        die("malloc");
    }

    /* ---- scan ---- */
    uint64_t sev_counts[256] = {0};
    uint64_t total_records   = 0;
    uint64_t bad_records     = 0;
    size_t   pos             = 0;

    while (pos < file_size) {

        /* Bounds: need 4 bytes for the length field. */
        if (pos + sizeof(uint32_t) > file_size) {
            fprintf(stderr,
                "logread: truncated length field at offset %zu "
                "(%zu byte%s remaining)\n",
                pos,
                file_size - pos,
                (file_size - pos) == 1 ? "" : "s");
            bad_records++;
            break;
        }

        uint32_t body_len = read_u32le(base + pos);

        /* A zero-length body has no severity byte — skip the record. */
        if (body_len == 0) {
            fprintf(stderr,
                "logread: zero-length record at offset %zu, skipping\n", pos);
            bad_records++;
            pos += sizeof(uint32_t);
            continue;
        }

        /*
         * Check that the full record fits without wrapping.
         * sizeof(uint32_t) + body_len cannot overflow size_t: file_size
         * itself fits in size_t, so any value larger than file_size is
         * caught by the second condition.
         */
        size_t header_end = pos + sizeof(uint32_t);
        size_t record_end = header_end + (size_t)body_len;

        if (record_end < header_end          /* size_t wrap */
         || record_end > file_size) {
            fprintf(stderr,
                "logread: record at offset %zu claims %" PRIu32 " body bytes "
                "but only %zu remain after the length field\n",
                pos, body_len,
                file_size - header_end);
            bad_records++;
            break;  /* cannot safely advance; stream is corrupt */
        }

        total_records++;
        uint8_t sev = base[header_end];   /* first byte of body = severity */
        sev_counts[sev]++;

        if (!filter || sev == sev_want) {
            rec_ref_t ref = { .file_off = pos, .body_len = body_len };
            ring_push(&ring, ref);
        }

        pos = record_end;
    }

    /* ---- summary ---- */
    printf("file    : %s\n", path);
    printf("size    : %zu bytes\n", file_size);
    printf("records : %" PRIu64 " valid", total_records);
    if (bad_records)
        printf(", %" PRIu64 " malformed", bad_records);
    putchar('\n');

    printf("by severity:\n");
    int any = 0;
    for (int i = 0; i < 256; i++) {
        if (sev_counts[i]) {
            printf("  [%3d]  %" PRIu64 "\n", i, sev_counts[i]);
            any = 1;
        }
    }
    if (!any) printf("  (no records)\n");

    /* ---- last N records ---- */
    putchar('\n');
    if (filter)
        printf("last %" PRIu64 " / %" PRIu64 " record%s  severity=%d:\n",
               (uint64_t)ring.count,
               sev_counts[sev_want],
               sev_counts[sev_want] == 1 ? "" : "s",
               sev_want);
    else
        printf("last %" PRIu64 " record%s  (all severities):\n",
               (uint64_t)ring.count,
               ring.count == 1 ? "" : "s");

    for (size_t i = 0; i < ring.count; i++) {
        rec_ref_t      ref     = ring_get(&ring, i);
        const uint8_t *body    = base + ref.file_off + sizeof(uint32_t);
        uint8_t        sev     = body[0];
        const uint8_t *payload = body + 1;
        uint32_t       plen    = ref.body_len - 1;

        printf("  offset=%8zu  sev=%3d  len=%6" PRIu32 "  |",
               ref.file_off, sev, ref.body_len);

        /* Render printable ASCII directly; escape everything else. */
        for (uint32_t j = 0; j < plen; j++) {
            uint8_t c = payload[j];
            if (c >= 0x20 && c < 0x7f)
                putchar(c);
            else
                printf("\\x%02x", c);
        }
        printf("|\n");
    }

    /* ---- clean up ---- */
    ring_free(&ring);
    if (munmap(map, file_size) == -1) die("munmap");

    return bad_records ? 2 : 0;
}
