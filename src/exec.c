#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "shell.h"
#include "vars.h"

/* True in a background child: keep ignoring keyboard signals there. */
static bool in_background;

int exec_status(int wstatus)
{
    if (WIFEXITED(wstatus))
        return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus))
        return 128 + WTERMSIG(wstatus);
    return 1;
}

static int wait_for(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            sh_perror("waitpid");
            return 1;
        }
    }
    return exec_status(st);
}

static void set_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/* Called first thing in every forked child. */
static void child_setup(void)
{
    shell.in_child = true;
    if (shell.interactive && !in_background) {
        set_signal(SIGINT, SIG_DFL);
        set_signal(SIGQUIT, SIG_DFL);
    }
}

static pid_t do_fork(void)
{
    /* Anything left in our stdio buffers would be written twice. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        sh_perror("fork");
    else if (pid == 0)
        child_setup();
    return pid;
}

/* ---- redirections ---- */

/* Copies of fds 0-2 taken before redirecting a builtin in the shell.
 * -1 means untouched, -2 means the fd was closed before. */
struct saved_fds {
    int fd[3];
};

static void save_fd(struct saved_fds *save, int fd)
{
    if (!save || save->fd[fd] != -1)
        return;
    save->fd[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if (save->fd[fd] < 0)
        save->fd[fd] = -2;
}

static void restore_fds(struct saved_fds *save)
{
    for (int i = 0; i < 3; i++) {
        if (save->fd[i] >= 0) {
            if (dup2(save->fd[i], i) < 0)
                sh_perror("dup2");
            close(save->fd[i]);
        } else if (save->fd[i] == -2) {
            close(i);
        }
        save->fd[i] = -1;
    }
}

/* Apply redirections in order. With save, the old fds are kept so they
 * can be restored. Returns -1 after printing an error. */
static int apply_redirs(const struct redir *r, size_t n,
                        struct saved_fds *save)
{
    for (size_t i = 0; i < n; i++) {
        if (r[i].type == R_ERR_TO_OUT) {
            save_fd(save, 2);
            if (dup2(1, 2) < 0) {
                sh_perror("dup2");
                return -1;
            }
            continue;
        }

        int target, flags;
        switch (r[i].type) {
        case R_IN: target = 0; flags = O_RDONLY; break;
        case R_OUT: target = 1; flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case R_APPEND: target = 1; flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case R_ERR: target = 2; flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        default: target = 2; flags = O_WRONLY | O_CREAT | O_APPEND; break;
        }

        char *path = expand_word(r[i].target, 0);
        int fd = open(path, flags | O_CLOEXEC, 0666);
        if (fd < 0) {
            sh_perror(path);
            free(path);
            return -1;
        }
        free(path);
        save_fd(save, target);
        int rc = dup2(fd, target);
        close(fd);
        if (rc < 0) {
            sh_perror("dup2");
            return -1;
        }
    }
    return 0;
}

/* ---- simple commands ---- */

/* If w looks like NAME=value, return the length of "NAME=", else 0. */
static size_t assignment_len(const struct word *w)
{
    if (w->nsegs == 0 || w->segs[0].kind != SEG_BARE)
        return 0;
    const char *t = w->segs[0].text;
    const char *eq = strchr(t, '=');
    if (!eq || !var_valid_name(t, (size_t)(eq - t)))
        return 0;
    return (size_t)(eq - t) + 1;
}

/* Perform the NAME=value words at the front of a command. With export_only
 * (in a child about to exec) they go into the environment. */
static void do_assignments(struct word *const *words, size_t n,
                           bool export_only)
{
    for (size_t i = 0; i < n; i++) {
        size_t len = assignment_len(words[i]);
        char *name = xmalloc(len);
        memcpy(name, words[i]->segs[0].text, len - 1);
        name[len - 1] = '\0';
        char *value = expand_word(words[i], len);
        if (export_only)
            var_export(name, value);
        else
            var_set(name, value);
        free(name);
        free(value);
    }
}

static size_t count_assignments(const struct node *cmd)
{
    size_t n = 0;
    while (n < cmd->simple.nwords && assignment_len(cmd->simple.words[n]))
        n++;
    return n;
}

/* exec argv in this (child) process; never returns. */
static void exec_external(char **argv)
{
    execvp(argv[0], argv);
    int err = errno;
    if (err == ENOENT && !strchr(argv[0], '/')) {
        sh_error("%s: command not found", argv[0]);
        sh_child_exit(127);
    }
    sh_error("%s: %s", argv[0], strerror(err));
    sh_child_exit(err == ENOENT ? 127 : 126);
}

static int status_after_builtin(int status)
{
    return shell.exit_requested ? shell.exit_status : status;
}

/* Run a simple command in the shell process. */
static int exec_simple(struct node *cmd)
{
    size_t nassign = count_assignments(cmd);
    size_t argc;
    char **argv = expand_words(cmd->simple.words + nassign,
                               cmd->simple.nwords - nassign, &argc);
    struct saved_fds save = {{-1, -1, -1}};
    int status;

    if (argc == 0) {
        /* only assignments and/or redirections */
        do_assignments(cmd->simple.words, nassign, false);
        status = apply_redirs(cmd->simple.redirs, cmd->simple.nredirs,
                              &save) < 0 ? 1 : 0;
        restore_fds(&save);
        argv_free(argv);
        return status;
    }

    builtin_fn fn = builtin_find(argv[0]);
    if (fn) {
        do_assignments(cmd->simple.words, nassign, false);
        if (apply_redirs(cmd->simple.redirs, cmd->simple.nredirs, &save) < 0) {
            status = 1;
        } else {
            status = fn((int)argc, argv);
            fflush(stdout);
            fflush(stderr);
        }
        restore_fds(&save);
        argv_free(argv);
        return status;
    }

    pid_t pid = do_fork();
    if (pid == 0) {
        do_assignments(cmd->simple.words, nassign, true);
        if (apply_redirs(cmd->simple.redirs, cmd->simple.nredirs, NULL) < 0)
            sh_child_exit(1);
        exec_external(argv);
    }
    argv_free(argv);
    return pid < 0 ? 1 : wait_for(pid);
}

/* Run one command of a pipeline (or a subshell) in a forked child. */
static void child_command(struct node *cmd)
{
    if (cmd->kind == N_SUBSHELL) {
        if (apply_redirs(cmd->sub.redirs, cmd->sub.nredirs, NULL) < 0)
            sh_child_exit(1);
        int status = exec_node(cmd->sub.body);
        sh_child_exit(status_after_builtin(status));
    }

    size_t nassign = count_assignments(cmd);
    size_t argc;
    char **argv = expand_words(cmd->simple.words + nassign,
                               cmd->simple.nwords - nassign, &argc);
    do_assignments(cmd->simple.words, nassign, true);
    if (apply_redirs(cmd->simple.redirs, cmd->simple.nredirs, NULL) < 0)
        sh_child_exit(1);
    if (argc == 0)
        sh_child_exit(0);

    builtin_fn fn = builtin_find(argv[0]);
    if (fn) {
        int status = fn((int)argc, argv);
        argv_free(argv);
        sh_child_exit(status_after_builtin(status));
    }
    exec_external(argv);
}

/* ---- pipelines and lists ---- */

static int exec_pipe(struct node *pl)
{
    size_t n = pl->pipe.n;
    pid_t *pids = xmalloc(n * sizeof *pids);
    int in = -1; /* read end of the previous pipe */
    size_t started = 0;

    for (size_t i = 0; i < n; i++) {
        int fds[2] = {-1, -1};
        if (i + 1 < n && pipe(fds) < 0) {
            sh_perror("pipe");
            break;
        }
        pid_t pid = do_fork();
        if (pid == 0) {
            if (in >= 0) {
                if (dup2(in, 0) < 0)
                    sh_child_exit(1);
                close(in);
            }
            if (fds[1] >= 0) {
                close(fds[0]);
                if (dup2(fds[1], 1) < 0)
                    sh_child_exit(1);
                close(fds[1]);
            }
            child_command(pl->pipe.cmds[i]);
        }
        if (in >= 0)
            close(in);
        if (fds[1] >= 0)
            close(fds[1]);
        in = fds[0];
        if (pid < 0)
            break;
        pids[started++] = pid;
    }
    if (in >= 0) /* only left open if we stopped early */
        close(in);

    int status = 1;
    for (size_t i = 0; i < started; i++) {
        int s = wait_for(pids[i]);
        if (i == n - 1)
            status = s;
    }
    free(pids);
    return status;
}

static int exec_pipeline(struct node *pl)
{
    int status;
    struct node *cmd = pl->pipe.cmds[0];

    if (pl->pipe.n > 1) {
        status = exec_pipe(pl);
    } else if (cmd->kind == N_SIMPLE) {
        status = exec_simple(cmd);
    } else {
        pid_t pid = do_fork();
        if (pid == 0)
            child_command(cmd);
        status = pid < 0 ? 1 : wait_for(pid);
    }
    if (pl->pipe.negate)
        status = !status;
    shell.last_status = status;
    return status;
}

static void run_background(struct list_item *item)
{
    pid_t pid = do_fork();
    if (pid < 0) {
        shell.last_status = 1;
        return;
    }
    if (pid == 0) {
        /* Without job control, background jobs must not read the
         * terminal or be killed by Ctrl-C meant for the foreground. */
        in_background = true;
        if (shell.interactive) {
            set_signal(SIGINT, SIG_IGN);
            set_signal(SIGQUIT, SIG_IGN);
        }
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0 && fd != 0) {
            if (dup2(fd, 0) < 0)
                sh_child_exit(1);
            close(fd);
        }
        int status = exec_node(item->node);
        sh_child_exit(status_after_builtin(status));
    }
    int id = jobs_add(pid, item->text);
    if (shell.interactive)
        fprintf(stderr, "[%d] %ld\n", id, (long)pid);
    shell.last_status = 0;
}

int exec_node(struct node *n)
{
    int status = shell.last_status;

    switch (n->kind) {
    case N_LIST:
        for (size_t i = 0; i < n->list.n && !shell.exit_requested; i++) {
            if (n->list.items[i].background)
                run_background(&n->list.items[i]);
            else
                exec_node(n->list.items[i].node);
            status = shell.last_status;
        }
        break;
    case N_AND:
    case N_OR:
        status = exec_node(n->bin.left);
        if (!shell.exit_requested && (n->kind == N_AND) == (status == 0))
            status = exec_node(n->bin.right);
        break;
    case N_PIPELINE:
        status = exec_pipeline(n);
        break;
    case N_SIMPLE:
    case N_SUBSHELL:
        /* commands are always wrapped in a pipeline node */
        abort();
    }
    return status;
}
