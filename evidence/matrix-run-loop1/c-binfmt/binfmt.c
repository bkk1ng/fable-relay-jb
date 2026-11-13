/* binfmt.c — little-endian binary container parser (C11, single file)
 *
 * Header layout (all fields little-endian, 16 bytes total):
 *   u32  magic        must equal 0x4C425931
 *   u16  version
 *   u16  flags
 *   u32  record_count
 *   u32  record_size  stride in bytes; must be >= 12
 *
 * Record layout (little-endian, minimum 12 bytes):
 *   u32  id
 *   u32  offset       file-absolute byte offset of the payload
 *   u32  length       byte length of the payload
 *
 * Usage:  binfmt <file>
 * Exit:   0 on success, non-zero on corruption or usage error.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Endian-safe read helpers (no aliasing, no platform assumption)       */
/* ------------------------------------------------------------------ */

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)( (uint16_t)p[0]
                     | (uint16_t)((uint16_t)p[1] << 8) );
}

static uint32_t rd32le(const uint8_t *p)
{
    return   (uint32_t)p[0]
           | ((uint32_t)p[1] <<  8)
           | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Named constants                                                      */
/* ------------------------------------------------------------------ */

#define MAGIC        UINT32_C(0x4C425931)
#define HEADER_SIZE  16u   /* 4+2+2+4+4 */
#define RECORD_FIXED 12u   /* id(4)+offset(4)+length(4) */

/* ------------------------------------------------------------------ */
/* Structs                                                              */
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
/* File I/O                                                             */
/* ------------------------------------------------------------------ */

/* Slurp the entire file into a heap buffer; set *out_size.
 * Returns NULL on failure (message already emitted to stderr). */
static uint8_t *slurp(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "error: seek failed on '%s': %s\n",
                path, strerror(errno));
        fclose(f);
        return NULL;
    }

    long raw = ftell(f);
    if (raw < 0) {
        fprintf(stderr, "error: tell failed on '%s': %s\n",
                path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    size_t  sz  = (size_t)raw;
    uint8_t *buf = malloc(sz ? sz : 1); /* avoid malloc(0) edge case */
    if (!buf) {
        fprintf(stderr, "error: out of memory\n");
        fclose(f);
        return NULL;
    }

    if (sz > 0 && fread(buf, 1, sz, f) != sz) {
        fprintf(stderr, "error: short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = sz;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                              */
/* ------------------------------------------------------------------ */

static int parse_header(const uint8_t *buf, size_t fsz, Header *h)
{
    if (fsz < HEADER_SIZE) {
        fprintf(stderr,
                "corrupt: file is %zu bytes; minimum for a valid header is %u\n",
                fsz, HEADER_SIZE);
        return -1;
    }

    h->magic        = rd32le(buf + 0);
    h->version      = rd16le(buf + 4);
    h->flags        = rd16le(buf + 6);
    h->record_count = rd32le(buf + 8);
    h->record_size  = rd32le(buf + 12);

    if (h->magic != MAGIC) {
        fprintf(stderr,
                "corrupt: magic 0x%08X does not match expected 0x%08X\n",
                h->magic, MAGIC);
        return -1;
    }

    if (h->version == 0) {
        fprintf(stderr, "corrupt: version field is zero\n");
        return -1;
    }

    if (h->record_size < RECORD_FIXED) {
        fprintf(stderr,
                "corrupt: record_size %u is below the minimum of %u bytes\n",
                h->record_size, RECORD_FIXED);
        return -1;
    }

    /* Use u64 arithmetic to avoid overflow on the multiplication. */
    uint64_t need = (uint64_t)HEADER_SIZE
                  + (uint64_t)h->record_count * h->record_size;
    if (need > (uint64_t)fsz) {
        fprintf(stderr,
                "corrupt: header claims %u record(s) × %u bytes = %llu data "
                "bytes, but file is only %zu bytes total\n",
                h->record_count, h->record_size,
                (unsigned long long)((uint64_t)h->record_count * h->record_size),
                fsz);
        return -1;
    }

    return 0;
}

/* Decode all records and validate that every payload region is in-bounds. */
static int parse_records(const uint8_t *buf, size_t fsz,
                         const Header *h, Record **out)
{
    if (h->record_count == 0) {
        *out = NULL;
        return 0;
    }

    Record *recs = malloc(h->record_count * sizeof(Record));
    if (!recs) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }

    const uint8_t *p = buf + HEADER_SIZE;

    for (uint32_t i = 0; i < h->record_count; i++, p += h->record_size) {
        recs[i].id     = rd32le(p + 0);
        recs[i].offset = rd32le(p + 4);
        recs[i].length = rd32le(p + 8);

        /* Payload range check: [offset, offset+length) must be within file. */
        uint64_t end = (uint64_t)recs[i].offset + (uint64_t)recs[i].length;
        if (end > (uint64_t)fsz) {
            fprintf(stderr,
                    "corrupt: record %u (id=0x%08X): offset=%u + length=%u "
                    "= %llu overflows file size %zu\n",
                    i, recs[i].id, recs[i].offset, recs[i].length,
                    (unsigned long long)end, fsz);
            free(recs);
            return -1;
        }
    }

    *out = recs;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

static void print_header(const Header *h)
{
    printf("== Container header ==\n");
    printf("  Magic        : 0x%08X\n", h->magic);
    printf("  Version      : %u\n",     h->version);
    printf("  Flags        : 0x%04X\n", h->flags);
    printf("  Record count : %u\n",     h->record_count);
    printf("  Record size  : %u bytes\n", h->record_size);
    printf("\n");
}

static void print_table(const Record *recs, uint32_t count)
{
    if (count == 0) {
        printf("(no records)\n");
        return;
    }

    printf("%-6s  %-12s  %-12s  %s\n",
           "Index", "ID",     "Offset",  "Length");
    printf("%-6s  %-12s  %-12s  %s\n",
           "------","----------","----------","----------");

    for (uint32_t i = 0; i < count; i++) {
        printf("%-6u  0x%08X    %-12u  %u\n",
               i, recs[i].id, recs[i].offset, recs[i].length);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    size_t   fsz = 0;
    uint8_t *buf = slurp(argv[1], &fsz);
    if (!buf)
        return 1;

    Header h;
    if (parse_header(buf, fsz, &h) != 0) {
        free(buf);
        return 1;
    }

    Record *recs = NULL;
    if (parse_records(buf, fsz, &h, &recs) != 0) {
        free(buf);
        return 1;
    }

    print_header(&h);
    print_table(recs, h.record_count);

    free(recs);
    free(buf);
    return 0;
}
