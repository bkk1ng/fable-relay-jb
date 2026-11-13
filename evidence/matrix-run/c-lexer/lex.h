/*
 * lex.h — tokenizer for a small config language (C11)
 *
 * Token types:
 *   TOK_IDENT    — identifier: [A-Za-z_][A-Za-z0-9_]*
 *   TOK_NUMBER   — integer or float literal (dec/hex/bin, optional exponent/suffix)
 *   TOK_STRING   — double-quoted string literal with C escape sequences
 *   TOK_PUNCT    — any single punctuation character not consumed above
 *   TOK_NEWLINE  — '\n' (useful for line-delimited config syntax)
 *   TOK_EOF      — end of input
 *   TOK_ERROR    — unterminated string or invalid input; start/len span the bad text
 *
 * Comments (// and /* ... * /) are consumed silently; they produce no token.
 *
 * Token.start is a pointer into the original source buffer and is NOT
 * NUL-terminated.  Token.len gives the byte count.  Caller must not free it.
 */

#ifndef LEX_H
#define LEX_H

#include <stddef.h>

typedef enum {
    TOK_EOF,
    TOK_ERROR,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_PUNCT,
    TOK_NEWLINE,
} TokType;

typedef struct {
    TokType     type;
    const char *start;  /* pointer into source, not NUL-terminated */
    size_t      len;
    int         line;   /* 1-based line number of the token's first character */
} Token;

typedef struct {
    const char *pos;    /* current scan position */
    int         line;   /* current 1-based line number */
} Lexer;

/*
 * lex_init — initialise *lx to scan src.
 * src must remain valid for the lifetime of lx and any Token values it
 * produces (tokens hold pointers into src).
 */
void  lex_init(Lexer *lx, const char *src);

/*
 * lex_next — return the next token and advance the lexer.
 * Returns TOK_EOF once the input is exhausted; calling again is safe.
 */
Token lex_next(Lexer *lx);

/* tok_type_name — printable name of a TokType value, for diagnostics. */
const char *tok_type_name(TokType t);

#endif /* LEX_H */
