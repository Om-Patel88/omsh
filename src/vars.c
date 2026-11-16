#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "vars.h"

struct var {
    char *name;
    char *value;
};

static struct var *locals;
static size_t nlocals;

static struct var *find_local(const char *name)
{
    for (size_t i = 0; i < nlocals; i++)
        if (strcmp(locals[i].name, name) == 0)
            return &locals[i];
    return NULL;
}

static void remove_local(const char *name)
{
    struct var *v = find_local(name);
    if (!v)
        return;
    free(v->name);
    free(v->value);
    *v = locals[--nlocals];
}

const char *var_get(const char *name)
{
    struct var *v = find_local(name);
    return v ? v->value : getenv(name);
}

void var_set(const char *name, const char *value)
{
    if (getenv(name)) {
        if (setenv(name, value, 1) != 0)
            sh_perror(name);
        return;
    }
    struct var *v = find_local(name);
    if (v) {
        char *copy = xstrdup(value);
        free(v->value);
        v->value = copy;
        return;
    }
    locals = xrealloc(locals, (nlocals + 1) * sizeof *locals);
    locals[nlocals].name = xstrdup(name);
    locals[nlocals].value = xstrdup(value);
    nlocals++;
}

void var_export(const char *name, const char *value)
{
    if (!value) {
        struct var *v = find_local(name);
        value = v ? v->value : getenv(name);
        if (!value)
            value = "";
    }
    if (setenv(name, value, 1) != 0)
        sh_perror(name);
    remove_local(name); /* after setenv: value may point into the table */
}

void var_unset(const char *name)
{
    remove_local(name);
    if (unsetenv(name) != 0)
        sh_perror(name);
}

int var_valid_name(const char *s, size_t len)
{
    if (len == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (size_t i = 1; i < len; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    return 1;
}

void vars_free(void)
{
    for (size_t i = 0; i < nlocals; i++) {
        free(locals[i].name);
        free(locals[i].value);
    }
    free(locals);
    locals = NULL;
    nlocals = 0;
}
