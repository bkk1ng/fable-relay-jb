/*
 * ini.h — C11 INI parser
 *
 * Features:
 *   [section] headers
 *   key = value pairs (whitespace trimmed)
 *   ; and # line comments (and inline comments on unquoted values)
 *   double-quoted values with \\ \" \n \r \t \0 escapes
 *   duplicate key detection (same section+key anywhere in the file)
 *   precise error messages with line numbers
 *   no heap allocation — fixed line buffer + on-stack dedup table
 */

#ifndef INI_H
#define INI_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------- */

typedef enum {
    INI_OK            =  0,  /* all keys parsed successfully              */
    INI_ERR_IO        = -1,  /* fgets / ferror failure                    */
    INI_ERR_SYNTAX    = -2,  /* malformed line (see errbuf for details)   */
    INI_ERR_DUPLICATE = -3,  /* duplicate key in same section             */
    INI_ERR_ABORTED   = -4,  /* handler returned non-zero                 */
} ini_status_t;

/* -------------------------------------------------------------------------
 * Callback
 *
 * Called once per key=value pair in document order.
 *
 *   user    opaque pointer forwarded from ini_parse_file()
 *   section current section name; empty string "" for keys before any header
 *   key     key name, leading/trailing whitespace stripped
 *   value   value string, quotes and escapes resolved, whitespace stripped
 *   lineno  1-based source line number of this pair
 *
 * Return 0 to continue parsing; any other value aborts and ini_parse_file()
 * returns INI_ERR_ABORTED.
 * ---------------------------------------------------------------------- */

typedef int (*ini_handler)(void       *user,
                           const char *section,
                           const char *key,
                           const char *value,
                           int         lineno);

/* -------------------------------------------------------------------------
 * ini_parse_file
 *
 *   fp       open FILE* positioned at the start of the INI data
 *   handler  callback invoked for every key=value pair
 *   user     forwarded verbatim to every handler call
 *   errbuf   if non-NULL, receives a NUL-terminated human-readable message
 *            on any non-OK return; unchanged on INI_OK
 *   errlen   capacity of errbuf in bytes
 *
 * Returns INI_OK (0) on success, negative ini_status_t on the first error.
 * ---------------------------------------------------------------------- */

ini_status_t ini_parse_file(FILE        *fp,
                            ini_handler  handler,
                            void        *user,
                            char        *errbuf,
                            size_t       errlen);

#ifdef __cplusplus
}
#endif

#endif /* INI_H */
