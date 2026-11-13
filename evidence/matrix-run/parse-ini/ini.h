/*
 * ini.h — C11 INI parser, callback-based, zero heap allocation.
 *
 * Usage:
 *   ini_parse_file(fp, my_handler, userdata, errbuf, sizeof errbuf);
 *
 * The parser recognises:
 *   [section]           section headers
 *   key = value         key/value pairs (whitespace around both is stripped)
 *   ; comment           semicolon comments (whole line or inline)
 *   # comment           hash comments    (whole line or inline)
 *   "value with\n esc"  double-quoted values with escape sequences
 *
 * Escape sequences inside double-quoted values:
 *   \\  \  \"  "  \'  '  \n  LF  \t  HT  \r  CR  \0  NUL
 *
 * Duplicate section.key pairs within a single parse call are detected and
 * cause INI_ERR_DUPLICATE to be returned.
 *
 * The only storage allocated inside ini_parse_file() beyond automatic
 * variables is a fixed line buffer and a fixed-size open-addressing hash
 * table, both on the stack (~32 KB combined worst case).
 */

#ifndef INI_H
#define INI_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum byte length of a single line including the newline and NUL. */
#define INI_LINE_MAX 4096

/*
 * Return codes from ini_parse_file() and from ini_handler_t.
 *
 *   INI_OK            Parsing completed successfully.
 *   INI_STOP          Handler returned non-zero; parsing was stopped cleanly.
 *   INI_ERR_IO        fgets() failed or NULL file/handler was supplied.
 *   INI_ERR_SYNTAX    Malformed line (error position in errbuf).
 *   INI_ERR_DUPLICATE Duplicate section.key pair detected.
 *   INI_ERR_OVERFLOW  Internal duplicate-detection table exhausted.
 *                     Increase INI_DEDUP_POOL / INI_DEDUP_SLOTS in ini.c if
 *                     the file has an unusually large number of unique keys.
 */
typedef enum {
    INI_OK            =  0,
    INI_STOP          =  1,
    INI_ERR_IO        = -1,
    INI_ERR_SYNTAX    = -2,
    INI_ERR_DUPLICATE = -3,
    INI_ERR_OVERFLOW  = -4
} ini_result_t;

/*
 * ini_handler_t — callback invoked for each key/value pair.
 *
 *   section   Current [section] name, stripped of whitespace.
 *             Empty string ("") if the pair precedes the first header.
 *   key       Key name, stripped of surrounding whitespace.
 *   value     Value string.  If the original was double-quoted, the quotes
 *             have been removed and escape sequences have been expanded.
 *             Otherwise trailing whitespace and inline comments are stripped.
 *   lineno    1-based line number of this pair in the stream.
 *   userdata  Forwarded unchanged from ini_parse_file().
 *
 * Return 0 to continue parsing.
 * Return any non-zero value to stop; ini_parse_file() will return INI_STOP.
 */
typedef int (*ini_handler_t)(const char *section,
                             const char *key,
                             const char *value,
                             int         lineno,
                             void       *userdata);

/*
 * ini_parse_file — parse an already-open, readable FILE stream.
 *
 *   f           Stream to read from (must be open in text or binary mode).
 *               The caller retains ownership; the stream is not closed here.
 *   handler     Invoked for every key/value pair found.
 *   userdata    Forwarded to every handler call; may be NULL.
 *   errbuf      If non-NULL, receives a human-readable error message
 *               including the line number when the return value is not
 *               INI_OK or INI_STOP.  Always NUL-terminated.
 *   errbuf_len  Byte capacity of errbuf.  Ignored when errbuf is NULL.
 *
 * Returns INI_OK on clean completion, INI_STOP if the handler requested
 * early termination, or a negative ini_result_t on error.
 */
ini_result_t ini_parse_file(FILE          *f,
                             ini_handler_t  handler,
                             void          *userdata,
                             char          *errbuf,
                             size_t         errbuf_len);

#ifdef __cplusplus
}
#endif

#endif /* INI_H */
