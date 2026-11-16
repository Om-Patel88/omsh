#include "parser.h"

#include <stdlib.h>
#include <string.h>

struct parser {
    struct tokens *t;
    const char *line;
    size_t pos;
    const char *err; /* set on the first syntax error */
};

static void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p)
        abort();
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q)
        abort();
    return q;
}

static enum tok_type peek(struct parser *p)
{
    return p->t->v[p->pos].type;
}

static int fail(struct parser *p)
{
    if (!p->err)
        p->err = tok_name(peek(p));
    return -1;
}

static int is_redirect(enum tok_type t)
{
    return t == TOK_LT || t == TOK_GT || t == TOK_DGT || t == TOK_ERR_GT ||
           t == TOK_ERR_DGT || t == TOK_ERR_TO_OUT;
}

static struct node *new_node(enum node_kind kind)
{
    struct node *n = xcalloc(1, sizeof *n);
    n->kind = kind;
    return n;
}

static struct node *parse_list(struct parser *p, enum tok_type end);

/* Parse one redirect and append it to *rv. */
static int parse_redirect(struct parser *p, struct redir **rv, size_t *rn)
{
    enum redir_type type;
    switch (peek(p)) {
    case TOK_LT: type = R_IN; break;
    case TOK_GT: type = R_OUT; break;
    case TOK_DGT: type = R_APPEND; break;
    case TOK_ERR_GT: type = R_ERR; break;
    case TOK_ERR_DGT: type = R_ERR_APPEND; break;
    default: type = R_ERR_TO_OUT; break;
    }
    p->pos++;

    struct word *target = NULL;
    if (type != R_ERR_TO_OUT) {
        if (peek(p) != TOK_WORD)
            return fail(p);
        target = p->t->v[p->pos].word;
        p->t->v[p->pos].word = NULL;
        p->pos++;
    }
    *rv = xrealloc(*rv, (*rn + 1) * sizeof **rv);
    (*rv)[(*rn)++] = (struct redir){type, target};
    return 0;
}

static struct node *parse_command(struct parser *p)
{
    struct node *n;

    if (peek(p) == TOK_LPAREN) {
        p->pos++;
        n = new_node(N_SUBSHELL);
        n->sub.body = parse_list(p, TOK_RPAREN);
        if (!n->sub.body || peek(p) != TOK_RPAREN) {
            fail(p);
            ast_free(n);
            return NULL;
        }
        p->pos++;
        while (is_redirect(peek(p))) {
            if (parse_redirect(p, &n->sub.redirs, &n->sub.nredirs) < 0) {
                ast_free(n);
                return NULL;
            }
        }
        return n;
    }

    n = new_node(N_SIMPLE);
    for (;;) {
        enum tok_type t = peek(p);
        if (t == TOK_WORD) {
            n->simple.words = xrealloc(n->simple.words,
                (n->simple.nwords + 1) * sizeof *n->simple.words);
            n->simple.words[n->simple.nwords++] = p->t->v[p->pos].word;
            p->t->v[p->pos].word = NULL;
            p->pos++;
        } else if (is_redirect(t)) {
            if (parse_redirect(p, &n->simple.redirs, &n->simple.nredirs) < 0) {
                ast_free(n);
                return NULL;
            }
        } else {
            break;
        }
    }
    if (n->simple.nwords == 0 && n->simple.nredirs == 0) {
        fail(p);
        ast_free(n);
        return NULL;
    }
    return n;
}

static struct node *parse_pipeline(struct parser *p)
{
    struct node *n = new_node(N_PIPELINE);

    /* '!' is an ordinary word to the lexer; unquoted at the start of a
     * pipeline it means negation. */
    if (peek(p) == TOK_WORD && word_is_bare(p->t->v[p->pos].word, "!")) {
        n->pipe.negate = true;
        p->pos++;
    }
    for (;;) {
        struct node *c = parse_command(p);
        if (!c) {
            ast_free(n);
            return NULL;
        }
        n->pipe.cmds = xrealloc(n->pipe.cmds,
                                (n->pipe.n + 1) * sizeof *n->pipe.cmds);
        n->pipe.cmds[n->pipe.n++] = c;
        if (peek(p) != TOK_PIPE)
            break;
        p->pos++;
    }
    return n;
}

static struct node *parse_and_or(struct parser *p)
{
    struct node *left = parse_pipeline(p);
    if (!left)
        return NULL;
    while (peek(p) == TOK_AND || peek(p) == TOK_OR) {
        struct node *n = new_node(peek(p) == TOK_AND ? N_AND : N_OR);
        p->pos++;
        n->bin.left = left;
        n->bin.right = parse_pipeline(p);
        if (!n->bin.right) {
            ast_free(n);
            return NULL;
        }
        left = n;
    }
    return left;
}

static char *slice(const char *s, size_t start, size_t end)
{
    char *r = xcalloc(1, end - start + 1);
    memcpy(r, s + start, end - start);
    return r;
}

static struct node *parse_list(struct parser *p, enum tok_type end)
{
    struct node *n = new_node(N_LIST);

    for (;;) {
        size_t first = p->pos;
        struct node *item = parse_and_or(p);
        if (!item) {
            ast_free(n);
            return NULL;
        }
        n->list.items = xrealloc(n->list.items,
                                 (n->list.n + 1) * sizeof *n->list.items);
        struct list_item *li = &n->list.items[n->list.n++];
        li->node = item;
        li->background = false;
        li->text = slice(p->line, p->t->v[first].start,
                         p->t->v[p->pos - 1].end);

        enum tok_type t = peek(p);
        if (t == TOK_SEMI || t == TOK_AMP) {
            li->background = t == TOK_AMP;
            p->pos++;
            if (peek(p) == end)
                break;
        } else if (t == end) {
            break;
        } else {
            fail(p);
            ast_free(n);
            return NULL;
        }
    }
    return n;
}

struct node *parse(struct tokens *t, const char *line, const char **err)
{
    struct parser p = {t, line, 0, NULL};

    *err = NULL;
    if (peek(&p) == TOK_END)
        return NULL;
    struct node *n = parse_list(&p, TOK_END);
    if (!n)
        *err = p.err ? p.err : "newline";
    return n;
}

static void free_redirs(struct redir *r, size_t n)
{
    for (size_t i = 0; i < n; i++)
        word_free(r[i].target);
    free(r);
}

void ast_free(struct node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case N_LIST:
        for (size_t i = 0; i < n->list.n; i++) {
            ast_free(n->list.items[i].node);
            free(n->list.items[i].text);
        }
        free(n->list.items);
        break;
    case N_AND:
    case N_OR:
        ast_free(n->bin.left);
        ast_free(n->bin.right);
        break;
    case N_PIPELINE:
        for (size_t i = 0; i < n->pipe.n; i++)
            ast_free(n->pipe.cmds[i]);
        free(n->pipe.cmds);
        break;
    case N_SIMPLE:
        for (size_t i = 0; i < n->simple.nwords; i++)
            word_free(n->simple.words[i]);
        free(n->simple.words);
        free_redirs(n->simple.redirs, n->simple.nredirs);
        break;
    case N_SUBSHELL:
        ast_free(n->sub.body);
        free_redirs(n->sub.redirs, n->sub.nredirs);
        break;
    }
    free(n);
}
