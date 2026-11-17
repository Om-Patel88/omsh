#ifndef OMSH_LEXER_H
#define OMSH_LEXER_H

#include <stddef.h>

/*
 * A word is a list of segments. The kind of a segment tells expansion what
 * it may do with the text:
 *   SEG_BARE     unquoted: variables, tilde and globbing apply
 *   SEG_DQUOTE   inside "...": variables only
 *   SEG_LITERAL  single quoted or backslash escaped: used as is
 *   SEG_SUBST    body of an unquoted $(...): run, then field split
 *   SEG_DQSUBST  body of a $(...) inside "...": run, kept as one field
 */
enum seg_kind { SEG_BARE, SEG_DQUOTE, SEG_LITERAL, SEG_SUBST, SEG_DQSUBST };

struct seg {
    enum seg_kind kind;
    char *text;
};

struct word {
    struct seg *segs;
    size_t nsegs;
};

enum tok_type {
    TOK_WORD,
    TOK_PIPE,       /* |    */
    TOK_AND,        /* &&   */
    TOK_OR,         /* ||   */
    TOK_SEMI,       /* ;    */
    TOK_AMP,        /* &    */
    TOK_LT,         /* <    */
    TOK_GT,         /* >    */
    TOK_DGT,        /* >>   */
    TOK_ERR_GT,     /* 2>   */
    TOK_ERR_DGT,    /* 2>>  */
    TOK_ERR_TO_OUT, /* 2>&1 */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_NEWLINE,    /* an unquoted newline separates commands like ; */
    TOK_END
};

struct token {
    enum tok_type type;
    struct word *word; /* only for TOK_WORD; NULL once the parser takes it */
    size_t start, end; /* byte range of the token in the input line */
};

struct tokens {
    struct token *v;
    size_t n;
};

/*
 * Split line into tokens. The list always ends with TOK_END.
 * Returns 0 on success. On a syntax error returns -1, sets *err to a static
 * message and leaves out empty. The only errors are input that ends inside
 * a quote or $(...), so -1 also means another line could finish it.
 */
int lex_line(const char *line, struct tokens *out, const char **err);
void tokens_free(struct tokens *t);

void word_free(struct word *w);
/* All segment text joined together, ignoring quoting. Caller frees. */
char *word_text(const struct word *w);
/* True if the word is exactly the given unquoted text. */
int word_is_bare(const struct word *w, const char *s);

const char *tok_name(enum tok_type t);

#endif
