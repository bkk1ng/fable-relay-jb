/*
 * lex.h — tokenizer for a small config language
 * C11, no external dependencies.
 */
#pragma once

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Token types                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    TOK_EOF,       /* end of input                                      */
    TOK_IDENT,     /* [A-Za-z_][A-Za-z0-9_]*                           */
    TOK_NUMBER,    /* integer, hex (0x…), or float (with . and/or e/E) */
    TOK_STRING,    /* "…" — raw source text including the quote chars   */
    TOK_PUNCT,     /* any single punctuation / operator character       */
    TOK_NEWLINE,   /* '\n'  (line feeds are returned as explicit tokens) */
    TOK_ERROR,     /* unterminated string literal or block comment      */
} TokenType;

/* ------------------------------------------------------------------ */
/* Token                                                               */
/* ------------------------------------------------------------------ */

/*
 * 'start' points into the caller's source buffer; the buffer must
 * remain valid for the lifetime of any token referencing it.
 * 'len'  is the byte count of the raw source text (string tokens
 *        include both enclosing double-quote characters).
 * 'line' is the 1-based line number on which the token begins.
 */
typedef struct {
    TokenType   type;
    const char *start;
    size_t      len;
    int         line;
} Token;

/* ------------------------------------------------------------------ */
/* Lexer state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *src;   /* pointer to start of source buffer (read-only) */
    const char *cur;   /* current read position                         */
    int         line;  /* 1-based line number at 'cur'                  */
} Lexer;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Initialise *l to scan the NUL-terminated string 'src'. */
void        lex_init      (Lexer *l, const char *src);

/*
 * Advance the lexer and return the next token.
 * - Whitespace (spaces, tabs, CR) is silently consumed between tokens.
 * - '//' comments are consumed to end-of-line; the '\n' that follows
 *   is still returned as TOK_NEWLINE.
 * - '/ *…* /' comments (including multi-line) are consumed entirely.
 *   Line numbers inside block comments are tracked correctly.
 * - An unterminated block comment or string literal yields TOK_ERROR
 *   whose 'start'/'len' span the bad text so the caller can report it.
 * - After TOK_EOF, every subsequent call returns TOK_EOF.
 */
Token       lex_next      (Lexer *l);

/* Return a short, stable C string naming the token type (for debugging). */
const char *tok_type_name (TokenType t);
