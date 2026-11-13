/*
 * lex.h — tokenizer for a small config language
 * C11
 */
#ifndef LEX_H
#define LEX_H

#include <stddef.h>

/* ------------------------------------------------------------------ types */

typedef enum {
    TOK_EOF     = 0,  /* end of input                                       */
    TOK_ERROR,        /* unrecognised byte or unterminated literal/comment  */
    TOK_IDENT,        /* [A-Za-z_][A-Za-z0-9_]*                            */
    TOK_NUMBER,       /* integer or float: 42  3.14  1.5e-3                 */
    TOK_STRING,       /* "..." with backslash escapes                       */
    TOK_PUNCT,        /* any single punctuation/operator character          */
    TOK_COMMENT,      /* // … \n   or   /* … */  (span includes delimiters) */
    TOK_NEWLINE,      /* \n  (bare \r is skipped as whitespace)             */
} TokType;

typedef struct {
    TokType     type;
    const char *start; /* pointer into the source buffer; NOT NUL-terminated */
    size_t      len;   /* byte length of the token text                      */
    int         line;  /* 1-based source line where the token begins         */
} Token;

typedef struct {
    const char *src;  /* original source string (kept for reference)        */
    const char *cur;  /* current read position                               */
    int         line; /* current line number (1-based)                       */
} Lexer;

/* --------------------------------------------------------------- interface */

/* Initialise *lx over a NUL-terminated source string. */
void        lex_init(Lexer *lx, const char *src);

/*
 * Return the next token from *lx.
 * - Horizontal whitespace (space, tab, bare \r) is silently skipped.
 * - \n produces TOK_NEWLINE and advances the line counter.
 * - TOK_EOF is returned repeatedly once input is exhausted.
 * - TOK_ERROR is returned for an unrecognised byte, an unterminated string,
 *   or an unterminated block comment; len covers the bytes consumed.
 */
Token       lex_next(Lexer *lx);

/* Return a short human-readable name for a token type (for debugging). */
const char *tok_type_name(TokType t);

#endif /* LEX_H */
