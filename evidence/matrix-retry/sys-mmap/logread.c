/*
 * logread.c — memory-mapped binary log scanner
 *
 * Record layout (little-endian u32, then body):
 *
 *   ┌──────────────────┬──────────┬────────────────────────────┐
 *   │ body_len: u32 LE │ sev: u8  │ message: body_len-1 bytes  │
 *   └──────────────────┴──────────┴────────────────────────────┘
 *
 *   body_len counts the severity byte, so body_len >= 1 always.
 *   body_len == 0 is treated as a corrupt record and aborts the scan.
 *
 * Usage:
 *   logread <file> <severity 0-255> <last_n>
 *
 *   Prints a per-severity count table, then the last <last_n> records
 *   whose severity byte equals <severity>.
 *
 * Compile:
 *   cc -std=c11 -Wall -Wextra -o logread logread.c
 *
 * Quick test-data writer (shell):
 *   python3 -c "
 *   import struct, sys
 *   data = [(2,'server started'),(1,'debug init'),(4,'disk full'),
 *           (2,'request ok'),(4,'out of memory'),(2,'graceful shutdown')]
 *   for sev,msg in data:
 *       b = bytes([sev]) + msg.encode()
 *       sys.stdout.buffer.write(struct.pack('<I', len(b)) + b)
 *   " > sample.log
 *   ./logread sample.log 4 10
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

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define HEADER_BYTES  4u   /* sizeof the u32 body_len prefix          */
#define MIN_BODY_LEN  1u   /* body must hold at least the sev byte    */

/* Initial capacity of the per-severity match pointer array. Doubles   */
/* on overflow.                                                         */
#define MATCH_INIT_CAP 128u

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

/*
 * A lightweight view into the mapped region.  `body` points at the
 * severity byte; `len` is the full body length (severity + message).
 * No copying — this is just a pointer + length into the mapping.
 */
typedef struct {
    const uint8_t *body;
    uint32_t       len;
} RecordView;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Read a u32 from an unaligned byte pointer, little-endian.           */
static uint32_t u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static const char *sev_label(uint8_t s)
{
    switch (s) {
        case 0: return "TRACE";
        case 1: return "DEBUG";
        case 2: return "INFO";
        case 3: return "WARN";
        case 4: return "ERROR";
        case 5: return "FATAL";
        default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Scan                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Walk every record in [map, map+file_size).
 *
 * Fills:
 *   sev_count[0..255]  — number of records per severity value
 *   *out_matches       — heap-allocated array of RecordView for target_sev
 *   *out_match_count   — length of that array
 *
 * Returns 0 on success, -1 on allocation failure.
 * Partial results (truncated file, corrupt length) are reported to
 * stderr and the scan stops at the first unrecoverable record.
 */
static int scan(const uint8_t *map,
                size_t         file_size,
                uint8_t        target_sev,
                uint64_t       sev_count[256],
                RecordView   **out_matches,
                size_t        *out_match_count)
{
    size_t      match_cap   = MATCH_INIT_CAP;
    size_t      match_count = 0;
    RecordView *matches     = malloc(match_cap * sizeof *matches);
    if (!matches) {
        perror("malloc");
        return -1;
    }

    size_t offset = 0;

    while (offset + HEADER_BYTES <= file_size) {

        /* ---- read the length prefix -------------------------------- */
        uint32_t body_len = u32_le(map + offset);

        /* ---- validate -------------------------------------------- */
        if (body_len < MIN_BODY_LEN) {
            fprintf(stderr,
                    "logread: corrupt record at byte %zu: "
                    "body_len=%" PRIu32 " (minimum %u), aborting scan\n",
                    offset, body_len, MIN_BODY_LEN);
            break;
        }

        /* Overflow-safe remaining-bytes check.                        */
        size_t remaining = file_size - offset - HEADER_BYTES;
        if ((size_t)body_len > remaining) {
            fprintf(stderr,
                    "logread: truncated record at byte %zu: "
                    "body_len=%" PRIu32 " but only %zu byte(s) remain, "
                    "aborting scan\n",
                    offset, body_len, remaining);
            break;
        }

        /* ---- extract severity ------------------------------------- */
        const uint8_t *body = map + offset + HEADER_BYTES;
        uint8_t sev = body[0];

        sev_count[sev]++;

        /* ---- collect match ---------------------------------------- */
        if (sev == target_sev) {
            if (match_count == match_cap) {
                size_t      new_cap = match_cap * 2;
                RecordView *tmp     = realloc(matches, new_cap * sizeof *tmp);
                if (!tmp) {
                    perror("realloc");
                    free(matches);
                    return -1;
                }
                matches   = tmp;
                match_cap = new_cap;
            }
            matches[match_count].body = body;
            matches[match_count].len  = body_len;
            match_count++;
        }

        offset += HEADER_BYTES + (size_t)body_len;
    }

    *out_matches      = matches;
    *out_match_count  = match_count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <file> <severity 0-255> <last_n>\n"
                "\n"
                "  file       binary log file\n"
                "  severity   byte value to filter on (0=TRACE 1=DEBUG\n"
                "             2=INFO 3=WARN 4=ERROR 5=FATAL)\n"
                "  last_n     number of tail records to print\n",
                argv[0]);
        return 1;
    }

    /* ---- argument parsing ---------------------------------------- */
    const char *path = argv[1];

    char *end;
    errno = 0;
    long sev_arg = strtol(argv[2], &end, 10);
    if (errno || *end != '\0' || sev_arg < 0 || sev_arg > 255) {
        fprintf(stderr, "logread: severity must be an integer 0-255\n");
        return 1;
    }
    uint8_t target_sev = (uint8_t)sev_arg;

    errno = 0;
    long last_n = strtol(argv[3], &end, 10);
    if (errno || *end != '\0' || last_n < 0) {
        fprintf(stderr, "logread: last_n must be a non-negative integer\n");
        return 1;
    }

    /* ---- open and stat ------------------------------------------- */
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("logread: open");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("logread: fstat");
        close(fd);
        return 1;
    }

    if (st.st_size < 0) {
        fprintf(stderr, "logread: unexpected negative st_size\n");
        close(fd);
        return 1;
    }

    size_t file_size = (size_t)st.st_size;

    if (file_size == 0) {
        close(fd);
        printf("logread: file is empty — 0 records\n");
        return 0;
    }

    /* ---- mmap ---------------------------------------------------- */
    const uint8_t *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

    /*
     * The fd is no longer needed once the mapping is established.
     * The mapping itself keeps the file pages alive.
     */
    close(fd);

    if (map == MAP_FAILED) {
        perror("logread: mmap");
        return 1;
    }

    /* Hint to the kernel that we will read sequentially.              */
    (void)madvise((void *)map, file_size, MADV_SEQUENTIAL);

    /* ---- scan ---------------------------------------------------- */
    uint64_t    sev_count[256] = {0};
    RecordView *matches        = NULL;
    size_t      match_count    = 0;

    int rc = scan(map, file_size, target_sev,
                  sev_count, &matches, &match_count);
    if (rc != 0) {
        /* scan already printed the reason; unmap and exit cleanly.   */
        munmap((void *)map, file_size);
        return 1;
    }

    /* ---- severity count table ------------------------------------ */
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) total += sev_count[i];

    printf("total records: %" PRIu64 "\n\nseverity counts:\n", total);
    for (int i = 0; i < 256; i++) {
        if (sev_count[i]) {
            printf("  [%3d] %-5s  %" PRIu64 "\n",
                   i, sev_label((uint8_t)i), sev_count[i]);
        }
    }

    /* ---- last-N printout ----------------------------------------- */
    printf("\nlast %ld record(s) with severity %d (%s)  "
           "[%" PRIu64 " total match(es)]:\n",
           last_n, target_sev, sev_label(target_sev), sev_count[target_sev]);

    if (match_count == 0 || last_n == 0) {
        printf("  (none)\n");
    } else {
        /*
         * start is the index of the first record to print.
         * Both values are size_t so the subtraction is safe after the
         * last_n == 0 guard above.
         */
        size_t n    = (size_t)last_n;
        size_t start = (n >= match_count) ? 0 : match_count - n;

        for (size_t i = start; i < match_count; i++) {
            const RecordView *r       = &matches[i];
            uint32_t          msg_len = r->len - 1;   /* strip sev byte */

            /*
             * %.*s bounds the output to msg_len bytes without requiring
             * a null terminator in the mapped data.
             */
            printf("  [%4zu] sev=%d(%-5s)  msg[%" PRIu32 "]: %.*s\n",
                   i + 1,
                   r->body[0], sev_label(r->body[0]),
                   msg_len,
                   (int)msg_len, (const char *)(r->body + 1));
        }
    }

    /* ---- clean up every exit path -------------------------------- */
    free(matches);
    munmap((void *)map, file_size);
    return 0;
}
