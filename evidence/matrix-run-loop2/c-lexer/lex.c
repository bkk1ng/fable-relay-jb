/*
 * lex.c — tokenizer implementation for a small config language (C11)
 */

#include "lex.h"

#include <stddef.h>

/* ---------------------------------------------------------------- helpers */

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_alnum(char c)
{
    return is_alpha(c) || is_digit(c);
}

/* Advance past spaces, tabs, and carriage returns (not newlines). */
static void skip_hspace(Lexer *lex)
{
    while (*lex->pos == ' ' || *lex->pos == '\t' || *lex->pos == '\r')
        lex->pos++;
}

/* ------------------------------------------------------------------ init */

void lex_init(Lexer *lex, const char *src)
{
    lex->src  = src;
    lex->pos  = src;
    lex->line = 1;
}

/* --------------------------------------------------------------- tok name */

const char *tok_type_name(TokType t)
{
    switch (t) {
    case TOK_EOF:     return "EOF";
    case TOK_ERROR:   return "ERROR";
    case TOK_IDENT:   return "IDENT";
    case TOK_NUMBER:  return "NUMBER";
    case TOK_STRING:  return "STRING";
    case TOK_PUNCT:   return "PUNCT";
    case TOK_NEWLINE: return "NEWLINE";
    }
    return "UNKNOWN";
}

/* --------------------------------------------------------------- lex_next */

Token lex_next(Lexer *lex)
{
    Token tok;

restart:
    skip_hspace(lex);

    tok.line  = lex->line;
    tok.start = lex->pos;
    tok.len   = 0;

    char c = *lex->pos;

    /* ---- end of input ---- */
    if (c == '\0') {
        tok.type = TOK_EOF;
        tok.len  = 0;
        return tok;
    }

    /* ---- newline ---- */
    if (c == '\n') {
        lex->line++;
        lex->pos++;
        tok.type = TOK_NEWLINE;
        tok.len  = 1;
        return tok;
    }

    /* ---- comments ---- */
    if (c == '/' && (lex->pos[1] == '/' || lex->pos[1] == '*')) {
        if (lex->pos[1] == '/') {
            /* single-line: consume through end of line, but NOT the \n */
            lex->pos += 2;
            while (*lex->pos != '\0' && *lex->pos != '\n')
                lex->pos++;
            goto restart;
        } else {
            /* block comment: consume until closing */
            lex->pos += 2;
            while (*lex->pos != '\0') {
                if (*lex->pos == '\n') {
                    lex->line++;
                    lex->pos++;
                } else if (*lex->pos == '*' && lex->pos[1] == '/') {
                    lex->pos += 2;
                    goto restart;
                } else {
                    lex->pos++;
                }
            }
            /* unterminated block comment */
            tok.type = TOK_ERROR;
            tok.len  = (size_t)(lex->pos - tok.start);
            return tok;
        }
    }

    /* ---- identifier ---- */
    if (is_alpha(c)) {
        tok.type = TOK_IDENT;
        while (is_alnum(*lex->pos))
            lex->pos++;
        tok.len = (size_t)(lex->pos - tok.start);
        return tok;
    }

    /* ---- number (integer or float) ----
     *
     * Grammar accepted:
     *   digit+
     *   digit+ '.' digit*
     *   digit* '.' digit+
     *   either form followed by [eE][+-]?digit+
     *
     * Leading minus is left to the parser so "x-1" tokenises as
     * IDENT PUNCT NUMBER rather than IDENT NUMBER.
     */
    if (is_digit(c) || (c == '.' && is_digit(lex->pos[1]))) {
        tok.type = TOK_NUMBER;

        /* integer part */
        while (is_digit(*lex->pos))
            lex->pos++;

        /* optional fractional part */
        if (*lex->pos == '.' && is_digit(lex->pos[1])) {
            lex->pos++; /* consume '.' */
            while (is_digit(*lex->pos))
                lex->pos++;
        }

        /* optional exponent */
        if (*lex->pos == 'e' || *lex->pos == 'E') {
            lex->pos++;
            if (*lex->pos == '+' || *lex->pos == '-')
                lex->pos++;
            while (is_digit(*lex->pos))
                lex->pos++;
        }

        tok.len = (size_t)(lex->pos - tok.start);
        return tok;
    }

    /* ---- string literal ---- */
    if (c == '"') {
        tok.type = TOK_STRING;
        lex->pos++; /* step past opening quote */

        for (;;) {
            char s = *lex->pos;

            if (s == '\0' || s == '\n') {
                /* unterminated string */
                tok.type = TOK_ERROR;
                tok.len  = (size_t)(lex->pos - tok.start);
                return tok;
            }

            if (s == '\\') {
                /* consume backslash plus the escaped character.
                 * Escape validation (\\, \", \n, \t, \r, \0, \xNN ...)
                 * is deferred to the parser/interpreter. */
                lex->pos++;
                if (*lex->pos == '\0') {
                    tok.type = TOK_ERROR;
                    tok.len  = (size_t)(lex->pos - tok.start);
                    return tok;
                }
                lex->pos++;
                continue;
            }

            if (s == '"') {
                lex->pos++; /* consume closing quote */
                break;
            }

            lex->pos++;
        }

        tok.len = (size_t)(lex->pos - tok.start);
        return tok;
    }

    /* ---- punctuation: any single remaining printable character ---- */
    tok.type = TOK_PUNCT;
    lex->pos++;
    tok.len = 1;
    return tok;
}
