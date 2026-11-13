/*
 * ini.h — C11 INI file parser
 *
 * Supports:
 *   [sections], key=value pairs, ; and # comments,
 *   quoted values with escape sequences, duplicate key detection,
 *   precise error messages with line numbers.
 *
 * No heap allocation is performed; the only storage beyond the caller's
 * stack is a single line buffer and a hash table for duplicate detection.
 */

#ifndef INI_H
#define INI_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum byte length of a single line (including the terminating NUL).
 * Lines longer than this are truncated; a parse error is reported for the
 * overflow fragment.
 */
#define INI_LINE_MAX 4096

/*
 * ini_handler — called once per key/value pair.
 *
 *   user    opaque pointer from ini_options_t
 *   section current section name; "" for keys before the first [section]
 *           (only possible when allow_no_section is set)
 *   key     trimmed key name
 *   value   trimmed, unquoted, escape-resolved value
 *   lineno  1-based source line number of this key
 *
 * Return 0 to continue. Any other value aborts parsing and is returned
 * unchanged from ini_parse_file().
 */
typedef int (*ini_handler)(void       *user,
                           const char *section,
                           const char *key,
                           const char *value,
                           int         lineno);

/*
 * ini_error_handler — called on parse errors and duplicate keys.
 *
 *   user     opaque pointer from ini_options_t
 *   filename source file name from ini_options_t (never NULL)
 *   lineno   1-based line number where the error occurred
 *   message  human-readable description; stable only for the duration
 *            of this call
 *
 * After an error the parser recovers and continues on the next line.
 * If this field is NULL, all errors are silently ignored.
 */
typedef void (*ini_error_handler)(void       *user,
                                  const char *filename,
                                  int         lineno,
                                  const char *message);

typedef struct {
    ini_handler       handler;         /* required; must not be NULL        */
    ini_error_handler error_handler;   /* optional; may be NULL             */
    void             *user;            /* forwarded to both callbacks        */
    const char       *filename;        /* used in error messages; may be NULL
                                          (falls back to "<input>")         */
    int               allow_no_section;/* non-zero: deliver keys that appear
                                          before the first [section] with
                                          section="" instead of erroring    */
} ini_options_t;

/*
 * ini_parse_file — parse INI data from an open FILE stream.
 *
 * The stream is read sequentially but not closed. Parsing always starts
 * at the current stream position.
 *
 * Returns:
 *    0   success
 *   -1   I/O error (ferror() will be set on f)
 *  other the first non-zero value returned by opts->handler
 */
int ini_parse_file(FILE *f, const ini_options_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* INI_H */
