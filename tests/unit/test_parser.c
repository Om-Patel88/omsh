#include "check.h"
#include "lexer.h"
#include "parser.h"

#include <stdlib.h>
#include <string.h>

/* Lex and parse line. Returns the AST; *err gets the parse error, if any. */
static struct node *parse_str(const char *line, const char **err)
{
    struct tokens t;
    if (lex_line(line, &t, err) != 0)
        return NULL;
    struct node *n = parse(&t, line, err);
    tokens_free(&t);
    return n;
}

static int syntax_error(const char *line)
{
    const char *err;
    struct node *n = parse_str(line, &err);
    ast_free(n);
    return n == NULL && err != NULL;
}

static int word_eq(const struct word *w, const char *s)
{
    char *t = word_text(w);
    int ok = strcmp(t, s) == 0;
    free(t);
    return ok;
}

static void test_simple(void)
{
    const char *err;
    struct node *n = parse_str("ls -l /tmp > out 2>&1", &err);
    CHECK(n && n->kind == N_LIST && n->list.n == 1);
    struct node *pl = n->list.items[0].node;
    CHECK(pl->kind == N_PIPELINE && pl->pipe.n == 1 && !pl->pipe.negate);
    struct node *c = pl->pipe.cmds[0];
    CHECK(c->kind == N_SIMPLE);
    CHECK(c->simple.nwords == 3);
    CHECK(word_eq(c->simple.words[2], "/tmp"));
    CHECK(c->simple.nredirs == 2);
    CHECK(c->simple.redirs[0].type == R_OUT);
    CHECK(word_eq(c->simple.redirs[0].target, "out"));
    CHECK(c->simple.redirs[1].type == R_ERR_TO_OUT);
    CHECK(strcmp(n->list.items[0].text, "ls -l /tmp > out 2>&1") == 0);
    ast_free(n);

    /* a redirect alone is a valid command */
    n = parse_str("> empty", &err);
    CHECK(n != NULL);
    ast_free(n);

    CHECK(parse_str("", &err) == NULL && err == NULL);
    CHECK(parse_str("  # nothing", &err) == NULL && err == NULL);
}

static void test_structure(void)
{
    const char *err;
    struct node *n = parse_str("a | b | c && ! d || e; f &", &err);
    CHECK(n && n->list.n == 2);
    CHECK(!n->list.items[0].background);
    CHECK(n->list.items[1].background);
    CHECK(strcmp(n->list.items[1].text, "f") == 0);

    /* && and || are left associative: ((a|b|c && !d) || e) */
    struct node *top = n->list.items[0].node;
    CHECK(top->kind == N_OR);
    CHECK(top->bin.left->kind == N_AND);
    struct node *pipe = top->bin.left->bin.left;
    CHECK(pipe->kind == N_PIPELINE && pipe->pipe.n == 3);
    struct node *neg = top->bin.left->bin.right;
    CHECK(neg->kind == N_PIPELINE && neg->pipe.negate);
    ast_free(n);

    n = parse_str("(cd /; pwd) > out", &err);
    CHECK(n != NULL);
    struct node *sub = n->list.items[0].node->pipe.cmds[0];
    CHECK(sub->kind == N_SUBSHELL);
    CHECK(sub->sub.body->kind == N_LIST && sub->sub.body->list.n == 2);
    CHECK(sub->sub.nredirs == 1);
    ast_free(n);

    n = parse_str("(a; (b | c)) | d", &err);
    CHECK(n != NULL);
    CHECK(n->list.items[0].node->pipe.n == 2);
    ast_free(n);

    /* a quoted ! is just a word */
    n = parse_str("'!' x", &err);
    CHECK(n && !n->list.items[0].node->pipe.negate);
    ast_free(n);
}

static void test_errors(void)
{
    CHECK(syntax_error("|"));
    CHECK(syntax_error("a |"));
    CHECK(syntax_error("a && "));
    CHECK(syntax_error("; a"));
    CHECK(syntax_error("a ;; b"));
    CHECK(syntax_error("a >"));
    CHECK(syntax_error("a > | b"));
    CHECK(syntax_error("()"));
    CHECK(syntax_error("(a"));
    CHECK(syntax_error("a )"));
    CHECK(syntax_error("!"));
    CHECK(syntax_error("echo 'x"));

    const char *err;
    struct node *n = parse_str("a | | b", &err);
    CHECK(n == NULL && err && strcmp(err, "|") == 0);
}

int main(void)
{
    test_simple();
    test_structure();
    test_errors();
    return CHECK_DONE("test_parser");
}
