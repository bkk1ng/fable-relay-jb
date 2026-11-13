/*
 * ini.c — C11 INI parser implementation.
 * See ini.h for the public API and full documentation.
 */

#include "ini.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Duplicate-detection: open-addressing hash table with linear probing.
 * Keys are "section\0key\0" byte pairs stored in a flat pool.
 * We never allocate heap; both structures live on the stack.
 * ------------------------------------------------------------------------- */

#define INI_DEDUP_POOL  16384   /* bytes for section+key string storage     */
#define INI_DEDUP_SLOTS 1024    /* hash table slots (must be power of two)  */

typedef struct {
    char    pool[INI_DEDUP_POOL];
    size_t  pool_used;
    /* Each occupied slot holds the byte offset into pool of the entry. */
    int32_t slot[INI_DEDUP_SLOTS]; /* -1 = empty */
} dedup_t;

static void dedup_init(dedup_t *d)
{
    d->pool_used = 0;
    for (int i = 0; i < INI_DEDUP_SLOTS; ++i)
        d->slot[i] = -1;
}

/* FNV-1a 32-bit over two NUL-terminated strings concatenated. */
static uint32_t fnv1a_pair(const char *a, const char *b)
{
    uint32_t h = 2166136261u;
    for (const char *p = a; *p; ++p) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    h ^= 0u; h *= 16777619u; /* NUL separator */
    for (const char *p = b; *p; ++p) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

/*
 * Returns 1 if (section, key) was already seen (duplicate).
 * Returns 0 if it is new and was inserted successfully.
 * Returns -1 if the pool or table is full (INI_ERR_OVERFLOW).
 */
static int dedup_check(dedup_t *d, const char *section, const char *key)
{
    size_t slen  = strlen(section);
    size_t klen  = strlen(key);
    size_t need  = slen + 1 + klen + 1; /* "section\0key\0" */

    uint32_t hash = fnv1a_pair(section, key);
    uint32_t idx  = hash & (INI_DEDUP_SLOTS - 1);

    /* Linear probe: look for the entry or an empty slot. */
    for (int probe = 0; probe < INI_DEDUP_SLOTS; ++probe) {
        int32_t off = d->slot[idx];
        if (off == -1) {
            /* Empty slot — entry is new.  Store it if there is pool space. */
            if (d->pool_used + need > INI_DEDUP_POOL)
                return -1; /* pool exhausted */

            d->slot[idx] = (int32_t)d->pool_used;
            memcpy(d->pool + d->pool_used, section, slen + 1);
            d->pool_used += slen + 1;
            memcpy(d->pool + d->pool_used, key, klen + 1);
            d->pool_used += klen + 1;
            return 0; /* new */
        }
        /* Occupied slot — check if it matches this pair. */
        const char *s = d->pool + off;
        const char *k = s + strlen(s) + 1;
        if (strcmp(s, section) == 0 && strcmp(k, key) == 0)
            return 1; /* duplicate */

        idx = (idx + 1) & (INI_DEDUP_SLOTS - 1);
    }
    return -1; /* table full */
}

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */

/* Advance pointer past ASCII whitespace (space, tab, CR). */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r')
        ++p;
    return p;
}

/* Trim trailing ASCII whitespace in-place; returns new length. */
static size_t rtrim(char *s, size_t len)
{
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n'))
        --len;
    s[len] = '\0';
    return len;
}

/*
 * Strip an inline ; or # comment from an unquoted value.
 * The comment character must be preceded by whitespace to avoid false
 * matches in values like "file#1" or "url;param".
 */
static void strip_inline_comment(char *s)
{
    for (char *p = s; *p; ++p) {
        if ((*p == ';' || *p == '#') && p > s &&
            (p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Escape sequence expansion for double-quoted values.
 *
 * Writes the expanded string into dst (which may alias src — dst <= src is
 * safe because expansion never grows the string).  Returns the final length,
 * or -1 on an unrecognised escape.
 * ------------------------------------------------------------------------- */
static int expand_escapes(char *dst, const char *src)
{
    char *d = dst;
    const char *s = src;
    while (*s) {
        if (*s != '\\') {
            *d++ = *s++;
            continue;
        }
        ++s; /* consume backslash */
        switch (*s) {
        case '\\': *d++ = '\\'; break;
        case '"':  *d++ = '"';  break;
        case '\'': *d++ = '\''; break;
        case 'n':  *d++ = '\n'; break;
        case 't':  *d++ = '\t'; break;
        case 'r':  *d++ = '\r'; break;
        case '0':  *d++ = '\0'; break;
        default:
            return -1; /* unknown escape */
        }
        ++s;
    }
    *d = '\0';
    return (int)(d - dst);
}

/* -------------------------------------------------------------------------
 * ini_parse_file
 * ------------------------------------------------------------------------- */

ini_result_t ini_parse_file(FILE          *f,
                             ini_handler_t  handler,
                             void          *userdata,
                             char          *errbuf,
                             size_t         errbuf_len)
{
/* Convenience macro: write an error message and return a result code. */
#define ERR(code, ...) do {                                     \
    if (errbuf && errbuf_len > 0) {                             \
        snprintf(errbuf, errbuf_len, __VA_ARGS__);              \
    }                                                           \
    return (code);                                              \
} while (0)

    if (!f || !handler)
        ERR(INI_ERR_IO, "null file or handler");

    if (errbuf && errbuf_len > 0)
        errbuf[0] = '\0';

    char line[INI_LINE_MAX];
    char section[INI_LINE_MAX] = "";  /* current section name, default "" */
    int  lineno = 0;

    /* Duplicate-detection state lives entirely on the stack. */
    dedup_t dedup;
    dedup_init(&dedup);

    while (fgets(line, sizeof line, f)) {
        ++lineno;

        /* Ensure the line was fully read (no overflow). */
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len-1] != '\n' && !feof(f))
            ERR(INI_ERR_SYNTAX, "line %d: line too long (max %d bytes)",
                lineno, INI_LINE_MAX - 1);

        const char *p = skip_ws(line);

        /* ----- blank line or full-line comment ----- */
        if (*p == '\0' || *p == '\n' || *p == ';' || *p == '#')
            continue;

        /* ----- section header: [name] ----- */
        if (*p == '[') {
            ++p; /* skip '[' */
            const char *start = p;
            while (*p && *p != ']' && *p != '\n')
                ++p;
            if (*p != ']')
                ERR(INI_ERR_SYNTAX,
                    "line %d: unclosed section header (missing ']')", lineno);

            /* Copy section name and strip surrounding whitespace. */
            size_t slen = (size_t)(p - start);
            if (slen >= sizeof section)
                ERR(INI_ERR_SYNTAX,
                    "line %d: section name too long", lineno);
            memcpy(section, start, slen);
            section[slen] = '\0';

            /* Trim leading whitespace that may appear inside the brackets. */
            const char *trimmed = skip_ws(section);
            if (trimmed != section)
                memmove(section, trimmed, strlen(trimmed) + 1);
            rtrim(section, strlen(section));

            /* Verify there is nothing but optional whitespace/comment after ] */
            ++p; /* skip ']' */
            p = skip_ws(p);
            if (*p != '\0' && *p != '\n' && *p != ';' && *p != '#')
                ERR(INI_ERR_SYNTAX,
                    "line %d: unexpected characters after ']'", lineno);
            continue;
        }

        /* ----- key = value ----- */
        {
            /* Locate the '=' separator. */
            const char *eq = strchr(p, '=');
            if (!eq)
                ERR(INI_ERR_SYNTAX,
                    "line %d: expected 'key = value' or '[section]'", lineno);

            /* Extract and trim the key. */
            char key[INI_LINE_MAX];
            size_t klen = (size_t)(eq - p);
            if (klen >= sizeof key)
                ERR(INI_ERR_SYNTAX, "line %d: key too long", lineno);
            memcpy(key, p, klen);
            key[klen] = '\0';
            rtrim(key, klen);

            /* A key must not be empty. */
            if (key[0] == '\0')
                ERR(INI_ERR_SYNTAX, "line %d: empty key", lineno);

            /* Extract the raw value (everything after '='). */
            char value[INI_LINE_MAX];
            const char *vstart = skip_ws(eq + 1);
            size_t vlen = strlen(vstart);
            if (vlen >= sizeof value)
                ERR(INI_ERR_SYNTAX, "line %d: value too long", lineno);
            memcpy(value, vstart, vlen + 1);
            rtrim(value, vlen);

            /* ----- quoted value ----- */
            if (value[0] == '"') {
                /* Find the closing quote, respecting backslash escapes. */
                size_t vl = strlen(value);
                if (vl < 2 || value[vl-1] != '"')
                    ERR(INI_ERR_SYNTAX,
                        "line %d: unterminated double-quoted value", lineno);

                /* Temporarily NUL the closing quote and expand escapes. */
                value[vl-1] = '\0';
                char expanded[INI_LINE_MAX];
                int rc = expand_escapes(expanded, value + 1);
                if (rc < 0)
                    ERR(INI_ERR_SYNTAX,
                        "line %d: unrecognised escape sequence in value",
                        lineno);
                memcpy(value, expanded, (size_t)rc + 1);
            } else {
                /* Unquoted value: strip inline ; or # comment, then rtrim. */
                strip_inline_comment(value);
                rtrim(value, strlen(value));
            }

            /* ----- duplicate detection ----- */
            int dup = dedup_check(&dedup, section, key);
            if (dup < 0)
                ERR(INI_ERR_OVERFLOW,
                    "line %d: duplicate-detection table full; "
                    "increase INI_DEDUP_POOL / INI_DEDUP_SLOTS in ini.c",
                    lineno);
            if (dup == 1)
                ERR(INI_ERR_DUPLICATE,
                    "line %d: duplicate key '%s' in section '[%s]'",
                    lineno, key, section);

            /* ----- invoke the caller's handler ----- */
            int hrc = handler(section, key, value, lineno, userdata);
            if (hrc != 0)
                return INI_STOP;
        }
    }

    if (ferror(f))
        ERR(INI_ERR_IO, "I/O error reading stream");

    return INI_OK;

#undef ERR
}
