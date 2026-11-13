/*
 * ini.c — C11 INI parser implementation
 */

#include "ini.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define INI_MAX_LINE    4096   /* maximum raw line length including '\n'    */
#define INI_MAX_SECTION  256   /* maximum section name length               */
#define INI_MAX_KEY      256   /* maximum key name length                   */
#define INI_MAX_VALUE   2048   /* maximum decoded value length              */

/* Dedup table: open-addressed hash set of "section\0key" strings.
 * Sized as a power-of-two comfortably larger than any realistic file. */
#define INI_DEDUP_SLOTS 2048   /* must be power of two                      */
#define INI_DEDUP_MASK  (INI_DEDUP_SLOTS - 1)

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void set_error(char *errbuf, size_t errlen, int lineno,
                      const char *fmt, ...)
{
    if (!errbuf || errlen == 0) return;

    char msg[INI_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(errbuf, errlen, "line %d: %s", lineno, msg);
}

/* Trim leading and trailing ASCII whitespace in-place; returns str. */
static char *trim(char *s)
{
    /* leading */
    while (*s && (unsigned char)*s <= ' ') ++s;

    /* trailing */
    char *end = s + strlen(s);
    while (end > s && (unsigned char)*(end - 1) <= ' ') --end;
    *end = '\0';

    return s;
}

/* -------------------------------------------------------------------------
 * Dedup table (on-stack, caller zero-initialises)
 * ---------------------------------------------------------------------- */

typedef struct {
    /* Each slot holds a "section\x01key" string or is empty (slot[0]=='\0').
     * We use \x01 (SOH) as separator since it cannot appear in a parsed
     * section or key (both are stripped of whitespace and kept printable). */
    char slot[INI_DEDUP_SLOTS][INI_MAX_SECTION + 1 + INI_MAX_KEY + 1];
} ini_dedup_t;

/* FNV-1a 32-bit */
static unsigned int fnv1a(const char *s)
{
    unsigned int h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/*
 * Returns 1 if (section, key) was already seen (duplicate), 0 if new and
 * recorded.  Returns -1 if the table is full (should not happen with
 * reasonable files and INI_DEDUP_SLOTS set generously).
 */
static int dedup_check(ini_dedup_t *d, const char *section, const char *key)
{
    char combined[INI_MAX_SECTION + 1 + INI_MAX_KEY + 1];
    snprintf(combined, sizeof(combined), "%s\x01%s", section, key);

    unsigned int h = fnv1a(combined) & INI_DEDUP_MASK;
    unsigned int start = h;

    do {
        if (d->slot[h][0] == '\0') {
            /* empty slot — record and report new */
            strncpy(d->slot[h], combined, sizeof(d->slot[h]) - 1);
            d->slot[h][sizeof(d->slot[h]) - 1] = '\0';
            return 0;
        }
        if (strcmp(d->slot[h], combined) == 0) {
            return 1; /* duplicate */
        }
        h = (h + 1) & INI_DEDUP_MASK;
    } while (h != start);

    return -1; /* table full */
}

/* -------------------------------------------------------------------------
 * Value decoder
 *
 * Handles two forms:
 *   quoted   "text with \" \\  \n \r \t \0 escapes"
 *   unquoted  plain text, stripped of inline ; or # comments and whitespace
 *
 * Returns 1 on success, 0 on error (errbuf filled).
 * ---------------------------------------------------------------------- */

static int decode_value(const char *raw, char *out, size_t outsz,
                        int lineno, char *errbuf, size_t errlen)
{
    const char *p = raw;

    /* Skip leading whitespace */
    while (*p && (unsigned char)*p <= ' ') ++p;

    if (*p == '"') {
        /* ---- quoted value ---- */
        ++p; /* consume opening quote */
        size_t i = 0;

        while (*p && *p != '"') {
            if (i + 1 >= outsz) {
                set_error(errbuf, errlen, lineno, "quoted value too long");
                return 0;
            }

            if (*p == '\\') {
                ++p;
                switch (*p) {
                    case '\\': out[i++] = '\\'; break;
                    case '"':  out[i++] = '"';  break;
                    case 'n':  out[i++] = '\n'; break;
                    case 'r':  out[i++] = '\r'; break;
                    case 't':  out[i++] = '\t'; break;
                    case '0':  out[i++] = '\0'; break;
                    case '\0':
                        set_error(errbuf, errlen, lineno,
                                  "unterminated escape sequence");
                        return 0;
                    default:
                        set_error(errbuf, errlen, lineno,
                                  "unknown escape '\\%c'", *p);
                        return 0;
                }
                if (*p) ++p;
            } else {
                out[i++] = *p++;
            }
        }

        if (*p != '"') {
            set_error(errbuf, errlen, lineno, "unterminated quoted value");
            return 0;
        }
        ++p; /* consume closing quote */

        /* Nothing meaningful should follow a closing quote */
        while (*p && (unsigned char)*p <= ' ') ++p;
        if (*p && *p != ';' && *p != '#') {
            set_error(errbuf, errlen, lineno,
                      "unexpected characters after closing quote");
            return 0;
        }

        out[i] = '\0';

    } else {
        /* ---- unquoted value ---- */
        /* Strip inline ; or # comment.  A comment must be preceded by
         * whitespace to avoid splitting paths like /a/b#fragment. */
        char buf[INI_MAX_VALUE];
        strncpy(buf, p, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        for (size_t i = 1; buf[i]; ++i) {
            if ((buf[i] == ';' || buf[i] == '#') &&
                (unsigned char)buf[i - 1] <= ' ') {
                buf[i - 1] = '\0'; /* trim the space before the comment */
                break;
            }
        }

        char *trimmed = trim(buf);
        size_t tlen = strlen(trimmed);
        if (tlen + 1 > outsz) {
            set_error(errbuf, errlen, lineno, "value too long");
            return 0;
        }
        memcpy(out, trimmed, tlen + 1);
    }

    return 1;
}

/* -------------------------------------------------------------------------
 * ini_parse_file
 * ---------------------------------------------------------------------- */

ini_status_t ini_parse_file(FILE        *fp,
                            ini_handler  handler,
                            void        *user,
                            char        *errbuf,
                            size_t       errlen)
{
    if (!fp || !handler) return INI_ERR_IO;

    /* Line buffer — the only allocation in this parser */
    char line[INI_MAX_LINE];

    /* Current section (empty string = before any header) */
    char section[INI_MAX_SECTION] = "";

    /* Decoded key and value */
    char key[INI_MAX_KEY];
    char value[INI_MAX_VALUE];

    /* Duplicate-detection table on the stack */
    ini_dedup_t dedup;
    memset(&dedup, 0, sizeof(dedup));

    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        ++lineno;

        /* Detect truncated lines (no '\n' and not EOF) */
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 &&
            line[len - 1] != '\n' &&
            !feof(fp)) {
            set_error(errbuf, errlen, lineno, "line too long (> %d bytes)",
                      (int)(sizeof(line) - 2));
            return INI_ERR_SYNTAX;
        }

        /* Strip the newline */
        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        /* Pointer for scanning */
        char *p = line;

        /* Skip leading whitespace */
        while (*p && (unsigned char)*p <= ' ') ++p;

        /* Empty line or comment */
        if (*p == '\0' || *p == ';' || *p == '#') continue;

        /* ---- Section header ---- */
        if (*p == '[') {
            char *start = p + 1;
            char *end   = strchr(start, ']');

            if (!end) {
                set_error(errbuf, errlen, lineno,
                          "missing closing ']' in section header");
                return INI_ERR_SYNTAX;
            }

            /* There must be nothing meaningful after the closing ] */
            char *after = end + 1;
            while (*after && (unsigned char)*after <= ' ') ++after;
            if (*after && *after != ';' && *after != '#') {
                set_error(errbuf, errlen, lineno,
                          "unexpected characters after section header");
                return INI_ERR_SYNTAX;
            }

            *end = '\0';
            char *name = trim(start);

            if (strlen(name) == 0) {
                set_error(errbuf, errlen, lineno, "empty section name");
                return INI_ERR_SYNTAX;
            }
            if (strlen(name) >= INI_MAX_SECTION) {
                set_error(errbuf, errlen, lineno, "section name too long");
                return INI_ERR_SYNTAX;
            }

            strncpy(section, name, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }

        /* ---- Key = value ---- */
        char *eq = strchr(p, '=');
        if (!eq) {
            set_error(errbuf, errlen, lineno,
                      "expected '=' in key=value pair");
            return INI_ERR_SYNTAX;
        }

        /* Key: everything before '=' */
        *eq = '\0';
        char *kraw = trim(p);

        if (strlen(kraw) == 0) {
            set_error(errbuf, errlen, lineno, "empty key");
            return INI_ERR_SYNTAX;
        }
        if (strlen(kraw) >= INI_MAX_KEY) {
            set_error(errbuf, errlen, lineno, "key too long");
            return INI_ERR_SYNTAX;
        }

        strncpy(key, kraw, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';

        /* Value: everything after '=' */
        const char *vraw = eq + 1;

        if (!decode_value(vraw, value, sizeof(value),
                          lineno, errbuf, errlen)) {
            return INI_ERR_SYNTAX;
        }

        /* Duplicate detection */
        int dup = dedup_check(&dedup, section, key);
        if (dup < 0) {
            /* Table full — treat as a non-fatal warning; parsing continues.
             * In practice INI_DEDUP_SLOTS would need to be exceeded first. */
        } else if (dup == 1) {
            set_error(errbuf, errlen, lineno,
                      "duplicate key '%s' in section '[%s]'", key, section);
            return INI_ERR_DUPLICATE;
        }

        /* Fire the handler */
        int rc = handler(user, section, key, value, lineno);
        if (rc != 0) return INI_ERR_ABORTED;
    }

    if (ferror(fp)) {
        set_error(errbuf, errlen, lineno, "I/O error reading file");
        return INI_ERR_IO;
    }

    return INI_OK;
}
