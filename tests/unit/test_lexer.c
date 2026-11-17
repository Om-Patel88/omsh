#include "check.h"
#include "lexer.h"

#include <stdlib.h>
#include <string.h>

/* Lex line and check the token types match the expected list. */
static int types_are(const char *line, const enum tok_type *want, size_t n)
{
    struct tokens t;
    const char *err;
    if (lex_line(line, &t, &err) != 0)
        return 0;
    int ok = t.n == n;
    for (size_t i = 0; ok && i < n; i++)
        ok = t.v[i].type == want[i];
    tokens_free(&t);
    return ok;
}

/* Text of word number idx (0-based among all tokens), or NULL. */
static char *text_of(const char *line, size_t idx)
{
    struct tokens t;
    const char *err;
    if (lex_line(line, &t, &err) != 0)
        return NULL;
    char *s = NULL;
    if (idx < t.n && t.v[idx].type == TOK_WORD)
        s = word_text(t.v[idx].word);
    tokens_free(&t);
    return s;
}

static int text_is(const char *line, size_t idx, const char *want)
{
    char *s = text_of(line, idx);
    int ok = s && strcmp(s, want) == 0;
    free(s);
    return ok;
}

static void test_operators(void)
{
    enum tok_type a[] = {TOK_WORD, TOK_PIPE, TOK_WORD, TOK_AND, TOK_WORD,
                         TOK_OR, TOK_WORD, TOK_SEMI, TOK_WORD, TOK_AMP,
                         TOK_END};
    CHECK(types_are("a | b && c || d ; e &", a, 11));
    CHECK(types_are("a|b&&c||d;e&", a, 11));

    enum tok_type r[] = {TOK_WORD, TOK_LT, TOK_WORD, TOK_GT, TOK_WORD,
                         TOK_DGT, TOK_WORD, TOK_ERR_GT, TOK_WORD,
                         TOK_ERR_DGT, TOK_WORD, TOK_ERR_TO_OUT, TOK_END};
    CHECK(types_are("c <i >o >>a 2>e 2>>f 2>&1", r, 13));

    enum tok_type p[] = {TOK_LPAREN, TOK_WORD, TOK_RPAREN, TOK_END};
    CHECK(types_are("(ls)", p, 4));

    /* "2>" in the middle of a word is not a redirect */
    enum tok_type m[] = {TOK_WORD, TOK_WORD, TOK_GT, TOK_WORD, TOK_END};
    CHECK(types_are("echo a2>f", m, 5));
    CHECK(text_is("echo a2>f", 1, "a2"));
}

static void test_quoting(void)
{
    CHECK(text_is("echo 'a b'", 1, "a b"));
    CHECK(text_is("echo \"a b\"", 1, "a b"));
    CHECK(text_is("echo a\"b\"'c'", 1, "abc"));
    CHECK(text_is("echo a\\ b", 1, "a b"));
    CHECK(text_is("echo \"x\\\"y\"", 1, "x\"y"));
    CHECK(text_is("echo \"\\\\\"", 1, "\\"));
    CHECK(text_is("echo '|;&'", 1, "|;&"));
    CHECK(text_is("echo ''", 1, ""));

    /* backslash before other chars in double quotes is kept */
    CHECK(text_is("echo \"a\\nb\"", 1, "a\\nb"));

    /* segment kinds */
    struct tokens t;
    const char *err;
    CHECK(lex_line("x'$a'\"$b\"\\$c", &t, &err) == 0);
    struct word *w = t.v[0].word;
    CHECK(w->nsegs == 5);
    CHECK(w->segs[0].kind == SEG_BARE);
    CHECK(w->segs[1].kind == SEG_LITERAL);
    CHECK(w->segs[2].kind == SEG_DQUOTE);
    CHECK(w->segs[3].kind == SEG_LITERAL);
    CHECK(strcmp(w->segs[3].text, "$") == 0);
    CHECK(w->segs[4].kind == SEG_BARE);
    tokens_free(&t);
}

static void test_errors_and_comments(void)
{
    struct tokens t;
    const char *err;
    CHECK(lex_line("echo 'oops", &t, &err) == -1 && err != NULL);
    CHECK(lex_line("echo \"oops", &t, &err) == -1 && err != NULL);

    enum tok_type c[] = {TOK_WORD, TOK_WORD, TOK_END};
    CHECK(types_are("echo hi # comment | x", c, 3));
    CHECK(types_are("echo a#b", c, 3));
    CHECK(text_is("echo a#b", 1, "a#b"));

    enum tok_type e[] = {TOK_END};
    CHECK(types_are("", e, 1));
    CHECK(types_are("   # only a comment", e, 1));

    /* a comment stops at the end of its line, not of the input */
    enum tok_type nl[] = {TOK_WORD, TOK_NEWLINE, TOK_WORD, TOK_END};
    CHECK(types_are("a # x | y\nb", nl, 4));
    CHECK(types_are("a\nb", nl, 4));
    CHECK(text_is("'a\nb'", 0, "a\nb"));
}

int main(void)
{
    test_operators();
    test_quoting();
    test_errors_and_comments();
    return CHECK_DONE("test_lexer");
}
