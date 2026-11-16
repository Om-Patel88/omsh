#ifndef OMSH_SHELL_H
#define OMSH_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* Global state of the running shell. */
struct shell {
    int last_status;     /* $? */
    bool interactive;
    bool exit_requested; /* set by `exit`; unwinds to the main loop */
    int exit_status;
    pid_t pid;           /* $$, stays the top-level shell's pid in children */
    bool in_child;       /* we are a forked copy of the shell */
    char **args;         /* $0 $1 ... */
    int nargs;
};

extern struct shell shell;

/* Print "omsh: <message>" to stderr. */
void sh_error(const char *fmt, ...);
/* Print "omsh: <what>: <strerror(errno)>" to stderr. */
void sh_perror(const char *what);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Lex, parse and run one line. Returns its exit status. */
int sh_run_line(const char *line);
/* Run every line of f until EOF or `exit`. Returns the last status. */
int sh_run_stream(FILE *f);
/* Run the file at path in the current shell (the `source` builtin). */
int sh_source(const char *path);

/* Leave a forked child: flush stdio and _exit without running atexit
 * handlers or freeing the parent's copies of shell state. */
void sh_child_exit(int status);

#endif
