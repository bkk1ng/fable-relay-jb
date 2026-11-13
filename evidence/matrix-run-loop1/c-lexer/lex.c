/*
 * lex.c — tokenizer for a small config language
 * C11
 */
#include "lex.h"

#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- helpers */

static inline bool is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static inline bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static inline bool is_alnum(char c)
{
    return is_alpha(c) || is_digit(c);
}

/* Horizontal whitespace only — newlines are tokens, not noise. */
static inline bool is_hspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static inline bool is_punct(char c)
{
    /*
     * Anything that is not a letter, digit, whitespace, NUL, or the two
     * characters that start comments (//) is punctuation.  The slash itself
     * still needs special treatment because it may start a comment.
     */
    if (c == '\0' || c == '\n' || is_alpha(c) || is_digit(c) || is_hspace(c))
        return false;
    return true;
}

/* ------------------------------------------------- public implementation */

void lex_init(Lexer *lx, const char *src)
{
    lx->src  = src;
    lx->cur  = src;
    lx->line = 1;
}

Token lex_next(Lexer *lx)
{
    /* Skip horizontal whitespace. */
    while (is_hspace(*lx->cur))
        lx->cur++;

    const char *start = lx->cur;
    int         line  = lx->line;
    char        c     = *lx->cur;

    /* ---- EOF ---- */
    if (c == '\0') {
        return (Token){ TOK_EOF, start, 0, line };
    }

    /* ---- Newline ---- */
    if (c == '\n') {
        lx->cur++;
        lx->line++;
        return (Token){ TOK_NEWLINE, start, 1, line };
    }

    /* ---- Comments ---- */
    if (c == '/' && lx->cur[1] == '/') {
        /* Line comment: consume up to (but not including) the newline. */
        lx->cur += 2;
        while (*lx->cur != '\0' && *lx->cur != '\n')
            lx->cur++;
        return (Token){ TOK_COMMENT, start, (size_t)(lx->cur - start), line };
    }

    if (c == '/' && lx->cur[1] == '*') {
        /* Block comment: consume until closing "*/". */
        lx->cur += 2;
        while (*lx->cur != '\0') {
            if (*lx->cur == '\n') {
                lx->line++;
                lx->cur++;
            } else if (*lx->cur == '*' && lx->cur[1] == '/') {
                lx->cur += 2;
                return (Token){ TOK_COMMENT, start,
                                (size_t)(lx->cur - start), line };
            } else {
                lx->cur++;
            }
        }
        /* Reached EOF without closing delimiter. */
        return (Token){ TOK_ERROR, start, (size_t)(lx->cur - start), line };
    }

    /* ---- String literal ---- */
    if (c == '"') {
        lx->cur++; /* skip opening quote */
        while (*lx->cur != '\0' && *lx->cur != '"') {
            if (*lx->cur == '\\') {
                lx->cur++; /* skip the escape introducer */
                if (*lx->cur == '\0')
                    break;  /* will be caught by the EOF check below */
                if (*lx->cur == '\n')
                    lx->line++;
            } else if (*lx->cur == '\n') {
                /* Bare newline inside a string — treat as unterminated. */
                break;
            }
            lx->cur++;
        }
        if (*lx->cur != '"') {
            /* Unterminated string. */
            return (Token){ TOK_ERROR, start,
                            (size_t)(lx->cur - start), line };
        }
        lx->cur++; /* skip closing quote */
        return (Token){ TOK_STRING, start, (size_t)(lx->cur - start), line };
    }

    /* ---- Number: integer or float ---- */
    /*
     * Grammar (simplified):
     *   digit+ ( '.' digit+ )? ( [eE] [+-]? digit+ )?
     */
    if (is_digit(c) || (c == '-' && is_digit(lx->cur[1])) ||
        (c == '+' && is_digit(lx->cur[1])))
    {
        if (c == '-' || c == '+')
            lx->cur++;
        while (is_digit(*lx->cur))
            lx->cur++;
        if (*lx->cur == '.' && is_digit(lx->cur[1])) {
            lx->cur++; /* skip '.' */
            while (is_digit(*lx->cur))
                lx->cur++;
        }
        if (*lx->cur == 'e' || *lx->cur == 'E') {
            lx->cur++;
            if (*lx->cur == '+' || *lx->cur == '-')
                lx->cur++;
            while (is_digit(*lx->cur))
                lx->cur++;
        }
        return (Token){ TOK_NUMBER, start, (size_t)(lx->cur - start), line };
    }

    /* ---- Identifier ---- */
    if (is_alpha(c)) {
        lx->cur++;
        while (is_alnum(*lx->cur))
            lx->cur++;
        return (Token){ TOK_IDENT, start, (size_t)(lx->cur - start), line };
    }

    /* ---- Punctuation (single-character) ---- */
    if (is_punct(c)) {
        lx->cur++;
        return (Token){ TOK_PUNCT, start, 1, line };
    }

    /* ---- Unrecognised byte ---- */
    lx->cur++;
    return (Token){ TOK_ERROR, start, 1, line };
}

/* --------------------------------------------------- diagnostic helper */

const char *tok_type_name(TokType t)
{
    switch (t) {
        case TOK_EOF:     return "EOF";
        case TOK_ERROR:   return "ERROR";
        case TOK_IDENT:   return "IDENT";
        case TOK_NUMBER:  return "NUMBER";
        case TOK_STRING:  return "STRING";
        case TOK_PUNCT:   return "PUNCT";
        case TOK_COMMENT: return "COMMENT";
        case TOK_NEWLINE: return "NEWLINE";
        default:          return "UNKNOWN";
    }
}
