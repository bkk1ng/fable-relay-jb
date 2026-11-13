/*
 * ini.h — C11 INI parser
 *
 * Features
 *   - [section] headers
 *   - key = value pairs (whitespace around '=' ignored)
 *   - ; and # full-line comments; inline comments stripped from unquoted values
 *   - "quoted values" with \\ \" \n \t \r \0 escapes
 *   - duplicate key detection (per section)
 *   - precise error messages with 1-based line numbers
 *   - ini_parse_file → caller-supplied callback, no heap allocation
 */

#ifndef INI_H
#define INI_H

#include <stdio.h>
#include <stddef.h>

/* Tune these if your files are wider or names are longer. */
#define INI_LINE_MAX    1024   /* maximum raw line length including NUL       */
#define INI_SECTION_MAX  256   /* maximum section name length including NUL   */
#define INI_KEY_MAX      256   /* maximum key length including NUL            */
#define INI_ERRBUF_MIN   256   /* recommended minimum size for errbuf         */

/*
 * Duplicate-detection hash table capacity (must be a power of two).
 * Covers up to ~HSET_CAP * 0.75 unique section+key pairs before the
 * check is silently skipped for further entries.
 */
#define INI_HSET_CAP    1024u

typedef enum {
    INI_OK            = 0,  /* success, or handler requested early stop      */
    INI_ERR_IO        = 1,  /* fgets / stream error                          */
    INI_ERR_SYNTAX    = 2,  /* malformed line                                */
    INI_ERR_DUPLICATE = 3,  /* duplicate key within a section                */
    INI_ERR_OVERFLOW  = 4,  /* line or name exceeds the configured limit     */
} ini_err_t;

/*
 * ini_handler_t — callback invoked for every key=value pair.
 *
 *   userdata  caller context, passed through unchanged
 *   section   current section name; empty string before the first header
 *   key       key string, trimmed
 *   value     value string, trimmed; quotes and escapes already resolved
 *   lineno    1-based source line number
 *
 * Return 0 to continue parsing, any non-zero value to stop early.
 * An early stop is reported as INI_OK (not an error).
 */
typedef int (*ini_handler_t)(
    void       *userdata,
    const char *section,
    const char *key,
    const char *value,
    int         lineno
);

/*
 * ini_parse_file — parse an INI stream.
 *
 *   f         open FILE* positioned at the start of INI content
 *   handler   callback invoked per key=value pair
 *   userdata  passed through to handler unchanged
 *   errbuf    if non-NULL, receives a NUL-terminated error message on failure
 *   errbufsz  byte capacity of errbuf (INI_ERRBUF_MIN recommended)
 *
 * Returns INI_OK on success or early handler stop.
 * On any INI_ERR_* code, errbuf (if provided) contains the reason.
 */
ini_err_t ini_parse_file(
    FILE          *f,
    ini_handler_t  handler,
    void          *userdata,
    char          *errbuf,
    size_t         errbufsz
);

#endif /* INI_H */
