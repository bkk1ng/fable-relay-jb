/* logread.c — memory-mapped binary log scanner
 *
 * Record format (little-endian u32 on the wire):
 *   [uint32_t body_len][body_len bytes of body]
 *   body[0]    = severity (uint8_t, 0-255)
 *   body[1..]  = raw message payload (not NUL-terminated in file)
 *
 * Records with body_len == 0 have no severity byte; they are counted under
 * severity slot 255 and never match a targeted severity < 255.
 *
 * Usage:
 *   logread <logfile> <severity 0-255> <last-N>
 *
 * Compile (C11, POSIX.1-2008):
 *   cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -o logread logread.c
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

/* Symbolic label for a severity byte.  Extend the table as needed. */
static const char *severity_name(uint8_t sev)
{
    switch (sev) {
        case 0:  return "TRACE";
        case 1:  return "DEBUG";
        case 2:  return "INFO";
        case 3:  return "WARN";
        case 4:  return "ERROR";
        case 5:  return "FATAL";
        default: return "UNKNOWN";
    }
}

/* A lightweight reference into the mapped region — no copies. */
typedef struct {
    const uint8_t *body; /* points to body[0] (the severity byte) */
    uint32_t       len;  /* full body length, including the severity byte */
} RecordRef;

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <logfile> <severity 0-255> <last-N>\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];

    /* ---- Parse <severity> ---- */
    char *endp;
    errno = 0;
    long sev_arg = strtol(argv[2], &endp, 10);
    if (errno != 0 || *endp != '\0' || sev_arg < 0 || sev_arg > 255) {
        fprintf(stderr, "logread: severity must be an integer 0-255\n");
        return 1;
    }
    uint8_t target_sev = (uint8_t)sev_arg;

    /* ---- Parse <last-N> ---- */
    errno = 0;
    long n_arg = strtol(argv[3], &endp, 10);
    if (errno != 0 || *endp != '\0' || n_arg < 0) {
        fprintf(stderr, "logread: last-N must be a non-negative integer\n");
        return 1;
    }
    size_t last_n = (size_t)n_arg;

    /* ---- Open and stat the file ---- */
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

    if (st.st_size < 0) {
        fprintf(stderr, "logread: %s: unexpected negative file size\n", path);
        close(fd);
        return 1;
    }
    size_t file_size = (size_t)st.st_size;

    if (file_size == 0) {
        printf("logread: %s is empty — no records\n", path);
        close(fd);
        return 0;
    }

    /* ---- Map the entire file read-only ---- */
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* fd not needed once the mapping is established */

    if (map == MAP_FAILED) {
        fprintf(stderr, "logread: mmap %s: %s\n", path, strerror(errno));
        return 1;
    }

    const uint8_t *base = (const uint8_t *)map;

    /* ---- Allocate the match list ---- */
    size_t match_cap = 256;
    RecordRef *matches = malloc(match_cap * sizeof(RecordRef));
    if (!matches) {
        fprintf(stderr, "logread: malloc: %s\n", strerror(errno));
        munmap(map, file_size);
        return 1;
    }

    /* ---- Single-pass scan ---- */
    uint64_t sev_count[256] = {0}; /* counts per severity byte */
    size_t   total_records  = 0;
    size_t   valid_records  = 0;
    size_t   match_count    = 0;
    size_t   pos            = 0;

    while (pos < file_size) {
        /* Need 4 bytes for the length field. */
        if (file_size - pos < sizeof(uint32_t)) {
            fprintf(stderr,
                "logread: warning: truncated length field at offset %zu "
                "(only %zu byte(s) remain) — stopping scan\n",
                pos, file_size - pos);
            break;
        }

        uint32_t body_len;
        memcpy(&body_len, base + pos, sizeof(uint32_t)); /* avoids alignment UB */
        pos += sizeof(uint32_t);
        total_records++;

        /* Validate that the declared body fits within the mapped region. */
        if ((uint64_t)body_len > (uint64_t)(file_size - pos)) {
            fprintf(stderr,
                "logread: warning: record at offset %zu declares body_len=%" PRIu32
                " but only %zu byte(s) remain — stopping scan\n",
                pos - sizeof(uint32_t), body_len, file_size - pos);
            break;
        }

        valid_records++;

        if (body_len == 0) {
            /* No severity byte; park under slot 255 and move on. */
            sev_count[255]++;
            continue; /* pos already past the 4-byte header; body is empty */
        }

        uint8_t sev = base[pos]; /* first byte of body is the severity */
        sev_count[sev]++;

        if (sev == target_sev) {
            /* Grow the match array when full. */
            if (match_count == match_cap) {
                size_t new_cap = match_cap * 2;
                RecordRef *tmp = realloc(matches, new_cap * sizeof(RecordRef));
                if (!tmp) {
                    fprintf(stderr, "logread: realloc: %s\n", strerror(errno));
                    free(matches);
                    munmap(map, file_size);
                    return 1;
                }
                matches   = tmp;
                match_cap = new_cap;
            }
            matches[match_count].body = base + pos;
            matches[match_count].len  = body_len;
            match_count++;
        }

        pos += body_len;
    }

    /* ---- Report severity counts ---- */
    printf("=== Severity counts (%zu / %zu valid records scanned from %s) ===\n",
           valid_records, total_records, path);
    for (int i = 0; i < 256; i++) {
        if (sev_count[i] > 0) {
            printf("  [%3d] %-8s  %" PRIu64 "\n",
                   i, severity_name((uint8_t)i), sev_count[i]);
        }
    }

    /* ---- Print last N matching records ---- */
    printf("\n=== Last %zu record(s) with severity %u (%s) — "
           "%zu total match(es) ===\n",
           last_n, (unsigned)target_sev, severity_name(target_sev), match_count);

    if (match_count == 0) {
        printf("  (none)\n");
    } else {
        size_t start = (match_count > last_n) ? match_count - last_n : 0;
        for (size_t i = start; i < match_count; i++) {
            /* Payload is everything after the severity byte. */
            const uint8_t *payload = matches[i].body + 1;
            uint32_t       pay_len = matches[i].len  - 1;

            printf("  [%4zu] len=%-6" PRIu32 " |", i - start + 1, pay_len);
            for (uint32_t b = 0; b < pay_len; b++) {
                uint8_t c = payload[b];
                putchar((c >= 0x20 && c < 0x7f) ? (char)c : '.');
            }
            printf("|\n");
        }
    }

    /* ---- Clean up ---- */
    free(matches);
    munmap(map, file_size);
    return 0;
}
