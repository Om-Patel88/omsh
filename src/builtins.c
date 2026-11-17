#include <errno.h>
#include <signal.h>
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

/* wait [%job | pid ...]: the status is that of the last one waited for. */
static int bi_wait(int argc, char **argv)
{
    int status = 0;

    if (argc == 1)
        return jobs_wait(NULL);
    for (int i = 1; i < argc; i++) {
        struct job *j;
        if (argv[i][0] == '%') {
            j = jobs_find(argv[i], "wait", false);
            if (!j) {
                status = 127;
                continue;
            }
        } else {
            char *end;
            long pid = strtol(argv[i], &end, 10);
            j = *argv[i] && *end == '\0' && pid > 0 ? jobs_find_pid((pid_t)pid)
                                                    : NULL;
            if (!j) {
                sh_error("wait: pid %s is not a child of this shell",
                         argv[i]);
                status = 127;
                continue;
            }
        }
        status = jobs_wait(j);
    }
    return status;
}

static int bi_fg(int argc, char **argv)
{
    if (!shell.job_control) {
        sh_error("fg: no job control");
        return 1;
    }
    struct job *j = jobs_find(argc > 1 ? argv[1] : NULL, "fg", true);
    if (!j)
        return 1;
    printf("%s\n", job_cmd(j));
    fflush(stdout);
    return job_foreground(j, true);
}

static int bi_bg(int argc, char **argv)
{
    int status = 0;

    if (!shell.job_control) {
        sh_error("bg: no job control");
        return 1;
    }
    for (int i = 1; i < argc || i == 1; i++) {
        struct job *j = jobs_find(i < argc ? argv[i] : NULL, "bg", true);
        if (!j) {
            status = 1;
            continue;
        }
        job_background(j, true);
        printf("[%d]+ %s &\n", job_id(j), job_cmd(j));
    }
    return status;
}

static const struct {
    const char *name;
    int sig;
} signals[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
    {"ABRT", SIGABRT}, {"KILL", SIGKILL}, {"ALRM", SIGALRM},
    {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
    {"PIPE", SIGPIPE}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT},
    {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
    {"TTOU", SIGTTOU},
};
#define NSIGNALS (sizeof signals / sizeof signals[0])

/* Signal number for "TERM", "SIGTERM" or "15"; -1 if unknown. */
static int parse_signal(const char *s)
{
    if (*s >= '0' && *s <= '9') {
        char *end;
        long n = strtol(s, &end, 10);
        return *end == '\0' && n >= 0 && n < 128 ? (int)n : -1;
    }
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (size_t i = 0; i < NSIGNALS; i++)
        if (strcmp(s, signals[i].name) == 0)
            return signals[i].sig;
    return -1;
}

static int bi_kill(int argc, char **argv)
{
    int sig = SIGTERM;
    int i = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        for (size_t k = 0; k < NSIGNALS; k++)
            printf("%d) SIG%s\n", signals[k].sig, signals[k].name);
        return 0;
    }
    if (i < argc && strcmp(argv[i], "-s") == 0) {
        if (i + 1 >= argc) {
            sh_error("kill: -s: option requires an argument");
            return 2;
        }
        sig = parse_signal(argv[i + 1]);
        if (sig < 0) {
            sh_error("kill: %s: invalid signal specification", argv[i + 1]);
            return 1;
        }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0' &&
               strcmp(argv[i], "--") != 0) {
        sig = parse_signal(argv[i] + 1);
        if (sig < 0) {
            sh_error("kill: %s: invalid signal specification", argv[i] + 1);
            return 1;
        }
        i++;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        sh_error("usage: kill [-s sig | -sig] %%job | pid ...");
        return 2;
    }

    int status = 0;
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            struct job *j = jobs_find(argv[i], "kill", false);
            if (!j) {
                status = 1;
                continue;
            }
            if (job_kill(j, sig) < 0) {
                sh_error("kill: %s: %s", argv[i], strerror(errno));
                status = 1;
            } else if (sig == SIGTERM || sig == SIGHUP) {
                /* a stopped job only sees the signal once it runs */
                if (job_kill(j, SIGCONT) < 0)
                    sh_error("kill: %s: %s", argv[i], strerror(errno));
            }
            continue;
        }
        char *end;
        errno = 0;
        long pid = strtol(argv[i], &end, 10);
        if (*argv[i] == '\0' || *end != '\0' || errno || pid != (pid_t)pid) {
            sh_error("kill: %s: arguments must be process or job IDs",
                     argv[i]);
            status = 1;
        } else if (kill((pid_t)pid, sig) < 0) {
            sh_error("kill: (%ld) - %s", pid, strerror(errno));
            status = 1;
        }
    }
    return status;
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
    {"fg", bi_fg},         {"bg", bi_bg},
    {"kill", bi_kill},
};

builtin_fn builtin_find(const char *name)
{
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(builtins[i].name, name) == 0)
            return builtins[i].fn;
    return NULL;
}
