/*
 * ini.h — C11 INI parser
 *
 * Supports:
 *   - [section] headers
 *   - key = value pairs
 *   - ; and # comments (full-line, or inline after whitespace on unquoted values)
 *   - Quoted values: "..." with escapes \\  \"  \n  \r  \t  \0
 *   - Duplicate key detection (per section)
 *   - Precise error messages with line numbers
 *   - No heap allocation beyond the fixed line buffer
 */

#ifndef INI_H
#define INI_H

#include <stdio.h>
#include <stdint.h>

/* Maximum line length including the NUL terminator. */
#define INI_MAX_LINE 4096

/* -------------------------------------------------------------------------
 * Result codes
 * ---------------------------------------------------------------------- */
typedef enum {
    INI_OK            = 0,  /* parsed successfully (or callback stopped early) */
    INI_ERR_IO        = 1,  /* fopen / fgets failed                            */
    INI_ERR_SYNTAX    = 2,  /* malformed line                                  */
    INI_ERR_DUPLICATE = 3,  /* same key seen twice within the same section     */
} ini_status_t;

typedef struct {
    ini_status_t code;
    int          lineno;        /* 1-based; 0 when error is not line-specific   */
    char         message[128];  /* human-readable, NUL-terminated               */
} ini_result_t;

/* -------------------------------------------------------------------------
 * User callback
 *
 * Called once for every key/value pair.
 *
 *   userdata  – opaque pointer passed through from ini_parse_file
 *   section   – current section name; empty string "" before the first header
 *   key       – key (whitespace-trimmed, never empty)
 *   value     – value (quotes stripped, escapes resolved, trailing space trimmed)
 *   lineno    – 1-based source line number
 *
 * Return 0 to keep parsing, any other value to stop (result code stays INI_OK).
 * ---------------------------------------------------------------------- */
typedef int (*ini_handler_t)(void       *userdata,
                             const char *section,
                             const char *key,
                             const char *value,
                             int         lineno);

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/*
 * ini_parse_file — parse an already-open FILE stream.
 *
 * Does not fclose(f).  f must be open in text mode.
 * handler and f must not be NULL.
 */
ini_result_t ini_parse_file(FILE          *f,
                             ini_handler_t  handler,
                             void          *userdata);

/*
 * ini_parse_path — convenience wrapper that opens path, parses, then closes.
 */
ini_result_t ini_parse_path(const char    *path,
                             ini_handler_t  handler,
                             void          *userdata);

#endif /* INI_H */
