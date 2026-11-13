/*
 * logread.c — memory-mapped binary log scanner (C11, POSIX)
 *
 * Record wire format (native endian):
 *   [uint32_t body_len][uint8_t severity][char message[body_len - 1]]
 *
 * body_len counts the severity byte plus the message bytes.
 * A body_len of 1 is a severity-only record with an empty message.
 *
 * Severities: 0=EMERG 1=ALERT 2=CRIT 3=ERR 4=WARN 5=NOTICE 6=INFO 7=DEBUG
 *
 * Usage: logread <file> <severity 0-7> <last_N>
 *
 * Build: cc -std=c11 -Wall -Wextra -o logread logread.c
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

#define SEV_COUNT    8
#define HDR_SIZE     sizeof(uint32_t)
#define MIN_BODY_LEN 1   /* severity byte is mandatory */

static const char *const sev_name[SEV_COUNT] = {
    "EMERG", "ALERT", "CRIT", "ERR", "WARN", "NOTICE", "INFO", "DEBUG"
};

/* Lightweight reference into the mapped region — no copies. */
typedef struct {
    const uint8_t *body;   /* points at the severity byte inside the map */
    uint32_t       len;    /* body_len from the record header */
} record_ref;

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <logfile> <severity 0-%d> <last_N>\n",
            prog, SEV_COUNT - 1);
    for (int i = 0; i < SEV_COUNT; i++)
        fprintf(stderr, "  %d = %s\n", i, sev_name[i]);
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];

    /* -- parse severity ------------------------------------------------- */
    char *endp;
    errno = 0;
    long sev_arg = strtol(argv[2], &endp, 10);
    if (errno || *endp != '\0' || sev_arg < 0 || sev_arg >= SEV_COUNT) {
        fprintf(stderr, "logread: severity must be 0–%d\n", SEV_COUNT - 1);
        return 1;
    }
    int target_sev = (int)sev_arg;

    /* -- parse last_N --------------------------------------------------- */
    errno = 0;
    long last_n = strtol(argv[3], &endp, 10);
    if (errno || *endp != '\0' || last_n <= 0) {
        fprintf(stderr, "logread: last_N must be a positive integer\n");
        return 1;
    }

    /* -- open + stat ----------------------------------------------------- */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }

    if (st.st_size == 0) {
        fprintf(stderr, "%s: empty file\n", path);
        close(fd);
        return 0;
    }

    size_t fsize = (size_t)st.st_size;

    /* -- mmap ------------------------------------------------------------ */
    void *map = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);   /* fd is no longer needed once the mapping exists */
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    const uint8_t *data = (const uint8_t *)map;

    /* -- dynamic array for matching record refs (avoids a second pass) -- */
    size_t      match_cap = 64;
    size_t      match_cnt = 0;
    record_ref *matches   = malloc(match_cap * sizeof(*matches));
    if (!matches) {
        perror("malloc");
        munmap(map, fsize);
        return 1;
    }

    /* -- first (and only) pass ------------------------------------------ */
    size_t sev_counts[SEV_COUNT] = {0};
    size_t truncated             = 0;
    size_t offset                = 0;

    while (offset < fsize) {

        /* need at least 4 bytes for the length header */
        if (fsize - offset < HDR_SIZE) {
            truncated++;
            break;
        }

        uint32_t body_len;
        memcpy(&body_len, data + offset, HDR_SIZE);
        offset += HDR_SIZE;

        /* body must carry at least the severity byte */
        if (body_len < MIN_BODY_LEN) {
            fprintf(stderr,
                    "logread: zero-length body at file offset %zu — stopping\n",
                    offset - HDR_SIZE);
            break;
        }

        /* body must fit entirely within the mapped region */
        if (body_len > fsize - offset) {
            truncated++;
            break;
        }

        uint8_t sev = data[offset];   /* first byte of body is severity */

        if (sev < SEV_COUNT) {
            sev_counts[sev]++;
        }
        /* unknown severity values are skipped in the tally but still
         * advance the cursor so scanning can continue. */

        /* collect a reference for records that match the requested sev */
        if ((int)sev == target_sev) {
            if (match_cnt == match_cap) {
                size_t      new_cap = match_cap * 2;
                record_ref *tmp     = realloc(matches,
                                              new_cap * sizeof(*matches));
                if (!tmp) {
                    perror("realloc");
                    free(matches);
                    munmap(map, fsize);
                    return 1;
                }
                matches   = tmp;
                match_cap = new_cap;
            }
            matches[match_cnt].body = data + offset;
            matches[match_cnt].len  = body_len;
            match_cnt++;
        }

        offset += body_len;
    }

    /* -- severity summary ----------------------------------------------- */
    printf("=== severity counts ===\n");
    for (int i = 0; i < SEV_COUNT; i++)
        printf("  %-6s (%d): %zu\n", sev_name[i], i, sev_counts[i]);
    if (truncated)
        printf("  (truncated/partial records at EOF: %zu)\n", truncated);
    printf("\n");

    /* -- last N matching records ---------------------------------------- */
    size_t start = (match_cnt > (size_t)last_n)
                   ? match_cnt - (size_t)last_n
                   : 0;

    printf("=== last %ld %s record(s) — %zu found ===\n",
           last_n, sev_name[target_sev], match_cnt);

    for (size_t i = start; i < match_cnt; i++) {
        const uint8_t *body    = matches[i].body;
        uint32_t       blen    = matches[i].len;
        /* body[0] = severity; body[1 .. blen-1] = message bytes.
         * The message is NOT required to be NUL-terminated, so we print
         * exactly blen-1 bytes with a width-limited %.*s. */
        uint32_t    msg_len = blen - 1;
        const char *msg     = (const char *)(body + 1);
        printf("[%s] %.*s\n", sev_name[target_sev], (int)msg_len, msg);
    }

    /* -- clean teardown on the normal path ------------------------------ */
    free(matches);
    munmap(map, fsize);
    return 0;
}
