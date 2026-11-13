/*
 * binfmt.c — little-endian binary container parser (C11, single file)
 *
 * Header layout (all fields little-endian, 16 bytes total):
 *   offset  0  u32  magic        must equal 0x4C425931
 *   offset  4  u16  version      must be non-zero
 *   offset  6  u16  flags
 *   offset  8  u32  record_count
 *   offset 12  u32  record_size  stride per record, must be >= 12
 *
 * Record layout (each record is record_size bytes; first 12 are defined):
 *   offset  0  u32  id
 *   offset  4  u32  offset       byte offset of this record's data in the file
 *   offset  8  u32  length       byte length of this record's data
 *
 * Exit status: 0 = clean, non-zero = corruption detected.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -o binfmt binfmt.c
 * Run:
 *   ./binfmt <file>
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── constants ────────────────────────────────────────────────────────────── */

#define MAGIC       UINT32_C(0x4C425931)
#define HEADER_SIZE UINT32_C(16)   /* 4+2+2+4+4 */
#define RECORD_MIN  UINT32_C(12)   /* 4+4+4      */

/* ── endian-safe readers ──────────────────────────────────────────────────── */

/*
 * These functions read multi-byte little-endian integers from an arbitrary
 * byte pointer without assuming the host's byte order or alignment.
 */

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ── types ────────────────────────────────────────────────────────────────── */

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

/* ── file I/O ─────────────────────────────────────────────────────────────── */

/*
 * Read the entire file at `path` into a heap buffer.
 * Returns NULL on failure (error already printed).
 * Sets *out_size to the file's byte count.
 * Caller must free() the returned buffer.
 */
static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "error: cannot seek in '%s': %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }

    long raw = ftell(f);
    if (raw < 0) {
        fprintf(stderr, "error: cannot tell position in '%s': %s\n",
                path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    size_t sz  = (size_t)raw;
    /* allocate sz+1 so a zero-length file still gets a valid non-NULL pointer */
    uint8_t *buf = malloc(sz + 1);
    if (!buf) {
        fprintf(stderr, "error: out of memory (%zu bytes)\n", sz);
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

/* ── parsing ──────────────────────────────────────────────────────────────── */

/*
 * Decode and validate the container header from `buf[0..file_size-1]`.
 * Returns 0 on success, -1 on any structural corruption (error already printed).
 */
static int parse_header(const uint8_t *buf, size_t file_size, Header *hdr)
{
    if (file_size < HEADER_SIZE) {
        fprintf(stderr,
                "corrupt: file too small for header (%zu < %u bytes)\n",
                file_size, HEADER_SIZE);
        return -1;
    }

    hdr->magic        = read_u32_le(buf +  0);
    hdr->version      = read_u16_le(buf +  4);
    hdr->flags        = read_u16_le(buf +  6);
    hdr->record_count = read_u32_le(buf +  8);
    hdr->record_size  = read_u32_le(buf + 12);

    if (hdr->magic != MAGIC) {
        fprintf(stderr,
                "corrupt: bad magic 0x%08X (expected 0x%08X)\n",
                hdr->magic, MAGIC);
        return -1;
    }

    if (hdr->version == 0) {
        fprintf(stderr, "corrupt: version field is 0 (reserved)\n");
        return -1;
    }

    if (hdr->record_size < RECORD_MIN) {
        fprintf(stderr,
                "corrupt: record_size %u < minimum %u bytes\n",
                hdr->record_size, RECORD_MIN);
        return -1;
    }

    /*
     * Check that all records fit in the file without overflow.
     * Instead of multiplying (which can overflow), divide:
     *   available_bytes / record_size >= record_count
     *   ⟺  available_bytes >= record_count * record_size
     * record_size is validated > 0 so the division is safe.
     */
    size_t available = file_size - HEADER_SIZE;
    if (available / hdr->record_size < hdr->record_count) {
        fprintf(stderr,
                "corrupt: %u record(s) × %u bytes/record exceeds the "
                "%zu bytes available after the header\n",
                hdr->record_count, hdr->record_size, available);
        return -1;
    }

    return 0;
}

/*
 * Decode a single record from the byte pointer `p`.
 * `p` must point to at least RECORD_MIN bytes.
 */
static void parse_record(const uint8_t *p, Record *r)
{
    r->id     = read_u32_le(p + 0);
    r->offset = read_u32_le(p + 4);
    r->length = read_u32_le(p + 8);
}

/* ── entry point ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: binfmt <file>\n");
        return 1;
    }

    size_t   file_size = 0;
    uint8_t *buf       = read_file(argv[1], &file_size);
    if (!buf) return 1;

    /* validate header */
    Header hdr;
    if (parse_header(buf, file_size, &hdr) != 0) {
        free(buf);
        return 1;
    }

    /* print header summary */
    printf("magic        : 0x%08X\n",   hdr.magic);
    printf("version      : %u\n",        hdr.version);
    printf("flags        : 0x%04X\n",   hdr.flags);
    printf("record_count : %u\n",        hdr.record_count);
    printf("record_size  : %u bytes\n\n", hdr.record_size);

    /* print record table */
    printf("%-8s  %-12s  %-12s  %-12s  %s\n",
           "INDEX", "ID", "OFFSET", "LENGTH", "STATUS");
    printf("--------  ------------  ------------  ------------  "
           "-------------\n");

    const uint8_t *records_base = buf + HEADER_SIZE;
    int            corrupt      = 0;

    for (uint32_t i = 0; i < hdr.record_count; ++i) {
        /*
         * Stride pointer arithmetic: i * record_size is validated to fit
         * within the buffer, so casting through size_t is safe.
         */
        const uint8_t *p = records_base + (size_t)i * hdr.record_size;

        Record r;
        parse_record(p, &r);

        /*
         * Per-record bounds check: the data range [offset, offset+length)
         * must lie entirely within the file.  Use 64-bit arithmetic to
         * avoid overflow on the addition before the comparison.
         */
        uint64_t    end = (uint64_t)r.offset + r.length;
        int         oob = (end > (uint64_t)file_size);
        const char *status = oob ? "OUT-OF-BOUNDS" : "ok";

        printf("%-8u  0x%08X    %-12u  %-12u  %s\n",
               i, r.id, r.offset, r.length, status);

        if (oob) {
            fprintf(stderr,
                    "corrupt: record[%u]: offset(%u) + length(%u) = %llu"
                    " > file size %zu\n",
                    i, r.offset, r.length,
                    (unsigned long long)end, file_size);
            corrupt = 1;
        }
    }

    free(buf);
    return corrupt ? 1 : 0;
}
