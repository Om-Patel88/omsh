#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "history.h"
#include "jobs.h"
#include "lineedit.h"
#include "shell.h"
#include "vars.h"

static void usage(void)
{
    fprintf(stderr, "usage: omsh [-c command [name [arg ...]]] [file [arg ...]]\n");
}

static void ignore_signal(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/* Make sure PWD names the real current directory. */
static void init_pwd(void)
{
    char buf[4096];
    if (getcwd(buf, sizeof buf))
        var_export("PWD", buf);
}

static void source_rc(void)
{
    const char *home = var_get("HOME");
    if (!home)
        return;
    size_t n = strlen(home) + sizeof "/.omshrc";
    char *path = xmalloc(n);
    snprintf(path, n, "%s/.omshrc", home);
    if (access(path, R_OK) == 0)
        sh_source(path);
    free(path);
}

/* Like bash, refuse the first attempt to leave while jobs are stopped. */
static bool may_exit(bool *warned)
{
    if (*warned || !jobs_any_stopped())
        return true;
    fprintf(stderr, "There are stopped jobs.\n");
    *warned = true;
    shell.exit_requested = false;
    return false;
}

static char *read_interactive(void *ctx, bool continuation)
{
    (void)ctx;
    const char *prompt = var_get(continuation ? "PS2" : "PS1");
    if (!prompt)
        prompt = continuation ? "> " : "omsh$ ";
    return lineedit_read(prompt);
}

static void interactive_loop(void)
{
    bool warned = false;

    if (!jobs_init_terminal()) {
        /* Ctrl-C and Ctrl-\ are for the running command, not the shell. */
        ignore_signal(SIGINT);
        ignore_signal(SIGQUIT);
    }
    source_rc();

    for (;;) {
        jobs_reap(true);
        char *line = sh_read_command(read_interactive, NULL);
        if (!line) {
            if (may_exit(&warned))
                break;
            continue;
        }
        history_add(line);
        bool was_warned = warned;
        sh_run_line(line);
        free(line);
        if (shell.exit_requested) {
            if (may_exit(&warned))
                break;
        } else if (was_warned) {
            warned = false; /* another command ran in between */
        }
    }
}

int main(int argc, char **argv)
{
    shell.pid = getpid();
    shell.args = argv;
    shell.nargs = 1;
    init_pwd();
    jobs_init_signals();

    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            sh_error("-c: option requires an argument");
            usage();
            return 2;
        }
        /* omsh -c cmd name a b: $0 is name, $1 is a, ... */
        if (argc > 3) {
            shell.args = argv + 3;
            shell.nargs = argc - 3;
        }
        sh_run_line(argv[2]);
    } else if (argc > 1) {
        shell.args = argv + 1;
        shell.nargs = argc - 1;
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            int err = errno;
            sh_perror(argv[1]);
            vars_free();
            return err == ENOENT ? 127 : 126;
        }
        fcntl(fileno(f), F_SETFD, FD_CLOEXEC);
        sh_run_stream(f);
        fclose(f);
    } else {
        shell.interactive = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
        if (shell.interactive)
            interactive_loop();
        else
            sh_run_stream(stdin);
    }

    int status = shell.exit_requested ? shell.exit_status : shell.last_status;
    fflush(stdout);
    jobs_shutdown();
    history_free();
    vars_free();
    return status;
}
