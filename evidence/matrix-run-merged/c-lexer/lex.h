/*
 * lex.h — tokenizer for a small config language (C11)
 *
 * Token types:  identifier, number (integer / float / hex), string literal
 *               (with escape sequences), punctuation, newline, EOF, error.
 * Comments:     // line comments and /* */ block comments are consumed and
 *               not surfaced as tokens; embedded newlines still increment the
 *               line counter.
 */

#ifndef LEX_H
#define LEX_H

#include <stddef.h>

/* ------------------------------------------------------------------ types */

typedef enum {
    TOK_EOF,       /* end of source                                        */
    TOK_ERROR,     /* unterminated string or block comment                 */
    TOK_NEWLINE,   /* '\n'  (significant in many config formats)           */
    TOK_IDENT,     /* [A-Za-z_][A-Za-z0-9_]*                              */
    TOK_NUMBER,    /* integer, hex (0x…), or float with optional exponent  */
    TOK_STRING,    /* "…" with \-escape sequences; includes the quotes     */
    TOK_PUNCT,     /* any single printable character not matched above     */
} TokType;

/*
 * Token — the value returned by lex_next().
 *
 * `start` points directly into the original source buffer; it is NOT
 * NUL-terminated.  Use `len` for all bounds.  For TOK_EOF the length is 0.
 */
typedef struct {
    TokType     type;
    const char *start;  /* first byte of the lexeme in the source buffer  */
    size_t      len;    /* byte length of the lexeme                       */
    int         line;   /* 1-based line number where the token begins      */
} Token;

/* ----------------------------------------------------------------- lexer  */

typedef struct {
    const char *cursor; /* current read position in the source buffer     */
    int         line;   /* 1-based line counter                            */
} Lexer;

/*
 * lex_init — bind a Lexer to a NUL-terminated source string.
 *
 * The source buffer must outlive all subsequent lex_next() calls; no copy
 * is made.
 */
void lex_init(Lexer *lx, const char *src);

/*
 * lex_next — consume and return the next token.
 *
 * Leading whitespace (spaces, tabs, carriage returns) and comments are
 * silently discarded before each token.  On an unterminated string or block
 * comment the returned type is TOK_ERROR; the cursor is left at EOF so the
 * next call returns TOK_EOF.
 */
Token lex_next(Lexer *lx);

/*
 * tok_type_name — return a static string name for a TokType value.
 *
 * Useful for diagnostics.  Never returns NULL.
 */
const char *tok_type_name(TokType t);

#endif /* LEX_H */
