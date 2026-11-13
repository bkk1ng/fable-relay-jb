/*
 * binfmt.c — little-endian binary container parser (C11)
 *
 * Header layout (all fields little-endian, 16 bytes total):
 *   u32  magic        must be 0x4C425931 ("LBY1")
 *   u16  version
 *   u16  flags
 *   u32  record_count
 *   u32  record_size   validated against the fixed record stride (12)
 *
 * Record layout (12 bytes each):
 *   u32  id
 *   u32  offset        absolute byte offset into the file
 *   u32  length        byte length of the referenced region
 *
 * Exit codes:
 *   0   clean
 *   1   I/O or usage error
 *   2   structural corruption (bad magic / version / bounds)
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define MAGIC            UINT32_C(0x4C425931)
#define EXPECTED_VERSION UINT16_C(1)
#define HEADER_BYTES     16u   /* 4 + 2 + 2 + 4 + 4                       */
#define RECORD_BYTES     12u   /* 4 + 4 + 4                                */

/* -------------------------------------------------------------------------
 * Endian-safe read helpers
 *
 * Read bytes individually and reconstruct in little-endian order.
 * Correct on both little- and big-endian hosts; no UB from aliasing.
 * ---------------------------------------------------------------------- */

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0]
                    | ((uint32_t)p[1] << 8));
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* -------------------------------------------------------------------------
 * File I/O
 * ---------------------------------------------------------------------- */

static uint8_t *slurp_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        perror("ftell");
        fclose(f);
        return NULL;
    }
    rewind(f);

    /* Allocate at least 1 byte so malloc(0) edge case is not hit. */
    uint8_t *buf = malloc(len > 0 ? (size_t)len : 1);
    if (!buf) {
        fputs("error: out of memory\n", stderr);
        fclose(f);
        return NULL;
    }

    if ((size_t)len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = (size_t)len;
    return buf;
}

/* -------------------------------------------------------------------------
 * Parsed types
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Header validation
 *
 * Returns 0 on success, -1 with a message on any structural error.
 * ---------------------------------------------------------------------- */

static int parse_header(const uint8_t *buf, size_t file_size, Header *hdr)
{
    if (file_size < HEADER_BYTES) {
        fprintf(stderr,
                "error: file is %zu byte(s), need at least %u for the header\n",
                file_size, HEADER_BYTES);
        return -1;
    }

    hdr->magic        = read_u32le(buf +  0);
    hdr->version      = read_u16le(buf +  4);
    hdr->flags        = read_u16le(buf +  6);
    hdr->record_count = read_u32le(buf +  8);
    hdr->record_size  = read_u32le(buf + 12);

    if (hdr->magic != MAGIC) {
        fprintf(stderr, "error: bad magic 0x%08" PRIX32
                        " (expected 0x%08" PRIX32 ")\n",
                hdr->magic, MAGIC);
        return -1;
    }

    if (hdr->version != EXPECTED_VERSION) {
        fprintf(stderr, "error: version %" PRIu16
                        " is not supported (expected %" PRIu16 ")\n",
                hdr->version, EXPECTED_VERSION);
        return -1;
    }

    /* The header declares a record_size; it must match our fixed stride. */
    if (hdr->record_size != RECORD_BYTES) {
        fprintf(stderr,
                "error: header record_size %" PRIu32
                " does not match expected %u\n",
                hdr->record_size, RECORD_BYTES);
        return -1;
    }

    /* Guard against overflow before the multiplication. */
    if (hdr->record_count > (SIZE_MAX - HEADER_BYTES) / RECORD_BYTES) {
        fprintf(stderr,
                "error: record_count %" PRIu32 " would overflow size_t\n",
                hdr->record_count);
        return -1;
    }

    size_t required = HEADER_BYTES + (size_t)hdr->record_count * RECORD_BYTES;
    if (file_size < required) {
        fprintf(stderr,
                "error: file is %zu byte(s) but %" PRIu32 " record(s) require %zu\n",
                file_size, hdr->record_count, required);
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Record helpers
 * ---------------------------------------------------------------------- */

static void parse_record(const uint8_t *p, Record *rec)
{
    rec->id     = read_u32le(p + 0);
    rec->offset = read_u32le(p + 4);
    rec->length = read_u32le(p + 8);
}

/*
 * Check that [offset, offset+length) fits inside the file.
 * Both fields are u32; promote to u64 to catch the wrap-around case where
 * offset + length would overflow a 32-bit value.
 */
static int record_in_bounds(const Record *rec, size_t file_size)
{
    uint64_t end = (uint64_t)rec->offset + (uint64_t)rec->length;
    return end <= (uint64_t)file_size;
}

/* -------------------------------------------------------------------------
 * Table printer
 * ---------------------------------------------------------------------- */

static void print_divider(void)
{
    puts("-------  ----------  ----------  ----------");
}

static void print_records(const uint8_t *buf,
                          const Header   *hdr,
                          size_t          file_size,
                          int            *out_corrupt)
{
    printf("%-7s  %-10s  %-10s  %-10s\n",
           "index", "id", "offset", "length");
    print_divider();

    for (uint32_t i = 0; i < hdr->record_count; i++) {
        const uint8_t *p = buf + HEADER_BYTES + (size_t)i * RECORD_BYTES;
        Record rec;
        parse_record(p, &rec);

        const char *flag = "";
        if (!record_in_bounds(&rec, file_size)) {
            fprintf(stderr,
                    "error: record %" PRIu32 ": offset 0x%08" PRIX32
                    " + length 0x%08" PRIX32 " = 0x%016" PRIX64
                    " exceeds file size %zu\n",
                    i, rec.offset, rec.length,
                    (uint64_t)rec.offset + rec.length,
                    file_size);
            *out_corrupt = 1;
            flag = " [OOB]";
        }

        printf("%-7" PRIu32 "  0x%08" PRIX32 "  0x%08" PRIX32
               "  0x%08" PRIX32 "%s\n",
               i, rec.id, rec.offset, rec.length, flag);
    }

    print_divider();
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    size_t file_size = 0;
    uint8_t *buf = slurp_file(argv[1], &file_size);
    if (!buf) return 1;

    Header hdr;
    if (parse_header(buf, file_size, &hdr) != 0) {
        free(buf);
        return 2;
    }

    /* Summary block */
    printf("file:         %s (%zu bytes)\n", argv[1], file_size);
    printf("magic:        0x%08" PRIX32 "  (\"%.4s\")\n",
           hdr.magic, (const char *)buf);  /* print ASCII form of magic */
    printf("version:      %" PRIu16 "\n",   hdr.version);
    printf("flags:        0x%04" PRIX16 "\n", hdr.flags);
    printf("record_count: %" PRIu32 "\n",   hdr.record_count);
    printf("record_size:  %" PRIu32 " bytes\n\n", hdr.record_size);

    int corrupt = 0;
    print_records(buf, &hdr, file_size, &corrupt);

    free(buf);
    return corrupt ? 2 : 0;
}
