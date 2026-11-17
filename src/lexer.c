#include "lexer.h"

#include <stdlib.h>
#include <string.h>

/* A growable byte buffer used while building segments. */
struct buf {
    char *s;
    size_t len, cap;
};

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q)
        abort();
    return q;
}

static void buf_push(struct buf *b, char c)
{
    if (b->len + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 16;
        b->s = xrealloc(b->s, b->cap);
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

/* State for the word currently being built. */
struct wbuilder {
    struct word *w;
    enum seg_kind kind; /* kind of the open segment */
    struct buf cur;
    int open;           /* a segment is in progress */
};

static void wb_flush(struct wbuilder *wb)
{
    if (!wb->open)
        return;
    struct word *w = wb->w;
    w->segs = xrealloc(w->segs, (w->nsegs + 1) * sizeof *w->segs);
    w->segs[w->nsegs].kind = wb->kind;
    w->segs[w->nsegs].text = wb->cur.s ? wb->cur.s : calloc(1, 1);
    if (!w->segs[w->nsegs].text)
        abort();
    w->nsegs++;
    wb->cur = (struct buf){0};
    wb->open = 0;
}

/* Start (or continue) a segment of the given kind. */
static void wb_begin(struct wbuilder *wb, enum seg_kind kind)
{
    if (!wb->w) {
        wb->w = calloc(1, sizeof *wb->w);
        if (!wb->w)
            abort();
    }
    if (wb->open && wb->kind == kind)
        return;
    wb_flush(wb);
    wb->kind = kind;
    wb->open = 1;
}

static void wb_add(struct wbuilder *wb, enum seg_kind kind, char c)
{
    wb_begin(wb, kind);
    buf_push(&wb->cur, c);
}

static void push_tok(struct tokens *t, size_t *cap, enum tok_type type,
                     struct word *w, size_t start, size_t end)
{
    if (t->n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        t->v = xrealloc(t->v, *cap * sizeof *t->v);
    }
    t->v[t->n++] = (struct token){type, w, start, end};
}

static int is_op_char(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '\n';
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Try to read an operator at s. Returns its length or 0. */
static size_t match_op(const char *s, enum tok_type *type)
{
    /* Only called at the start of a token, so "a2>f" lexes as "a2" > f. */
    if (s[0] == '2' && s[1] == '>') {
        if (s[2] == '&' && s[3] == '1') {
            *type = TOK_ERR_TO_OUT;
            return 4;
        }
        if (s[2] == '>') {
            *type = TOK_ERR_DGT;
            return 3;
        }
        *type = TOK_ERR_GT;
        return 2;
    }
    switch (s[0]) {
    case '|':
        *type = s[1] == '|' ? TOK_OR : TOK_PIPE;
        return s[1] == '|' ? 2 : 1;
    case '&':
        *type = s[1] == '&' ? TOK_AND : TOK_AMP;
        return s[1] == '&' ? 2 : 1;
    case '>':
        *type = s[1] == '>' ? TOK_DGT : TOK_GT;
        return s[1] == '>' ? 2 : 1;
    case ';': *type = TOK_SEMI; return 1;
    case '<': *type = TOK_LT; return 1;
    case '(': *type = TOK_LPAREN; return 1;
    case ')': *type = TOK_RPAREN; return 1;
    case '\n': *type = TOK_NEWLINE; return 1;
    }
    return 0;
}

int lex_line(const char *line, struct tokens *out, const char **err)
{
    struct tokens t = {0};
    size_t cap = 0;
    size_t i = 0;

    *err = NULL;
    for (;;) {
        while (is_blank(line[i]))
            i++;
        if (line[i] == '\0')
            break;
        if (line[i] == '#') { /* comment runs to end of line */
            while (line[i] && line[i] != '\n')
                i++;
            continue;
        }

        enum tok_type op;
        size_t oplen = match_op(line + i, &op);
        if (oplen) {
            push_tok(&t, &cap, op, NULL, i, i + oplen);
            i += oplen;
            continue;
        }

        /* A word: read until an unquoted blank or operator. */
        struct wbuilder wb = {0};
        size_t start = i;
        while (line[i] && !is_blank(line[i]) && !is_op_char(line[i])) {
            char c = line[i];
            if (c == '\\') {
                if (line[i + 1] == '\0') { /* trailing backslash: keep it */
                    wb_add(&wb, SEG_LITERAL, '\\');
                    i++;
                } else {
                    wb_add(&wb, SEG_LITERAL, line[i + 1]);
                    i += 2;
                }
            } else if (c == '\'') {
                size_t j = i + 1;
                while (line[j] && line[j] != '\'')
                    j++;
                if (!line[j])
                    goto unterminated;
                wb_begin(&wb, SEG_LITERAL);
                for (size_t k = i + 1; k < j; k++)
                    buf_push(&wb.cur, line[k]);
                i = j + 1;
            } else if (c == '"') {
                size_t j = i + 1;
                wb_begin(&wb, SEG_DQUOTE);
                while (line[j] && line[j] != '"') {
                    if (line[j] == '\\' &&
                        (line[j + 1] == '"' || line[j + 1] == '\\' ||
                         line[j + 1] == '$')) {
                        /* escaped char must not be expanded later */
                        wb_add(&wb, SEG_LITERAL, line[j + 1]);
                        j += 2;
                    } else {
                        wb_add(&wb, SEG_DQUOTE, line[j]);
                        j++;
                    }
                }
                if (!line[j])
                    goto unterminated;
                i = j + 1;
            } else {
                wb_add(&wb, SEG_BARE, c);
                i++;
            }
            continue;

        unterminated:
            wb_flush(&wb);
            word_free(wb.w);
            tokens_free(&t);
            *err = "unterminated quote";
            out->v = NULL;
            out->n = 0;
            return -1;
        }
        wb_flush(&wb);
        push_tok(&t, &cap, TOK_WORD, wb.w, start, i);
    }
    push_tok(&t, &cap, TOK_END, NULL, i, i);
    *out = t;
    return 0;
}

void word_free(struct word *w)
{
    if (!w)
        return;
    for (size_t i = 0; i < w->nsegs; i++)
        free(w->segs[i].text);
    free(w->segs);
    free(w);
}

void tokens_free(struct tokens *t)
{
    for (size_t i = 0; i < t->n; i++)
        word_free(t->v[i].word);
    free(t->v);
    t->v = NULL;
    t->n = 0;
}

char *word_text(const struct word *w)
{
    struct buf b = {0};
    for (size_t i = 0; i < w->nsegs; i++)
        for (const char *p = w->segs[i].text; *p; p++)
            buf_push(&b, *p);
    if (!b.s) {
        b.s = calloc(1, 1);
        if (!b.s)
            abort();
    }
    return b.s;
}

int word_is_bare(const struct word *w, const char *s)
{
    return w && w->nsegs == 1 && w->segs[0].kind == SEG_BARE &&
           strcmp(w->segs[0].text, s) == 0;
}

const char *tok_name(enum tok_type t)
{
    static const char *names[] = {
        [TOK_WORD] = "word", [TOK_PIPE] = "|",    [TOK_AND] = "&&",
        [TOK_OR] = "||",     [TOK_SEMI] = ";",    [TOK_AMP] = "&",
        [TOK_LT] = "<",      [TOK_GT] = ">",      [TOK_DGT] = ">>",
        [TOK_ERR_GT] = "2>", [TOK_ERR_DGT] = "2>>",
        [TOK_ERR_TO_OUT] = "2>&1",
        [TOK_LPAREN] = "(",  [TOK_RPAREN] = ")",  [TOK_NEWLINE] = "newline",
        [TOK_END] = "newline",
    };
    return names[t];
}
