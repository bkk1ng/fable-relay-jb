/*
 * binfmt.c — little-endian binary container parser (C11)
 *
 * Header layout (all fields little-endian):
 *   u32  magic       0x4C425931 ("LBY1")
 *   u16  version
 *   u16  flags
 *   u32  record_count
 *   u32  record_size  (bytes per record; must be >= 12)
 *
 * Record layout (all fields little-endian):
 *   u32  id
 *   u32  offset
 *   u32  length
 *
 * Returns 0 on success, non-zero on any corruption or I/O error.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Endian-safe read helpers (no alignment assumptions)                 */
/* ------------------------------------------------------------------ */

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)( (uint32_t)p[0]
                     | (uint32_t)p[1] << 8 );
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAGIC          UINT32_C(0x4C425931)
#define SUPPORTED_VER  UINT16_C(1)

/* Fixed header size in bytes */
#define HEADER_SIZE    16u   /* 4 + 2 + 2 + 4 + 4 */

/* Minimum record size (id + offset + length = 3 × u32) */
#define RECORD_MIN     12u

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t record_count;
    uint32_t record_size;
} BinfmtHeader;

typedef struct {
    uint32_t id;
    uint32_t offset;
    uint32_t length;
} BinfmtRecord;

/* ------------------------------------------------------------------ */
/* File helpers                                                        */
/* ------------------------------------------------------------------ */

/* Returns the file size in bytes, or -1 on error. */
static long file_size(FILE *fp)
{
    if (fseek(fp, 0, SEEK_END) != 0)
        return -1;
    long sz = ftell(fp);
    if (fseek(fp, 0, SEEK_SET) != 0)
        return -1;
    return sz;
}

/* Read exactly n bytes into buf; returns 0 on success, -1 on short read/error. */
static int read_exact(FILE *fp, void *buf, size_t n)
{
    if (n == 0)
        return 0;
    if (fread(buf, 1, n, fp) != n)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static int parse_header(const uint8_t *buf, BinfmtHeader *hdr)
{
    hdr->magic        = read_u32le(buf +  0);
    hdr->version      = read_u16le(buf +  4);
    hdr->flags        = read_u16le(buf +  6);
    hdr->record_count = read_u32le(buf +  8);
    hdr->record_size  = read_u32le(buf + 12);
    return 0;
}

static void parse_record(const uint8_t *buf, BinfmtRecord *rec)
{
    rec->id     = read_u32le(buf + 0);
    rec->offset = read_u32le(buf + 4);
    rec->length = read_u32le(buf + 8);
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static int validate_header(const BinfmtHeader *hdr, long fsize)
{
    if (hdr->magic != MAGIC) {
        fprintf(stderr, "error: bad magic 0x%08X (expected 0x%08X)\n",
                hdr->magic, MAGIC);
        return -1;
    }

    if (hdr->version != SUPPORTED_VER) {
        fprintf(stderr, "error: unsupported version %u\n", hdr->version);
        return -1;
    }

    if (hdr->record_size < RECORD_MIN) {
        fprintf(stderr, "error: record_size %u is too small (minimum %u)\n",
                hdr->record_size, RECORD_MIN);
        return -1;
    }

    /* Total file must be at least header + all record descriptors */
    uint64_t required = (uint64_t)HEADER_SIZE
                      + (uint64_t)hdr->record_count * hdr->record_size;
    if ((uint64_t)fsize < required) {
        fprintf(stderr,
                "error: file too small: have %ld bytes, need at least %llu "
                "for %u records of %u bytes each\n",
                fsize,
                (unsigned long long)required,
                hdr->record_count,
                hdr->record_size);
        return -1;
    }

    return 0;
}

/*
 * Validate that each record's data region [offset, offset+length) lies
 * within the file. Offset 0 is relative to the start of the file; it is
 * the caller's responsibility to decide whether record data may overlap
 * the header — we only check that it doesn't exceed the file boundary.
 */
static int validate_record(const BinfmtRecord *rec, uint32_t idx, long fsize)
{
    uint64_t end = (uint64_t)rec->offset + (uint64_t)rec->length;
    if (end > (uint64_t)fsize) {
        fprintf(stderr,
                "error: record %u (id=0x%08X) data region [%u, %llu) "
                "exceeds file size %ld\n",
                idx, rec->id,
                rec->offset, (unsigned long long)end,
                fsize);
        return -1;
    }
    return 0;
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

    const char *path = argv[1];
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    /* Determine file size */
    long fsize = file_size(fp);
    if (fsize < 0) {
        fprintf(stderr, "error: cannot determine file size of '%s': %s\n",
                path, strerror(errno));
        fclose(fp);
        return 1;
    }

    if ((long)fsize < (long)HEADER_SIZE) {
        fprintf(stderr, "error: file too small to contain a valid header "
                        "(%ld bytes, need %u)\n", fsize, HEADER_SIZE);
        fclose(fp);
        return 1;
    }

    /* Read and parse header */
    uint8_t hbuf[HEADER_SIZE];
    if (read_exact(fp, hbuf, HEADER_SIZE) != 0) {
        fprintf(stderr, "error: failed to read header: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    BinfmtHeader hdr;
    parse_header(hbuf, &hdr);

    if (validate_header(&hdr, fsize) != 0) {
        fclose(fp);
        return 1;
    }

    /* Print header summary */
    printf("File   : %s\n", path);
    printf("Magic  : 0x%08X\n", hdr.magic);
    printf("Version: %u\n",     hdr.version);
    printf("Flags  : 0x%04X\n", hdr.flags);
    printf("Records: %u  (record_size=%u)\n\n", hdr.record_count, hdr.record_size);

    /* Column widths are fixed for alignment */
    printf("%-10s  %-12s  %-12s  %-12s\n",
           "Index", "ID", "Offset", "Length");
    printf("%-10s  %-12s  %-12s  %-12s\n",
           "----------", "------------", "------------", "------------");

    /* Allocate a buffer for one record row (record_size may exceed 12 bytes) */
    uint8_t *rbuf = malloc(hdr.record_size);
    if (!rbuf) {
        fprintf(stderr, "error: out of memory\n");
        fclose(fp);
        return 1;
    }

    int corrupt = 0;

    for (uint32_t i = 0; i < hdr.record_count; i++) {
        if (read_exact(fp, rbuf, hdr.record_size) != 0) {
            fprintf(stderr, "error: short read at record %u\n", i);
            corrupt = 1;
            break;
        }

        BinfmtRecord rec;
        parse_record(rbuf, &rec);

        if (validate_record(&rec, i, fsize) != 0) {
            corrupt = 1;
            /* continue printing remaining records for diagnostics */
        }

        printf("%-10u  0x%08X    %-12u  %-12u%s\n",
               i, rec.id, rec.offset, rec.length,
               (validate_record(&rec, i, fsize) != 0) ? "  [CORRUPT]" : "");
    }

    free(rbuf);
    fclose(fp);

    if (corrupt) {
        fprintf(stderr, "\ncorruption detected — file is not valid\n");
        return 1;
    }

    printf("\nOK — %u record%s validated\n",
           hdr.record_count, hdr.record_count == 1 ? "" : "s");
    return 0;
}
