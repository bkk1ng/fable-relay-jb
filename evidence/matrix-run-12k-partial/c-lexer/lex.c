/*
 * lex.c — implementation of the config-language tokenizer
 *
 * Build (C11):
 *   cc -std=c11 -Wall -Wextra -c lex.c
 */
#include "lex.h"

#include <ctype.h>
#include <string.h>

void
lex_init(Lexer *lx, const char *input)
{
    lx->input     = input;
    lx->input_len = strlen(input);
    lx->pos       = 0;
    lx->line      = 1;
}

Token
lex_next(Lexer *lx)
{
    /*
     * Skip horizontal whitespace and comments before every token.
     * Newlines are NOT skipped here — they are returned as
     * TOKEN_NEWLINE so the parser can treat them as statement
     * terminators if it wants to.
     */
    for (;;) {
        /* spaces, tabs, carriage returns */
        while (lx->pos < lx->input_len) {
            char c = lx->input[lx->pos];
            if (c == ' ' || c == '\t' || c == '\r')
                lx->pos++;
            else
                break;
        }

        /* line comment  // ... end-of-line */
        if (lx->pos + 1 < lx->input_len
            && lx->input[lx->pos]     == '/'
            && lx->input[lx->pos + 1] == '/')
        {
            lx->pos += 2;
            while (lx->pos < lx->input_len && lx->input[lx->pos] != '\n')
                lx->pos++;
            continue;
        }

        /* block comment  / * ... * /  (no nesting) */
        if (lx->pos + 1 < lx->input_len
            && lx->input[lx->pos]     == '/'
            && lx->input[lx->pos + 1] == '*')
        {
            lx->pos += 2;
            int closed = 0;
            while (lx->pos + 1 < lx->input_len) {
                if (lx->input[lx->pos]     == '*'
                    && lx->input[lx->pos + 1] == '/')
                {
                    lx->pos += 2;
                    closed = 1;
                    break;
                }
                if (lx->input[lx->pos] == '\n')
                    lx->line++;
                lx->pos++;
            }
            if (!closed) {
                /* unterminated block comment — consume the rest */
                while (lx->pos < lx->input_len) {
                    if (lx->input[lx->pos] == '\n')
                        lx->line++;
                    lx->pos++;
                }
            }
            continue;
        }

        break;
    }

    /* ---- end of input ------------------------------------------------ */
    if (lx->pos >= lx->input_len) {
        Token tok = { TOKEN_EOF, lx->input + lx->pos, 0, lx->line };
        return tok;
    }

    /* ---- newline ----------------------------------------------------- */
    if (lx->input[lx->pos] == '\n') {
        int line = lx->line;
        const char *start = lx->input + lx->pos;
        lx->pos++;
        lx->line++;
        Token tok = { TOKEN_NEWLINE, start, 1, line };
        return tok;
    }

    char c = lx->input[lx->pos];

    /* ---- identifier  [A-Za-z_][A-Za-z0-9_]* ------------------------- */
    if (isalpha((unsigned char)c) || c == '_') {
        size_t      start = lx->pos;
        int         line  = lx->line;
        while (lx->pos < lx->input_len) {
            char ch = lx->input[lx->pos];
            if (isalnum((unsigned char)ch) || ch == '_')
                lx->pos++;
            else
                break;
        }
        Token tok = { TOKEN_IDENT, lx->input + start, lx->pos - start, line };
        return tok;
    }

    /* ---- number  -----------------------------------------------------
     *
     * Accepted forms:
     *   123        integer
     *   123.456    float
     *   123.       float (trailing dot)
     *   .456       float (leading dot)
     *   1e10       integer with exponent
     *   1.5e-3     float with exponent
     *
     * The parser can tell int from float by scanning the span for
     * '.', 'e', or 'E'.
     */
    if (isdigit((unsigned char)c)
        || (c == '.'
            && lx->pos + 1 < lx->input_len
            && isdigit((unsigned char)lx->input[lx->pos + 1])))
    {
        size_t start = lx->pos;
        int    line  = lx->line;

        /* integer part */
        while (lx->pos < lx->input_len
               && isdigit((unsigned char)lx->input[lx->pos]))
            lx->pos++;

        /* fractional part */
        if (lx->pos < lx->input_len && lx->input[lx->pos] == '.') {
            lx->pos++;
            while (lx->pos < lx->input_len
                   && isdigit((unsigned char)lx->input[lx->pos]))
                lx->pos++;
        }

        /* exponent part  [eE][+-]?[0-9]+ */
        if (lx->pos < lx->input_len
            && (lx->input[lx->pos] == 'e' || lx->input[lx->pos] == 'E'))
        {
            size_t save = lx->pos;
            lx->pos++;
            if (lx->pos < lx->input_len
                && (lx->input[lx->pos] == '+' || lx->input[lx->pos] == '-'))
                lx->pos++;
            if (lx->pos < lx->input_len
                && isdigit((unsigned char)lx->input[lx->pos]))
            {
                while (lx->pos < lx->input_len
                       && isdigit((unsigned char)lx->input[lx->pos]))
                    lx->pos++;
            } else {
                /* not a valid exponent — rewind so 'e' is its own token */
                lx->pos = save;
            }
        }

        Token tok = { TOKEN_NUMBER, lx->input + start, lx->pos - start, line };
        return tok;
    }

    /* ---- string literal  "..."  with \-escapes ---------------------- */
    if (c == '"') {
        size_t start = lx->pos;
        int    line  = lx->line;
        lx->pos++;   /* consume opening quote */

        while (lx->pos < lx->input_len) {
            char ch = lx->input[lx->pos];
            if (ch == '\\' && lx->pos + 1 < lx->input_len) {
                /* escape: skip backslash + the escaped character */
                if (lx->input[lx->pos + 1] == '\n')
                    lx->line++;
                lx->pos += 2;
            } else if (ch == '"') {
                lx->pos++;   /* consume closing quote */
                break;
            } else if (ch == '\n') {
                /* unterminated string — stop at the newline boundary */
                break;
            } else {
                lx->pos++;
            }
        }

        Token tok = { TOKEN_STRING, lx->input + start, lx->pos - start, line };
        return tok;
    }

    /* ---- punctuation  (any other single character) ------------------ */
    {
        size_t start = lx->pos;
        int    line  = lx->line;
        lx->pos++;
        Token tok = { TOKEN_PUNCT, lx->input + start, 1, line };
        return tok;
    }
}
