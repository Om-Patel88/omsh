#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "builtins.h"
#include "history.h"
#include "jobs.h"
#include "shell.h"
#include "vars.h"

extern char **environ;

/* getcwd into a fresh buffer of whatever size it needs. */
static char *current_dir(void)
{
    size_t size = 256;
    for (;;) {
        char *buf = xmalloc(size);
        if (getcwd(buf, size))
            return buf;
        free(buf);
        if (errno != ERANGE)
            return NULL;
        size *= 2;
    }
}

static int bi_cd(int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : var_get("HOME");
    bool print = false;

    if (argc > 2) {
        sh_error("cd: too many arguments");
        return 1;
    }
    if (!arg) {
        sh_error("cd: HOME not set");
        return 1;
    }
    if (strcmp(arg, "-") == 0) {
        arg = var_get("OLDPWD");
        if (!arg) {
            sh_error("cd: OLDPWD not set");
            return 1;
        }
        print = true;
    }

    /* arg may point into the environment, which we are about to change */
    char *dir = xstrdup(arg);
    char *old = current_dir();
    if (chdir(dir) != 0) {
        sh_error("cd: %s: %s", dir, strerror(errno));
        free(dir);
        free(old);
        return 1;
    }
    if (old)
        var_export("OLDPWD", old);
    char *now = current_dir();
    if (now) {
        var_export("PWD", now);
        if (print)
            printf("%s\n", now);
    }
    free(now);
    free(old);
    free(dir);
    return 0;
}

static int bi_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char *dir = current_dir();
    if (!dir) {
        sh_perror("pwd");
        return 1;
    }
    printf("%s\n", dir);
    free(dir);
    return 0;
}

static int bi_exit(int argc, char **argv)
{
    int status = shell.last_status;

    if (argc > 1) {
        char *end;
        errno = 0;
        long n = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || errno) {
            sh_error("exit: %s: numeric argument required", argv[1]);
            status = 2;
        } else {
            status = (int)(n & 0xff);
        }
    }
    shell.exit_requested = true;
    shell.exit_status = status;
    return status;
}

static int bi_export(int argc, char **argv)
{
    int status = 0;

    if (argc == 1) {
        for (char **e = environ; *e; e++)
            printf("export %s\n", *e);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!var_valid_name(argv[i], len)) {
            sh_error("export: `%s': not a valid identifier", argv[i]);
            status = 1;
            continue;
        }
        if (eq) {
            *eq = '\0';
            var_export(argv[i], eq + 1);
            *eq = '=';
        } else {
            var_export(argv[i], NULL);
        }
    }
    return status;
}

static int bi_unset(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!var_valid_name(argv[i], strlen(argv[i]))) {
            sh_error("unset: `%s': not a valid identifier", argv[i]);
            status = 1;
            continue;
        }
        var_unset(argv[i]);
    }
    return status;
}

static int bi_echo(int argc, char **argv)
{
    int i = 1;
    bool newline = true;

    while (i < argc && strcmp(argv[i], "-n") == 0) {
        newline = false;
        i++;
    }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc)
            putchar(' ');
    }
    if (newline)
        putchar('\n');
    return 0;
}

/* Search PATH for an executable called name. Caller frees. */
static char *find_in_path(const char *name)
{
    struct stat st;

    if (strchr(name, '/')) {
        if (stat(name, &st) == 0 && S_ISREG(st.st_mode) &&
            access(name, X_OK) == 0)
            return xstrdup(name);
        return NULL;
    }
    const char *path = var_get("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    while (*path) {
        size_t len = strcspn(path, ":");
        /* an empty PATH entry means the current directory */
        const char *dir = len ? path : ".";
        size_t dlen = len ? len : 1;
        char *full = xmalloc(dlen + strlen(name) + 2);
        memcpy(full, dir, dlen);
        full[dlen] = '/';
        strcpy(full + dlen + 1, name);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
            access(full, X_OK) == 0)
            return full;
        free(full);
        path += len;
        if (*path == ':')
            path++;
    }
    return NULL;
}

static int bi_type(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (builtin_find(argv[i])) {
            printf("%s is a shell builtin\n", argv[i]);
            continue;
        }
        char *path = find_in_path(argv[i]);
        if (path) {
            printf("%s is %s\n", argv[i], path);
            free(path);
        } else {
            sh_error("type: %s: not found", argv[i]);
            status = 1;
        }
    }
    return status;
}

static int bi_history(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < history_len(); i++)
        printf("%5zu  %s\n", history_number(i), history_get(i));
    return 0;
}

static int bi_source(int argc, char **argv)
{
    if (argc < 2) {
        sh_error("%s: filename argument required", argv[0]);
        return 2;
    }
    return sh_source(argv[1]);
}

static int bi_true(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int bi_false(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

static int bi_jobs(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    jobs_print();
    return 0;
}

static int bi_wait(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    jobs_wait_all();
    return 0;
}

static int bi_no_job_control(int argc, char **argv)
{
    (void)argc;
    sh_error("%s: no job control", argv[0]);
    return 1;
}

static const struct {
    const char *name;
    builtin_fn fn;
} builtins[] = {
    {"cd", bi_cd},         {"pwd", bi_pwd},
    {"exit", bi_exit},     {"export", bi_export},
    {"unset", bi_unset},   {"echo", bi_echo},
    {"type", bi_type},     {"history", bi_history},
    {"source", bi_source}, {".", bi_source},
    {"true", bi_true},     {"false", bi_false},
    {"jobs", bi_jobs},     {"wait", bi_wait},
    {"fg", bi_no_job_control},
    {"bg", bi_no_job_control},
};

builtin_fn builtin_find(const char *name)
{
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(builtins[i].name, name) == 0)
            return builtins[i].fn;
    return NULL;
}
