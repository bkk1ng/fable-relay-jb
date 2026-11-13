/*
 * binfmt.c — little-endian binary container parser (C11, single file)
 *
 * Header layout (16 bytes, all fields little-endian):
 *   u32  magic        must equal 0x4C425931
 *   u16  version
 *   u16  flags
 *   u32  record_count
 *   u32  record_size  bytes per record on disk; must be >= 12
 *
 * Record layout (record_size bytes each, little-endian):
 *   u32  id
 *   u32  offset       byte offset of this record's data within the file
 *   u32  length       byte length of this record's data
 *
 * Exit codes:
 *   0  all records valid
 *   1  usage / I-O error
 *   2  corrupt container (bad magic, bad bounds, etc.)
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Endian-safe read helpers (no type-punning, no UB)                   */
/* ------------------------------------------------------------------ */

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)( (uint16_t)p[0]
                     | (uint16_t)((uint16_t)p[1] << 8) );
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MAGIC        UINT32_C(0x4C425931)
#define HEADER_SIZE  UINT32_C(16)   /* fixed on-disk header size       */
#define RECORD_MIN   UINT32_C(12)   /* id(4) + offset(4) + length(4)   */

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: binfmt <file>\n");
        return 1;
    }

    /* ---- open and read the whole file into memory ---- */
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "binfmt: cannot open '%s': %s\n",
                argv[1], strerror(errno));
        return 1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "binfmt: seek failed: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }

    long signed_len = ftell(f);
    if (signed_len < 0) {
        fprintf(stderr, "binfmt: ftell failed: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }
    rewind(f);

    size_t file_len = (size_t)signed_len;

    uint8_t *buf = malloc(file_len ? file_len : 1);  /* avoid malloc(0) */
    if (!buf) {
        fprintf(stderr, "binfmt: out of memory\n");
        fclose(f);
        return 1;
    }

    if (fread(buf, 1, file_len, f) != file_len) {
        fprintf(stderr, "binfmt: read error: %s\n", strerror(errno));
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);

    /* ---- check header fits ---- */
    if (file_len < (size_t)HEADER_SIZE) {
        fprintf(stderr,
                "binfmt: file too small for header "
                "(have %zu bytes, need %u)\n",
                file_len, HEADER_SIZE);
        free(buf);
        return 2;
    }

    /* ---- parse header ---- */
    uint32_t magic        = read_u32le(buf +  0);
    uint16_t version      = read_u16le(buf +  4);
    uint16_t flags        = read_u16le(buf +  6);
    uint32_t record_count = read_u32le(buf +  8);
    uint32_t record_size  = read_u32le(buf + 12);

    /* ---- validate magic ---- */
    if (magic != MAGIC) {
        fprintf(stderr,
                "binfmt: bad magic 0x%08X (expected 0x%08X)\n",
                magic, MAGIC);
        free(buf);
        return 2;
    }

    /* ---- validate record_size is large enough to hold known fields ---- */
    if (record_size < RECORD_MIN) {
        fprintf(stderr,
                "binfmt: record_size %u is below minimum %u\n",
                record_size, RECORD_MIN);
        free(buf);
        return 2;
    }

    /* ---- validate that the entire record table fits in the file ----
     * Guard against overflow before multiplying.                       */
    if (record_count > (SIZE_MAX - (size_t)HEADER_SIZE) / (size_t)record_size) {
        fprintf(stderr, "binfmt: record table size overflows size_t\n");
        free(buf);
        return 2;
    }

    size_t table_bytes = (size_t)record_count * (size_t)record_size;
    size_t table_end   = (size_t)HEADER_SIZE + table_bytes;

    if (table_end > file_len) {
        fprintf(stderr,
                "binfmt: record table extends past end of file "
                "(table ends at %zu, file is %zu bytes)\n",
                table_end, file_len);
        free(buf);
        return 2;
    }

    /* ---- print header summary ---- */
    printf("File    : %s\n",           argv[1]);
    printf("Magic   : 0x%08X\n",       magic);
    printf("Version : %u\n",           (unsigned)version);
    printf("Flags   : 0x%04X\n",       (unsigned)flags);
    printf("Records : %u  (record_size = %u bytes)\n\n",
           record_count, record_size);

    /* ---- print record table ---- */
    printf("%-6s  %-10s  %-10s  %-12s  %s\n",
           "INDEX", "ID", "OFFSET", "LENGTH", "STATUS");
    printf("------  ----------  ----------  ------------  --------\n");

    int corrupt = 0;

    for (uint32_t i = 0; i < record_count; ++i) {
        const uint8_t *r = buf + (size_t)HEADER_SIZE
                               + (size_t)i * (size_t)record_size;

        uint32_t id      = read_u32le(r + 0);
        uint32_t offset  = read_u32le(r + 4);
        uint32_t length  = read_u32le(r + 8);

        /*
         * Bounds check: [offset, offset + length) must lie within the
         * file.  Check length <= file_len first so the subtraction
         * file_len - length cannot underflow (both are size_t).
         */
        const char *status = "OK";
        if ((size_t)length > file_len ||
            (size_t)offset > file_len - (size_t)length) {
            status  = "CORRUPT";
            corrupt = 1;
        }

        printf("%-6u  0x%08X  0x%08X  %-12u  %s\n",
               i, id, offset, length, status);
    }

    free(buf);

    if (corrupt) {
        fprintf(stderr,
                "\nbinfmt: one or more records reference data "
                "outside the file boundary.\n");
        return 2;
    }

    return 0;
}
