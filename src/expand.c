#include <ctype.h>
#include <glob.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "exec.h"
#include "expand.h"
#include "shell.h"
#include "vars.h"

struct str {
    char *s;
    size_t len, cap;
};

static void str_push(struct str *b, char c)
{
    if (b->len + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        b->s = xrealloc(b->s, b->cap);
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

/*
 * Expansion of one field builds two strings side by side: `plain` is the
 * final text, `pat` is the same text with quoted glob characters escaped,
 * ready for glob(3).
 */
struct xword {
    struct str plain, pat;
    bool has_glob; /* an unquoted *, ? or [ was seen */
    bool keep;     /* quoted or ended by a delimiter: keep even if empty */
};

/*
 * One word can expand to several fields when an unquoted $VAR or $(...)
 * contains IFS characters. The last field is the one being built.
 */
struct fields {
    struct xword *v;
    size_t n;
    bool split;      /* field splitting applies (not for NAME=value) */
    bool pending;    /* IFS white space seen: next text starts a field */
    const char *ifs;
};

static struct xword *cur(struct fields *f)
{
    return &f->v[f->n - 1];
}

static void new_field(struct fields *f)
{
    f->v = xrealloc(f->v, (f->n + 1) * sizeof *f->v);
    memset(&f->v[f->n++], 0, sizeof *f->v);
    f->pending = false;
}

static bool started(const struct xword *x)
{
    return x->plain.len > 0 || x->keep;
}

static void add_char(struct fields *f, char c, bool globbable)
{
    if (f->pending)
        new_field(f);
    struct xword *x = cur(f);
    str_push(&x->plain, c);
    if (!globbable && strchr("*?[]\\", c))
        str_push(&x->pat, '\\');
    str_push(&x->pat, c);
    if (globbable && strchr("*?[", c))
        x->has_glob = true;
}

static void add_str(struct fields *f, const char *s, bool globbable)
{
    while (*s)
        add_char(f, *s++, globbable);
}

/* A quoted part, even an empty one, makes sure its field exists. */
static void add_quoted(struct fields *f)
{
    if (f->pending)
        new_field(f);
    cur(f)->keep = true;
}

/*
 * Add the result of an unquoted expansion, splitting it on IFS: runs of
 * IFS white space separate fields and are dropped at the edges, while
 * each other IFS character ends a field, even an empty one.
 */
static void add_split(struct fields *f, const char *s)
{
    if (!f->split) {
        add_str(f, s, true);
        return;
    }
    for (; *s; s++) {
        if (!strchr(f->ifs, *s)) {
            add_char(f, *s, true);
        } else if (*s == ' ' || *s == '\t' || *s == '\n') {
            if (started(cur(f)))
                f->pending = true;
        } else {
            cur(f)->keep = true;
            new_field(f);
        }
    }
}

static void add_var(struct fields *f, const char *val, bool bare)
{
    if (bare)
        add_split(f, val);
    else
        add_str(f, val, false);
}

/*
 * Expand the variable reference starting at s (which points just past '$').
 * Returns the number of bytes consumed, or 0 if this is a literal '$'.
 */
static size_t expand_var(struct fields *f, const char *s, bool bare)
{
    char num[32];

    if (*s == '?') {
        snprintf(num, sizeof num, "%d", shell.last_status);
        add_str(f, num, false);
        return 1;
    }
    if (*s == '$') {
        snprintf(num, sizeof num, "%ld", (long)shell.pid);
        add_str(f, num, false);
        return 1;
    }
    if (*s == '!') {
        if (shell.last_bg_pid > 0) {
            snprintf(num, sizeof num, "%ld", (long)shell.last_bg_pid);
            add_str(f, num, false);
        }
        return 1;
    }
    if (*s == '#') {
        snprintf(num, sizeof num, "%d", shell.nargs > 0 ? shell.nargs - 1 : 0);
        add_str(f, num, false);
        return 1;
    }
    if (isdigit((unsigned char)*s)) {
        int i = *s - '0';
        if (i < shell.nargs)
            add_var(f, shell.args[i], bare);
        return 1;
    }

    const char *name = s;
    size_t len = 0, used;
    if (*s == '{') {
        const char *close = strchr(s, '}');
        if (!close)
            return 0;
        name = s + 1;
        len = (size_t)(close - name);
        used = len + 2;
        if (len == 1 && (*name == '?' || *name == '$' || *name == '#' || *name == '!' ||
                         isdigit((unsigned char)*name))) {
            expand_var(f, name, bare);
            return used;
        }
        if (!var_valid_name(name, len))
            return 0;
    } else {
        while (isalnum((unsigned char)name[len]) || name[len] == '_')
            len++;
        if (len == 0 || isdigit((unsigned char)name[0]))
            return 0;
        used = len;
    }

    char *key = xmalloc(len + 1);
    memcpy(key, name, len);
    key[len] = '\0';
    const char *val = var_get(key);
    free(key);
    if (val)
        add_var(f, val, bare);
    return used;
}

/* Expand a leading ~ or ~user in text. Returns bytes consumed or 0. */
static size_t expand_tilde(struct fields *f, const char *text)
{
    size_t len = strcspn(text + 1, "/");
    const char *home = NULL;

    if (len == 0) {
        home = var_get("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : NULL;
        }
    } else {
        char *user = xmalloc(len + 1);
        memcpy(user, text + 1, len);
        user[len] = '\0';
        struct passwd *pw = getpwnam(user);
        free(user);
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home)
        return 0;
    add_str(f, home, false);
    return len + 1;
}

static void expand_into(struct fields *f, const struct word *w, size_t skip)
{
    for (size_t i = 0; i < w->nsegs; i++) {
        const struct seg *sg = &w->segs[i];
        const char *p = sg->text + (i == 0 ? skip : 0);
        bool bare = sg->kind == SEG_BARE;

        if (!bare && sg->kind != SEG_SUBST)
            add_quoted(f);
        if (sg->kind == SEG_LITERAL) {
            add_str(f, p, false);
            continue;
        }
        if (sg->kind == SEG_SUBST || sg->kind == SEG_DQSUBST) {
            char *out = exec_capture(sg->text);
            add_var(f, out, sg->kind == SEG_SUBST);
            free(out);
            continue;
        }
        if (bare && i == 0 && skip == 0 && *p == '~')
            p += expand_tilde(f, p);
        while (*p) {
            if (*p == '$') {
                size_t used = expand_var(f, p + 1, bare);
                if (used) {
                    p += used + 1;
                    continue;
                }
            }
            add_char(f, *p++, bare);
        }
    }
}

static void fields_init(struct fields *f, bool split)
{
    memset(f, 0, sizeof *f);
    f->split = split;
    f->ifs = var_get("IFS");
    if (!f->ifs)
        f->ifs = " \t\n";
    new_field(f);
}

static void xword_free(struct xword *x)
{
    free(x->plain.s);
    free(x->pat.s);
}

char *expand_word(const struct word *w, size_t skip)
{
    struct fields f;
    fields_init(&f, false);
    expand_into(&f, w, skip);
    char *s = f.v[0].plain.s ? f.v[0].plain.s : xstrdup("");
    free(f.v[0].pat.s);
    free(f.v);
    return s;
}

struct argv {
    char **v;
    size_t n, cap;
};

static void argv_push(struct argv *a, char *s)
{
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = xrealloc(a->v, a->cap * sizeof *a->v);
    }
    a->v[a->n++] = s;
    a->v[a->n] = NULL;
}

/* Glob one field into a, or add it as is. Takes over x's strings. */
static void push_field(struct argv *a, struct xword *x)
{
    if (x->has_glob) {
        glob_t g;
        if (glob(x->pat.s, 0, NULL, &g) == 0) {
            for (size_t j = 0; j < g.gl_pathc; j++)
                argv_push(a, xstrdup(g.gl_pathv[j]));
            globfree(&g);
            xword_free(x);
            return;
        }
        globfree(&g);
        /* no match: the word stays as written, like POSIX sh */
    }
    if (started(x))
        argv_push(a, x->plain.s ? x->plain.s : xstrdup(""));
    else
        free(x->plain.s);
    free(x->pat.s);
}

char **expand_words(struct word *const *words, size_t n, size_t *argc)
{
    struct argv a = {0};

    a.cap = n + 1;
    a.v = xmalloc(a.cap * sizeof *a.v);
    a.v[0] = NULL;
    for (size_t i = 0; i < n; i++) {
        struct fields f;
        fields_init(&f, true);
        expand_into(&f, words[i], 0);
        for (size_t k = 0; k < f.n; k++)
            push_field(&a, &f.v[k]);
        free(f.v);
    }
    *argc = a.n;
    return a.v;
}

void argv_free(char **argv)
{
    if (!argv)
        return;
    for (char **p = argv; *p; p++)
        free(*p);
    free(argv);
}
