/*
 * ini.c — C11 INI file parser implementation
 */

#include "ini.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Duplicate-key detection
 *
 * We keep a fixed-size open-addressing hash table of (section, key) pairs.
 * No allocation: entries are stored inside a fixed-size char pool.
 * If the pool overflows we stop tracking duplicates silently — the parser
 * still works, it just won't report further duplicates.
 * ------------------------------------------------------------------------- */

#define DUP_TABLE_SLOTS  512          /* must be a power of two              */
#define DUP_POOL_BYTES   65536        /* total char storage for all strings  */

typedef struct {
    unsigned int offset;  /* byte offset into pool, 0 == empty slot         */
    unsigned int hash;    /* cached hash of the concatenated key string      */
} dup_slot_t;

typedef struct {
    dup_slot_t  slots[DUP_TABLE_SLOTS];
    char        pool[DUP_POOL_BYTES];
    unsigned int pool_used;
    int          full;    /* set once pool is exhausted                      */
} dup_table_t;

static void dup_table_init(dup_table_t *t)
{
    memset(t->slots, 0, sizeof(t->slots));
    t->pool[0]  = '\0';
    t->pool_used = 1;   /* offset 0 is reserved as "empty" sentinel          */
    t->full     = 0;
}

/* djb2 over arbitrary bytes */
static unsigned int djb2(const char *s, size_t n)
{
    unsigned int h = 5381u;
    for (size_t i = 0; i < n; i++)
        h = h * 33u ^ (unsigned char)s[i];
    return h;
}

/*
 * Build a compound key "<section>\0<key>" in buf (caller provides buf of at
 * least INI_LINE_MAX*2 bytes).  Returns the total byte length including both
 * NUL terminators.
 */
static size_t make_compound(char *buf, const char *section, const char *key)
{
    size_t sl = strlen(section);
    size_t kl = strlen(key);
    memcpy(buf, section, sl);
    buf[sl] = '\0';
    memcpy(buf + sl + 1, key, kl);
    buf[sl + 1 + kl] = '\0';
    return sl + 1 + kl + 1;
}

/*
 * Returns 1 if (section, key) was already seen, 0 otherwise.
 * Inserts the pair when not present, unless the pool is full.
 */
static int dup_check_and_insert(dup_table_t *t,
                                const char  *section,
                                const char  *key)
{
    char         compound[INI_LINE_MAX * 2];
    size_t       len = make_compound(compound, section, key);
    unsigned int h   = djb2(compound, len);
    unsigned int idx = h & (DUP_TABLE_SLOTS - 1);

    for (unsigned int probe = 0; probe < DUP_TABLE_SLOTS; probe++) {
        unsigned int i = (idx + probe) & (DUP_TABLE_SLOTS - 1);
        dup_slot_t  *s = &t->slots[i];

        if (s->offset == 0) {
            /* empty — insert here if pool has room */
            if (t->full)
                return 0; /* can't track, assume not duplicate */
            if (t->pool_used + len > DUP_POOL_BYTES) {
                t->full = 1;
                return 0;
            }
            memcpy(t->pool + t->pool_used, compound, len);
            s->offset    = t->pool_used;
            s->hash      = h;
            t->pool_used += (unsigned int)len;
            return 0; /* newly inserted */
        }

        if (s->hash == h) {
            /* compare stored compound key */
            const char *stored = t->pool + s->offset;
            size_t sl_s = strlen(stored);
            size_t kl_s = strlen(stored + sl_s + 1);
            size_t len_s = sl_s + 1 + kl_s + 1;
            if (len_s == len && memcmp(stored, compound, len) == 0)
                return 1; /* duplicate */
        }
    }
    /* table full — treat as not-duplicate */
    return 0;
}

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */

/* In-place left trim; returns pointer to first non-space character */
static const char *ltrim(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* In-place right trim of a mutable buffer; writes NUL and returns buf */
static char *rtrim(char *s)
{
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1)))
        end--;
    *end = '\0';
    return s;
}

/* -------------------------------------------------------------------------
 * Value unquoting
 *
 * If the value starts with " or ', the matching close-quote is found and
 * escape sequences within are resolved:
 *
 *   \\  ->  backslash
 *   \"  ->  double quote
 *   \'  ->  single quote
 *   \n  ->  newline
 *   \r  ->  carriage return
 *   \t  ->  tab
 *   \0  ->  NUL
 *   \xHH -> byte with hex value HH
 *
 * Any other \X is kept as-is (the backslash is preserved).
 * Writes the result into out (caller guarantees out is at least strlen(in)+1
 * bytes).  Returns 0 on success, -1 if the closing quote is missing.
 * ------------------------------------------------------------------------- */
static int unquote(const char *in, char *out, int *error_out)
{
    char  quote = *in++;
    char *dst   = out;
    *error_out  = 0;

    while (*in) {
        if (*in == quote) {
            /* closing quote — ignore trailing content */
            break;
        }
        if (*in == '\\') {
            in++;
            if (!*in) break;
            switch (*in) {
                case '\\': *dst++ = '\\'; break;
                case '"':  *dst++ = '"';  break;
                case '\'': *dst++ = '\''; break;
                case 'n':  *dst++ = '\n'; break;
                case 'r':  *dst++ = '\r'; break;
                case 't':  *dst++ = '\t'; break;
                case '0':  *dst++ = '\0'; break;
                case 'x': {
                    /* \xHH */
                    unsigned int val = 0;
                    int digits = 0;
                    while (digits < 2 && isxdigit((unsigned char)*(in + 1))) {
                        in++;
                        unsigned char c = (unsigned char)*in;
                        val = val * 16u + (unsigned int)(
                            (c >= '0' && c <= '9') ? (c - '0') :
                            (c >= 'a' && c <= 'f') ? (c - 'a' + 10) :
                                                     (c - 'A' + 10));
                        digits++;
                    }
                    *dst++ = (char)(unsigned char)val;
                    break;
                }
                default:
                    /* unknown escape — preserve backslash + char */
                    *dst++ = '\\';
                    *dst++ = *in;
                    break;
            }
        } else {
            *dst++ = *in;
        }
        in++;
    }

    if (*in != quote)
        *error_out = 1; /* unterminated */

    *dst = '\0';
    return *error_out ? -1 : 0;
}

/* -------------------------------------------------------------------------
 * Error reporting helper
 * ------------------------------------------------------------------------- */
static void report_error(const ini_options_t *opts,
                         int                  lineno,
                         const char          *message)
{
    if (opts->error_handler) {
        const char *fname = opts->filename ? opts->filename : "<input>";
        opts->error_handler(opts->user, fname, lineno, message);
    }
}

/* -------------------------------------------------------------------------
 * Main parser
 * ------------------------------------------------------------------------- */
int ini_parse_file(FILE *f, const ini_options_t *opts)
{
    if (!f || !opts || !opts->handler)
        return -1;

    char        line[INI_LINE_MAX];
    char        section[INI_LINE_MAX] = "";
    char        value_buf[INI_LINE_MAX];
    char        err_msg[INI_LINE_MAX + 64];
    dup_table_t dups;
    int         lineno = 0;
    int         rc     = 0;

    dup_table_init(&dups);

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        /* detect truncated line */
        size_t len = strlen(line);
        int    truncated = 0;
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            truncated = 1;
            /* drain to end of physical line */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
        }

        /* strip trailing newline */
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
            len--;
        }

        if (truncated) {
            snprintf(err_msg, sizeof(err_msg),
                     "line too long (truncated at %d bytes)", INI_LINE_MAX - 1);
            report_error(opts, lineno, err_msg);
            continue;
        }

        const char *p = ltrim(line);

        /* blank line or comment */
        if (*p == '\0' || *p == ';' || *p == '#')
            continue;

        /* section header */
        if (*p == '[') {
            const char *end = strchr(p + 1, ']');
            if (!end) {
                report_error(opts, lineno, "missing ']' in section header");
                continue;
            }
            /* copy section name and trim */
            size_t slen = (size_t)(end - (p + 1));
            if (slen >= INI_LINE_MAX) {
                report_error(opts, lineno, "section name too long");
                continue;
            }
            memcpy(section, p + 1, slen);
            section[slen] = '\0';
            /* trim section name */
            const char *st = ltrim(section);
            /* move in-place */
            if (st != section)
                memmove(section, st, strlen(st) + 1);
            rtrim(section);
            if (section[0] == '\0') {
                report_error(opts, lineno, "empty section name");
                /* reset to empty section anyway */
            }
            continue;
        }

        /* key = value */
        const char *eq = strchr(p, '=');
        if (!eq) {
            snprintf(err_msg, sizeof(err_msg),
                     "expected '=' separator, got: %.80s", p);
            report_error(opts, lineno, err_msg);
            continue;
        }

        /* extract key */
        char key[INI_LINE_MAX];
        size_t klen = (size_t)(eq - p);
        if (klen >= INI_LINE_MAX) {
            report_error(opts, lineno, "key name too long");
            continue;
        }
        memcpy(key, p, klen);
        key[klen] = '\0';
        rtrim(key);
        const char *kt = ltrim(key);
        if (kt != key)
            memmove(key, kt, strlen(kt) + 1);

        if (key[0] == '\0') {
            report_error(opts, lineno, "empty key name");
            continue;
        }

        /* check for section requirement */
        if (section[0] == '\0' && !opts->allow_no_section) {
            snprintf(err_msg, sizeof(err_msg),
                     "key '%s' appears before any [section]", key);
            report_error(opts, lineno, err_msg);
            continue;
        }

        /* extract raw value: everything after '=', trimmed */
        const char *vstart = ltrim(eq + 1);

        /* strip inline comment: unquoted ; or # ends the value */
        char raw_val[INI_LINE_MAX];
        {
            const char *vp    = vstart;
            char       *out   = raw_val;
            int         in_dq = 0, in_sq = 0;
            while (*vp) {
                if (*vp == '\\' && (in_dq || in_sq)) {
                    /* escape inside quote — copy both chars intact */
                    *out++ = *vp++;
                    if (*vp) *out++ = *vp++;
                    continue;
                }
                if (*vp == '"' && !in_sq)  { in_dq = !in_dq; *out++ = *vp++; continue; }
                if (*vp == '\'' && !in_dq) { in_sq = !in_sq; *out++ = *vp++; continue; }
                if (!in_dq && !in_sq && (*vp == ';' || *vp == '#'))
                    break; /* inline comment */
                *out++ = *vp++;
            }
            *out = '\0';
            rtrim(raw_val);
        }

        /* unquote if necessary */
        const char *final_val;
        if (raw_val[0] == '"' || raw_val[0] == '\'') {
            int qerr = 0;
            unquote(raw_val, value_buf, &qerr);
            if (qerr) {
                snprintf(err_msg, sizeof(err_msg),
                         "unterminated string literal for key '%s'", key);
                report_error(opts, lineno, err_msg);
                /* still deliver the partially-unquoted value */
            }
            final_val = value_buf;
        } else {
            final_val = raw_val;
        }

        /* duplicate key detection */
        if (dup_check_and_insert(&dups, section, key)) {
            snprintf(err_msg, sizeof(err_msg),
                     "duplicate key '%s' in section '%s'",
                     key, section[0] ? section : "(root)");
            report_error(opts, lineno, err_msg);
            /* still deliver the value — caller decides what to do */
        }

        rc = opts->handler(opts->user, section, key, final_val, lineno);
        if (rc != 0)
            return rc;
    }

    if (ferror(f))
        return -1;

    return 0;
}
