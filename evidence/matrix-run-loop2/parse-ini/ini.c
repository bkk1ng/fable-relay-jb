/*
 * ini.c — C11 INI parser implementation
 */

#include "ini.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Write a formatted message into result->message (always NUL-terminated). */
static void set_message(ini_result_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->message, sizeof(r->message), fmt);
    va_end(ap);
}

/* Trim leading ASCII whitespace; returns pointer into the same buffer. */
static char *ltrim(char *s)
{
    while (*s && (unsigned char)*s <= ' ')
        ++s;
    return s;
}

/* Trim trailing ASCII whitespace in-place. */
static void rtrim(char *s)
{
    char *end = s + strlen(s);
    while (end > s && (unsigned char)end[-1] <= ' ')
        --end;
    *end = '\0';
}

/*
 * Resolve a quoted value string in-place.
 *
 * src points at the character AFTER the opening quote.
 * dst must have room for at least strlen(src)+1 bytes.
 * Returns 0 on success, -1 on unterminated/bad escape.
 */
static int unescape_quoted(const char *src, char *dst)
{
    while (*src && *src != '"') {
        if (*src == '\\') {
            ++src;
            switch (*src) {
                case '\\': *dst++ = '\\'; break;
                case '"':  *dst++ = '"';  break;
                case 'n':  *dst++ = '\n'; break;
                case 'r':  *dst++ = '\r'; break;
                case 't':  *dst++ = '\t'; break;
                case '0':  *dst++ = '\0'; break;
                default:   return -1;
            }
            ++src;
        } else {
            *dst++ = *src++;
        }
    }
    if (*src != '"')
        return -1;   /* unterminated */
    *dst = '\0';
    return 0;
}

/*
 * Strip an inline comment from an unquoted value.
 * A comment starts at the first ';' or '#' that is preceded by whitespace
 * (to avoid mangling values like "foo#bar" if they appear without a space).
 */
static void strip_inline_comment(char *s)
{
    char *p = s;
    while (*p) {
        if ((*p == ';' || *p == '#') && p > s && (unsigned char)p[-1] <= ' ') {
            /* back up past the whitespace and chop */
            char *q = p - 1;
            while (q > s && (unsigned char)q[-1] <= ' ')
                --q;
            *q = '\0';
            return;
        }
        ++p;
    }
}

/* -------------------------------------------------------------------------
 * Duplicate-key detection
 *
 * We use a fixed-size open-addressing hash table of (section, key) pairs
 * stored in a single arena so there is no heap allocation.  The table lives
 * on the stack of ini_parse_file.
 *
 * FNV-1a over the concatenation  section '\0' key '\0'.
 * ---------------------------------------------------------------------- */

#define DUP_TABLE_CAP  1024   /* must be a power of two */
#define DUP_KEY_MAX    (64 + 1 + INI_MAX_LINE)   /* section NUL key */

typedef struct {
    char  buf[DUP_KEY_MAX];
    int   used;
} dup_slot_t;

typedef struct {
    dup_slot_t slots[DUP_TABLE_CAP];
} dup_table_t;

static uint32_t fnv1a(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/*
 * Returns 1 if already seen (duplicate), 0 if freshly inserted.
 */
static int dup_check(dup_table_t *t, const char *section, const char *key)
{
    /* Build composite key: section NUL key NUL */
    char composite[DUP_KEY_MAX];
    size_t slen = strlen(section);
    size_t klen = strlen(key);
    if (slen + 1 + klen + 1 > sizeof composite)
        return 0;   /* too long to track — treat as new */
    memcpy(composite, section, slen + 1);
    memcpy(composite + slen + 1, key, klen + 1);
    size_t total = slen + 1 + klen + 1;

    uint32_t h = fnv1a(composite, total);
    uint32_t idx = h & (DUP_TABLE_CAP - 1);

    for (uint32_t i = 0; i < DUP_TABLE_CAP; ++i) {
        uint32_t probe = (idx + i) & (DUP_TABLE_CAP - 1);
        dup_slot_t *s = &t->slots[probe];
        if (!s->used) {
            /* Empty slot — insert and report as new. */
            memcpy(s->buf, composite, total);
            s->used = 1;
            return 0;
        }
        if (memcmp(s->buf, composite, total) == 0)
            return 1;   /* duplicate */
    }
    /* Table full; silently treat as new rather than false positive. */
    return 0;
}

/* -------------------------------------------------------------------------
 * Core parser
 * ---------------------------------------------------------------------- */

ini_result_t ini_parse_file(FILE *f, ini_handler_t handler, void *userdata)
{
    ini_result_t result = { INI_OK, 0, "" };

    /* Static line buffer — only allocation in this parser. */
    char raw[INI_MAX_LINE];
    char value_buf[INI_MAX_LINE];
    char section[256] = "";

    /*
     * Duplicate table on the stack.  Zero-initialised via compound literal
     * so slot.used is 0 for every entry.
     */
    dup_table_t dups = { { 0 } };

    int lineno = 0;

    while (fgets(raw, sizeof raw, f)) {
        ++lineno;

        /* Detect truncated lines (no newline and buffer completely full). */
        size_t rlen = strlen(raw);
        if (rlen == sizeof raw - 1 && raw[rlen - 1] != '\n') {
            result.code   = INI_ERR_SYNTAX;
            result.lineno = lineno;
            set_message(&result, "line %d: line exceeds maximum length (%d)",
                        lineno, INI_MAX_LINE - 1);
            return result;
        }

        char *line = ltrim(raw);
        rtrim(line);

        /* Blank line or comment — skip. */
        if (*line == '\0' || *line == ';' || *line == '#')
            continue;

        /* ---- Section header ------------------------------------------ */
        if (*line == '[') {
            char *close = strchr(line + 1, ']');
            if (!close) {
                result.code   = INI_ERR_SYNTAX;
                result.lineno = lineno;
                set_message(&result, "line %d: missing ']' in section header",
                            lineno);
                return result;
            }
            /* Anything after ']' must be whitespace or a comment. */
            char *after = ltrim(close + 1);
            if (*after != '\0' && *after != ';' && *after != '#') {
                result.code   = INI_ERR_SYNTAX;
                result.lineno = lineno;
                set_message(&result,
                            "line %d: unexpected text after section header",
                            lineno);
                return result;
            }
            *close = '\0';
            char *sname = ltrim(line + 1);
            rtrim(sname);
            if (*sname == '\0') {
                result.code   = INI_ERR_SYNTAX;
                result.lineno = lineno;
                set_message(&result, "line %d: empty section name", lineno);
                return result;
            }
            if (strlen(sname) >= sizeof section) {
                result.code   = INI_ERR_SYNTAX;
                result.lineno = lineno;
                set_message(&result, "line %d: section name too long", lineno);
                return result;
            }
            strncpy(section, sname, sizeof section - 1);
            section[sizeof section - 1] = '\0';
            continue;
        }

        /* ---- Key = Value --------------------------------------------- */
        char *eq = strchr(line, '=');
        if (!eq) {
            result.code   = INI_ERR_SYNTAX;
            result.lineno = lineno;
            set_message(&result,
                        "line %d: expected 'key = value', got '%s'",
                        lineno, line);
            return result;
        }

        /* Split and trim key. */
        *eq = '\0';
        char *key = line;
        rtrim(key);
        key = ltrim(key);
        if (*key == '\0') {
            result.code   = INI_ERR_SYNTAX;
            result.lineno = lineno;
            set_message(&result, "line %d: empty key", lineno);
            return result;
        }

        /* Process value. */
        char *vstart = ltrim(eq + 1);
        const char *value;

        if (*vstart == '"') {
            /* Quoted value. */
            if (unescape_quoted(vstart + 1, value_buf) != 0) {
                result.code   = INI_ERR_SYNTAX;
                result.lineno = lineno;
                set_message(&result,
                            "line %d: bad quoted value (unterminated or "
                            "invalid escape)",
                            lineno);
                return result;
            }
            value = value_buf;
        } else {
            /* Unquoted value — strip inline comment, then trailing space. */
            strncpy(value_buf, vstart, sizeof value_buf - 1);
            value_buf[sizeof value_buf - 1] = '\0';
            strip_inline_comment(value_buf);
            rtrim(value_buf);
            value = value_buf;
        }

        /* Duplicate key check. */
        if (dup_check(&dups, section, key)) {
            result.code   = INI_ERR_DUPLICATE;
            result.lineno = lineno;
            set_message(&result,
                        "line %d: duplicate key '%s' in section '[%s]'",
                        lineno, key, section);
            return result;
        }

        /* Dispatch to caller. */
        if (handler(userdata, section, key, value, lineno) != 0)
            return result;   /* caller requested early stop; code stays OK */
    }

    if (ferror(f)) {
        result.code   = INI_ERR_IO;
        result.lineno = 0;
        set_message(&result, "I/O error while reading file");
    }

    return result;
}

/* -------------------------------------------------------------------------
 * Convenience path wrapper
 * ---------------------------------------------------------------------- */

ini_result_t ini_parse_path(const char *path,
                             ini_handler_t handler,
                             void *userdata)
{
    ini_result_t result = { INI_OK, 0, "" };

    FILE *f = fopen(path, "r");
    if (!f) {
        result.code   = INI_ERR_IO;
        result.lineno = 0;
        set_message(&result, "cannot open '%s'", path);
        return result;
    }

    result = ini_parse_file(f, handler, userdata);
    fclose(f);
    return result;
}
