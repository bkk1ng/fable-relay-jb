/*
 * binfmt.c — little-endian binary container parser (C11)
 *
 * Header layout (16 bytes, offsets from byte 0):
 *   [0..3]   magic        u32 LE  must be 0x4C425931
 *   [4..5]   version      u16 LE  must be non-zero
 *   [6..7]   flags        u16 LE
 *   [8..11]  record_count u32 LE
 *   [12..15] record_size  u32 LE  bytes per record, must be >= 12
 *
 * Record layout (record_size bytes each, record table follows header):
 *   [0..3]   id      u32 LE
 *   [4..7]   offset  u32 LE  byte offset into file
 *   [8..11]  length  u32 LE  byte length of referenced data
 *
 * Returns 0 on success, non-zero on I/O error or any corruption.
 *
 * Build:  cc -std=c11 -Wall -Wextra -Wpedantic -o binfmt binfmt.c
 * Usage:  ./binfmt <file>
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAGIC        UINT32_C(0x4C425931)
#define HEADER_BYTES UINT32_C(16)  /* fixed header size                */
#define MIN_REC_SIZE UINT32_C(12)  /* id(4) + offset(4) + length(4)   */

/* ------------------------------------------------------------------ */
/* Endian-safe read helpers                                            */
/*                                                                     */
/* Each function assembles the value from individual bytes so the     */
/* result is correct on any host byte order with no UB.               */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)(  (uint32_t)p[0]
                      | ((uint32_t)p[1] <<  8)
                      | ((uint32_t)p[2] << 16)
                      | ((uint32_t)p[3] << 24));
}

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t record_count;
    uint32_t record_size;
} Header;

typedef struct {
    uint32_t id;
    uint32_t offset;
    uint32_t length;
} Record;

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Decode the fixed 16-byte header from buf and validate magic,
 * version, record_size, and whether the record table fits in the
 * file.  Returns 0 on success, -1 with a message on failure.
 */
static int parse_header(const uint8_t *buf, size_t file_size, Header *out)
{
    if (file_size < HEADER_BYTES) {
        fprintf(stderr,
                "error: file too small for header "
                "(need %u bytes, have %zu)\n",
                HEADER_BYTES, file_size);
        return -1;
    }

    out->magic        = rd_u32le(buf +  0);
    out->version      = rd_u16le(buf +  4);
    out->flags        = rd_u16le(buf +  6);
    out->record_count = rd_u32le(buf +  8);
    out->record_size  = rd_u32le(buf + 12);

    /* magic */
    if (out->magic != MAGIC) {
        fprintf(stderr,
                "error: bad magic 0x%08X (expected 0x%08X)\n",
                out->magic, MAGIC);
        return -1;
    }

    /* version: 0 is reserved; unknown future versions are accepted  */
    if (out->version == 0) {
        fprintf(stderr, "error: version 0 is reserved\n");
        return -1;
    }

    /* record_size must accommodate the three known fixed fields      */
    if (out->record_size < MIN_REC_SIZE) {
        fprintf(stderr,
                "error: record_size %u is below minimum %u\n",
                out->record_size, MIN_REC_SIZE);
        return -1;
    }

    /*
     * Bounds check: header + record table must fit entirely in the
     * file.  Use uint64_t for the product to avoid 32-bit overflow.
     */
    uint64_t table_bytes = (uint64_t)out->record_count * out->record_size;
    uint64_t needed      = (uint64_t)HEADER_BYTES + table_bytes;
    if (needed > (uint64_t)file_size) {
        fprintf(stderr,
                "error: record table needs %llu bytes "
                "but file is only %zu bytes\n",
                (unsigned long long)needed, file_size);
        return -1;
    }

    return 0;
}

/* Decode one record from a pointer to its first byte. */
static void parse_record(const uint8_t *p, Record *out)
{
    out->id     = rd_u32le(p + 0);
    out->offset = rd_u32le(p + 4);
    out->length = rd_u32le(p + 8);
}

/* ------------------------------------------------------------------ */
/* Record validation                                                   */
/* ------------------------------------------------------------------ */

/*
 * Verify that every record's data window [offset, offset+length)
 * lies within the file.  Checks all records even after the first bad
 * one so every violation is reported.  Returns 0 if all are valid,
 * -1 if any are corrupt.
 */
static int validate_records(const uint8_t *buf, const Header *h,
                             size_t file_size)
{
    const uint8_t *table  = buf + HEADER_BYTES;
    int            result = 0;

    for (uint32_t i = 0; i < h->record_count; i++) {
        Record r;
        parse_record(table + (uint64_t)i * h->record_size, &r);

        uint64_t end = (uint64_t)r.offset + (uint64_t)r.length;
        if (end > (uint64_t)file_size) {
            fprintf(stderr,
                    "error: record %u (id=0x%08X): "
                    "data window [%u, %llu) exceeds file size %zu\n",
                    i, r.id, r.offset,
                    (unsigned long long)end, file_size);
            result = -1;
        }
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void print_header(const Header *h)
{
    printf("magic:        0x%08X\n", h->magic);
    printf("version:      %u\n",     h->version);
    printf("flags:        0x%04X\n", h->flags);
    printf("record_count: %u\n",     h->record_count);
    printf("record_size:  %u\n",     h->record_size);
    putchar('\n');
}

static void print_records(const uint8_t *buf, const Header *h)
{
    const uint8_t *table = buf + HEADER_BYTES;

    printf("%-8s  %-12s  %-12s  %-12s\n",
           "INDEX", "ID", "OFFSET", "LENGTH");
    printf("%-8s  %-12s  %-12s  %-12s\n",
           "--------", "------------", "------------", "------------");

    for (uint32_t i = 0; i < h->record_count; i++) {
        Record r;
        parse_record(table + (uint64_t)i * h->record_size, &r);
        printf("%-8u  0x%08X    %-12u  %-12u\n",
               i, r.id, r.offset, r.length);
    }

    if (h->record_count == 0)
        printf("(no records)\n");
}

/* ------------------------------------------------------------------ */
/* File I/O                                                            */
/* ------------------------------------------------------------------ */

/*
 * Read the entire file at path into a malloc'd buffer.
 * Sets *size to the file length on success.
 * Returns NULL on any error (message already printed).
 */
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "error: fseek on '%s': %s\n",
                path, strerror(errno));
        fclose(f);
        return NULL;
    }

    long raw_len = ftell(f);
    if (raw_len < 0) {
        fprintf(stderr, "error: ftell on '%s': %s\n",
                path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    size_t len = (size_t)raw_len;

    /* Allocate at least 1 byte so malloc(0) is never called.         */
    uint8_t *buf = malloc(len > 0 ? len : 1);
    if (!buf) {
        fprintf(stderr, "error: out of memory\n");
        fclose(f);
        return NULL;
    }

    if (len > 0 && fread(buf, 1, len, f) != len) {
        fprintf(stderr, "error: read failed on '%s': %s\n",
                path, strerror(errno));
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size = len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    size_t   file_size = 0;
    uint8_t *buf       = read_file(argv[1], &file_size);
    if (!buf)
        return 1;

    Header h;
    if (parse_header(buf, file_size, &h) != 0) {
        free(buf);
        return 1;
    }

    print_header(&h);

    /*
     * Validate all record bounds before printing so corruption notices
     * appear above the table, making them easy to spot.
     */
    int corrupt = validate_records(buf, &h, file_size);
    print_records(buf, &h);

    free(buf);
    return (corrupt != 0) ? 1 : 0;
}
