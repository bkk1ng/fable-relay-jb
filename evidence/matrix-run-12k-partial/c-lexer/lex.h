/*
 * lex.h — tokenizer for a small config language
 *
 * Token types: identifiers, numbers (int and float), string literals
 * with escape sequences, single-character punctuation, and newlines.
 * Comments (// and / * * /) are skipped and do not produce tokens.
 */
#ifndef LEX_H
#define LEX_H

#include <stddef.h>

typedef enum {
    TOKEN_EOF,      /* end of input                        */
    TOKEN_IDENT,    /* [A-Za-z_][A-Za-z0-9_]*              */
    TOKEN_NUMBER,   /* integer or floating-point literal    */
    TOKEN_STRING,   /* "..." with backslash escape sequences*/
    TOKEN_PUNCT,    /* single-character punctuation         */
    TOKEN_NEWLINE,  /* \n                                   */
} TokenType;

typedef struct {
    TokenType   type;
    const char *start;   /* pointer into the source text      */
    size_t      length;  /* number of bytes the token spans    */
    int         line;    /* 1-based line number               */
} Token;

typedef struct {
    const char *input;    /* NUL-terminated source text       */
    size_t      input_len; /* byte length (excluding the NUL)  */
    size_t      pos;       /* current byte offset into input    */
    int         line;      /* current 1-based line              */
} Lexer;

/* Initialise a lexer over a NUL-terminated source string. */
void lex_init(Lexer *lx, const char *input);

/*
 * Scan and return the next token.  Repeated calls after the input
 * is exhausted yield TOKEN_EOF with length 0.
 */
Token lex_next(Lexer *lx);

#endif /* LEX_H */
