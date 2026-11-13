/*
 * lex.c — tokenizer for a small config language (C11)
 */

#include "lex.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static int is_xdigit(char c) {
    return is_digit(c) ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Make a token whose extent is [start, lx->pos). */
static Token make_tok(TokType type, const char *start, int line) {
    Token t;
    t.type  = type;
    t.start = start;
    t.len   = 0;      /* filled by callers */
    t.line  = line;
    return t;
}

/* ------------------------------------------------------------------ */
/* public api                                                           */
/* ------------------------------------------------------------------ */

void lex_init(Lexer *lx, const char *src) {
    lx->pos  = src ? src : "";
    lx->line = 1;
}

Token lex_next(Lexer *lx) {
    const char *p    = lx->pos;
    int         line = lx->line;
    Token       t;

restart:
    /* ---- skip whitespace (not newline) and comments ---- */
    while (*p == ' ' || *p == '\t' || *p == '\r') {
        ++p;
    }

    /* line comment: // ... end-of-line */
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        while (*p && *p != '\n') ++p;
        goto restart;
    }

    /* block comment: /* ... */ */
    if (p[0] == '/' && p[1] == '*') {
        p += 2;
        while (*p) {
            if (*p == '\n') {
                ++lx->line;
                ++p;
            } else if (p[0] == '*' && p[1] == '/') {
                p += 2;
                break;
            } else {
                ++p;
            }
        }
        line = lx->line;
        goto restart;
    }

    lx->pos = p;   /* update before early returns below */

    /* ---- EOF ---- */
    if (*p == '\0') {
        t       = make_tok(TOK_EOF, p, lx->line);
        t.len   = 0;
        return t;
    }

    /* ---- newline ---- */
    if (*p == '\n') {
        t        = make_tok(TOK_NEWLINE, p, lx->line);
        t.len    = 1;
        ++lx->line;
        lx->pos  = p + 1;
        return t;
    }

    /* ---- identifier ---- */
    if (is_alpha(*p)) {
        const char *start = p;
        while (is_alnum(*p)) ++p;
        t        = make_tok(TOK_IDENT, start, line);
        t.len    = (size_t)(p - start);
        lx->pos  = p;
        return t;
    }

    /* ---- number: integer (dec/hex/bin/oct) or float ---- */
    if (is_digit(*p) ||
        (*p == '.' && is_digit(p[1])) ||
        (*p == '-' && (is_digit(p[1]) || p[1] == '.')) ||
        (*p == '+' && (is_digit(p[1]) || p[1] == '.'))) {

        const char *start = p;

        /* optional sign */
        if (*p == '+' || *p == '-') ++p;

        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
            /* hexadecimal */
            p += 2;
            while (is_xdigit(*p) || *p == '_') ++p;
        } else if (*p == '0' && (p[1] == 'b' || p[1] == 'B')) {
            /* binary */
            p += 2;
            while (*p == '0' || *p == '1' || *p == '_') ++p;
        } else {
            /* decimal integer or float */
            while (is_digit(*p) || *p == '_') ++p;
            if (*p == '.') {
                ++p;
                while (is_digit(*p) || *p == '_') ++p;
            }
            /* optional exponent: e / E / p / P followed by optional sign */
            if (*p == 'e' || *p == 'E' || *p == 'p' || *p == 'P') {
                ++p;
                if (*p == '+' || *p == '-') ++p;
                while (is_digit(*p)) ++p;
            }
        }

        /* optional suffix: f F l L u U and combinations */
        while (*p == 'f' || *p == 'F' ||
               *p == 'l' || *p == 'L' ||
               *p == 'u' || *p == 'U') {
            ++p;
        }

        t        = make_tok(TOK_NUMBER, start, line);
        t.len    = (size_t)(p - start);
        lx->pos  = p;
        return t;
    }

    /* ---- string literal ---- */
    if (*p == '"') {
        const char *start = p;
        ++p;    /* skip opening quote */
        while (*p && *p != '"' && *p != '\n') {
            if (*p == '\\') {
                ++p;    /* skip escape character */
                if (*p) ++p;
            } else {
                if (*p == '\n') ++lx->line;
                ++p;
            }
        }
        if (*p == '"') {
            ++p;    /* consume closing quote */
            t        = make_tok(TOK_STRING, start, line);
            t.len    = (size_t)(p - start);
            lx->pos  = p;
            return t;
        } else {
            /* unterminated: return whatever we have as an error */
            t        = make_tok(TOK_ERROR, start, line);
            t.len    = (size_t)(p - start);
            lx->pos  = p;
            return t;
        }
    }

    /* ---- punctuation: any other single character ---- */
    {
        t        = make_tok(TOK_PUNCT, p, line);
        t.len    = 1;
        lx->pos  = p + 1;
        return t;
    }
}

const char *tok_type_name(TokType t) {
    switch (t) {
        case TOK_EOF:     return "EOF";
        case TOK_ERROR:   return "ERROR";
        case TOK_IDENT:   return "IDENT";
        case TOK_NUMBER:  return "NUMBER";
        case TOK_STRING:  return "STRING";
        case TOK_PUNCT:   return "PUNCT";
        case TOK_NEWLINE: return "NEWLINE";
        default:          return "UNKNOWN";
    }
}
