#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "exec.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "shell.h"

struct shell shell;

void sh_error(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("omsh: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void sh_perror(const char *what)
{
    sh_error("%s: %s", what, strerror(errno));
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        sh_error("out of memory");
        abort();
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        sh_error("out of memory");
        abort();
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

void sh_child_exit(int status)
{
    fflush(stdout);
    fflush(stderr);
    _exit(status & 0xff);
}

/* A syntax error ends a non-interactive shell, like POSIX sh. */
static int syntax_error(void)
{
    shell.last_status = 2;
    if (!shell.interactive) {
        shell.exit_requested = true;
        shell.exit_status = 2;
    }
    return 2;
}

int sh_run_line(const char *line)
{
    struct tokens toks;
    const char *err;

    if (lex_line(line, &toks, &err) != 0) {
        sh_error("syntax error: %s", err);
        return syntax_error();
    }
    struct node *ast = parse(&toks, line, &err);
    tokens_free(&toks);
    if (!ast) {
        if (!err)
            return shell.last_status; /* blank line or comment */
        sh_error("syntax error near unexpected token `%s'", err);
        return syntax_error();
    }
    int status = exec_node(ast);
    ast_free(ast);
    return status;
}

int sh_run_stream(FILE *f)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while (!shell.exit_requested && (len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        jobs_reap(shell.interactive);
        sh_run_line(line);
    }
    free(line);
    return shell.last_status;
}

int sh_source(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        sh_perror(path);
        return 1;
    }
    /* children must not inherit the script's fd */
    fcntl(fileno(f), F_SETFD, FD_CLOEXEC);
    int status = sh_run_stream(f);
    fclose(f);
    return status;
}
