#include <ctype.h>
#include <glob.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
 * Expansion of one word builds two strings side by side: `plain` is the
 * final text, `pat` is the same text with quoted glob characters escaped,
 * ready for glob(3).
 */
struct xword {
    struct str plain, pat;
    bool has_glob; /* an unquoted *, ? or [ was seen */
    bool quoted;   /* some part was quoted, so keep the word even if empty */
};

static void add_char(struct xword *x, char c, bool globbable)
{
    str_push(&x->plain, c);
    if (!globbable && strchr("*?[]\\", c))
        str_push(&x->pat, '\\');
    str_push(&x->pat, c);
    if (globbable && strchr("*?[", c))
        x->has_glob = true;
}

static void add_str(struct xword *x, const char *s, bool globbable)
{
    while (*s)
        add_char(x, *s++, globbable);
}

/*
 * Expand the variable reference starting at s (which points just past '$').
 * Returns the number of bytes consumed, or 0 if this is a literal '$'.
 */
static size_t expand_var(struct xword *x, const char *s, bool globbable)
{
    char num[32];

    if (*s == '?') {
        snprintf(num, sizeof num, "%d", shell.last_status);
        add_str(x, num, false);
        return 1;
    }
    if (*s == '$') {
        snprintf(num, sizeof num, "%ld", (long)shell.pid);
        add_str(x, num, false);
        return 1;
    }
    if (*s == '#') {
        snprintf(num, sizeof num, "%d", shell.nargs > 0 ? shell.nargs - 1 : 0);
        add_str(x, num, false);
        return 1;
    }
    if (isdigit((unsigned char)*s)) {
        int i = *s - '0';
        if (i < shell.nargs)
            add_str(x, shell.args[i], globbable);
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
        if (len == 1 && (*name == '?' || *name == '$' || *name == '#' ||
                         isdigit((unsigned char)*name))) {
            expand_var(x, name, globbable);
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
        add_str(x, val, globbable);
    return used;
}

/* Expand a leading ~ or ~user in text. Returns bytes consumed or 0. */
static size_t expand_tilde(struct xword *x, const char *text)
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
    add_str(x, home, false);
    return len + 1;
}

static void expand_into(struct xword *x, const struct word *w, size_t skip)
{
    for (size_t i = 0; i < w->nsegs; i++) {
        const struct seg *sg = &w->segs[i];
        const char *p = sg->text + (i == 0 ? skip : 0);
        bool bare = sg->kind == SEG_BARE;

        if (!bare)
            x->quoted = true;
        if (sg->kind == SEG_LITERAL) {
            add_str(x, p, false);
            continue;
        }
        if (bare && i == 0 && skip == 0 && *p == '~')
            p += expand_tilde(x, p);
        while (*p) {
            if (*p == '$') {
                size_t used = expand_var(x, p + 1, bare);
                if (used) {
                    p += used + 1;
                    continue;
                }
            }
            add_char(x, *p++, bare);
        }
    }
}

static void xword_free(struct xword *x)
{
    free(x->plain.s);
    free(x->pat.s);
}

char *expand_word(const struct word *w, size_t skip)
{
    struct xword x = {0};
    expand_into(&x, w, skip);
    free(x.pat.s);
    return x.plain.s ? x.plain.s : xstrdup("");
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

char **expand_words(struct word *const *words, size_t n, size_t *argc)
{
    struct argv a = {0};

    a.cap = n + 1;
    a.v = xmalloc(a.cap * sizeof *a.v);
    a.v[0] = NULL;
    for (size_t i = 0; i < n; i++) {
        struct xword x = {0};
        expand_into(&x, words[i], 0);

        if (x.has_glob) {
            glob_t g;
            if (glob(x.pat.s, 0, NULL, &g) == 0) {
                for (size_t j = 0; j < g.gl_pathc; j++)
                    argv_push(&a, xstrdup(g.gl_pathv[j]));
                globfree(&g);
                xword_free(&x);
                continue;
            }
            globfree(&g);
            /* no match: the word stays as written, like POSIX sh */
        }
        if (x.plain.s || x.quoted)
            argv_push(&a, x.plain.s ? x.plain.s : xstrdup(""));
        free(x.pat.s);
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
