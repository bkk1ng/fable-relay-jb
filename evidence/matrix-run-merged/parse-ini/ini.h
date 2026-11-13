/*
 * ini.h — C11 INI parser
 *
 * Features
 *   - Sections:          [section]
 *   - Key/value pairs:   key = value
 *   - Comments:          ; ... and # ... (line-start or inline after whitespace)
 *   - Quoted values:     "..." and '...' with escape sequences
 *   - Escape sequences:  \\ \" \' \n \r \t \0 \xHH
 *   - Duplicate keys:    detected and reported with line number
 *   - Allocation:        none beyond a stack-local line buffer
 *
 * Usage
 *   Open a FILE *, call ini_parse_file() with your handler, check the return.
 *   The file is not closed by the parser.
 *
 * Tunables (define before including, or via -D)
 *   INI_MAX_LINE   Maximum bytes per line including the NUL  (default 1024)
 *   INI_MAX_NAME   Maximum bytes in a section or key name    (default 128)
 *   INI_MAX_KEYS   Hash-table slots for duplicate detection  (default 512)
 *                  Must be a power of two.
 */

#ifndef INI_H
#define INI_H

#include <stdio.h>

#ifndef INI_MAX_LINE
#  define INI_MAX_LINE 1024
#endif

#ifndef INI_MAX_NAME
#  define INI_MAX_NAME 128
#endif

#ifndef INI_MAX_KEYS
#  define INI_MAX_KEYS 512
#endif

/* Return codes from ini_parse_file(). */
typedef enum {
    INI_OK          =  0,  /* Parsed to end of file without error.           */
    INI_ERR_IO      = -1,  /* fgets() / ferror() reported an I/O error.      */
    INI_ERR_SYNTAX  = -2,  /* Malformed line, bad section header, or unknown
                              escape sequence.  *errline is set.             */
    INI_ERR_LINE_LEN= -3,  /* A line exceeded INI_MAX_LINE-1 bytes.          */
    INI_ERR_DUP_KEY = -4,  /* The same section+key pair appeared twice.
                              *errline is the line of the second occurrence. */
    INI_ERR_HANDLER = -5,  /* The caller's handler returned non-zero.        */
} ini_error_t;

/*
 * ini_handler_t — callback invoked for every key/value pair.
 *
 *   user     Caller-provided context pointer, passed through unchanged.
 *   section  Current section name; empty string "" for keys before any
 *            section header.
 *   key      Key name, with leading/trailing whitespace stripped.
 *   value    Value string: for quoted values the quotes and escapes have been
 *            resolved; for unquoted values trailing whitespace and any inline
 *            comment have been stripped.
 *   lineno   1-based line number of this key in the source file.
 *
 * Return 0 to continue parsing, any other value to abort with INI_ERR_HANDLER.
 */
typedef int (*ini_handler_t)(void       *user,
                             const char *section,
                             const char *key,
                             const char *value,
                             int         lineno);

/*
 * ini_parse_file — parse an already-opened FILE *.
 *
 *   fp       Readable FILE *.  The parser does not close or rewind it.
 *   handler  Callback invoked once per key/value pair.
 *   user     Passed to every handler invocation.
 *   errline  If non-NULL, receives the 1-based line number on any error;
 *            set to 0 on INI_OK and INI_ERR_IO.
 *
 * Returns INI_OK on success, or a negative ini_error_t code on failure.
 */
ini_error_t ini_parse_file(FILE         *fp,
                           ini_handler_t handler,
                           void         *user,
                           int          *errline);

/* ini_strerror — human-readable string for an ini_error_t code. */
const char *ini_strerror(ini_error_t err);

#endif /* INI_H */
