/* lex.h — tokenizer for a small config language (C11) */
#ifndef LEX_H
#define LEX_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Token types                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    TOK_EOF     = 0,  /* end of source                                 */
    TOK_ERROR,        /* unterminated string or block comment          */
    TOK_NEWLINE,      /* '\n'                                          */
    TOK_IDENT,        /* [a-zA-Z_][a-zA-Z0-9_]*                       */
    TOK_NUMBER,       /* integer or float, including exponent form     */
    TOK_STRING,       /* "..." with escape sequences                   */
    TOK_PUNCT,        /* any other single non-whitespace character     */
} TokenType;

/* ------------------------------------------------------------------ */
/* Token                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    TokenType   type;
    const char *start;  /* pointer into the original source buffer     */
    size_t      len;    /* byte length of the token in source          */
    int         line;   /* 1-based line number where token begins      */
} Token;

/* ------------------------------------------------------------------ */
/* Lexer state                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *src;   /* original source (NUL-terminated)            */
    const char *pos;   /* current scan position                        */
    int         line;  /* current 1-based line number                  */
} Lexer;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/* Initialise lex over a NUL-terminated source string.
   The string must remain valid for the lifetime of lex. */
void        lex_init(Lexer *lex, const char *src);

/* Return the next token.
   - Horizontal whitespace (' ', '\t', '\r') is silently consumed.
   - '//' and '/ * * /' comments are silently consumed.
   - '\n' is returned as TOK_NEWLINE (useful for line-sensitive parsing).
   - After TOK_EOF the function may be called repeatedly; it keeps
     returning TOK_EOF.
   - TOK_ERROR is returned for an unterminated string or block comment;
     the token covers everything consumed up to (and including) the
     current position. */
Token       lex_next(Lexer *lex);

/* Human-readable name for a token type (useful for diagnostics). */
const char *lex_type_name(TokenType type);

#endif /* LEX_H */
