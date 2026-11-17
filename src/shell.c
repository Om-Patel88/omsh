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

bool sh_incomplete(const char *line)
{
    struct tokens t;
    const char *err;

    if (lex_line(line, &t, &err) != 0)
        return true; /* still inside a quote */
    size_t i = t.n - 1; /* the TOK_END */
    while (i > 0 && t.v[i - 1].type == TOK_NEWLINE)
        i--;
    enum tok_type last = i > 0 ? t.v[i - 1].type : TOK_END;
    tokens_free(&t);
    return last == TOK_PIPE || last == TOK_AND || last == TOK_OR;
}

char *sh_read_command(line_reader read, void *ctx)
{
    char *cmd = read(ctx, false);

    while (cmd && sh_incomplete(cmd)) {
        char *more = read(ctx, true);
        if (!more)
            break; /* end of input: let the parser report the error */
        size_t a = strlen(cmd), b = strlen(more);
        cmd = xrealloc(cmd, a + b + 2);
        cmd[a] = '\n';
        memcpy(cmd + a + 1, more, b + 1);
        free(more);
    }
    return cmd;
}

static char *read_from_file(void *ctx, bool continuation)
{
    char *line = NULL;
    size_t cap = 0;
    (void)continuation;

    ssize_t len = getline(&line, &cap, ctx);
    if (len < 0) {
        free(line);
        return NULL;
    }
    if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = '\0';
    return line;
}

int sh_run_stream(FILE *f)
{
    char *cmd;

    while (!shell.exit_requested &&
           (cmd = sh_read_command(read_from_file, f)) != NULL) {
        jobs_reap(shell.interactive);
        sh_run_line(cmd);
        free(cmd);
    }
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
    if (fcntl(fileno(f), F_SETFD, FD_CLOEXEC) < 0)
        sh_perror(path);
    int status = sh_run_stream(f);
    fclose(f);
    return status;
}
