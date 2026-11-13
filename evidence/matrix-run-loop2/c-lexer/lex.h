/*
 * lex.h — tokenizer for a small config language (C11)
 *
 * Token layout:
 *   start  - pointer into the original source string
 *   len    - byte length of the token (for strings, includes the quotes;
 *             for numbers, includes any decimal point and exponent)
 *   line   - 1-based line number where the token begins
 *
 * Comments (// and / * * /) are consumed silently.
 * Newlines are reported as TOK_NEWLINE so callers can track statement ends.
 * On an unterminated string or block comment, lex_next returns TOK_ERROR
 * with start/len spanning the bad input.
 */

#ifndef LEX_H
#define LEX_H

#include <stddef.h>

/* ------------------------------------------------------------------ types */

typedef enum {
    TOK_EOF,       /* end of input                                  */
    TOK_ERROR,     /* unterminated string or block comment          */
    TOK_IDENT,     /* [A-Za-z_][A-Za-z0-9_]*                       */
    TOK_NUMBER,    /* integer or float, decimal; optional exponent  */
    TOK_STRING,    /* "..." with backslash escapes; quotes included  */
    TOK_PUNCT,     /* any single non-whitespace, non-quote char     */
    TOK_NEWLINE,   /* \n (line counter is incremented before return) */
} TokType;

typedef struct {
    TokType     type;
    const char *start;  /* pointer into source; NOT NUL-terminated  */
    size_t      len;
    int         line;
} Token;

/* Lexer state — initialise once, then call lex_next repeatedly. */
typedef struct {
    const char *src;    /* original source, kept for reference       */
    const char *pos;    /* current scan position                     */
    int         line;   /* current line (starts at 1)                */
} Lexer;

/* ------------------------------------------------------------------ API */

/* Initialise *lex to scan src (must remain valid for the lexer's lifetime). */
void  lex_init(Lexer *lex, const char *src);

/* Return the next token, advancing the lexer past it. */
Token lex_next(Lexer *lex);

/* Human-readable name for a TokType, useful for diagnostics. */
const char *tok_type_name(TokType t);

#endif /* LEX_H */
