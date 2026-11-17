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

static void set_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) < 0)
        sh_perror("sigaction");
}

/* Point stdin at /dev/null. */
static void stdin_from_null(void)
{
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        sh_perror("/dev/null");
        return;
    }
    if (fd != 0) {
        if (dup2(fd, 0) < 0)
            sh_perror("dup2");
        close(fd);
    }
}

/*
 * Fork one process of job j and set up the child's signals. Without job
 * control, background children ignore Ctrl-C and (if first_in_job) read
 * from /dev/null instead of the terminal, as POSIX asks.
 */
static pid_t fork_child(struct job *j, bool fg, bool first_in_job)
{
    bool had_job_control = shell.job_control;
    pid_t pid = job_fork(j, fg);
    if (pid != 0)
        return pid;

    shell.in_child = true;
    if (!fg && !had_job_control) {
        in_background = true;
        set_signal(SIGINT, SIG_IGN);
        set_signal(SIGQUIT, SIG_IGN);
        if (first_in_job)
            stdin_from_null();
    } else if (shell.interactive && !in_background) {
        set_signal(SIGINT, SIG_DFL);
        set_signal(SIGQUIT, SIG_DFL);
    }
    return 0;
}

/* State of a forked shell that is not part of a job of its own. */
static void enter_subshell(void)
{
    shell.in_child = true;
    shell.job_control = false;
    jobs_forget();
    if (shell.interactive && !in_background) {
        set_signal(SIGINT, SIG_DFL);
        set_signal(SIGQUIT, SIG_DFL);
    }
}

/* Bookkeeping after a background job has been started. */
static void announce_background(struct job *j)
{
    job_background(j, false);
    if (shell.interactive)
        fprintf(stderr, "[%d] %ld\n", job_id(j), (long)job_last_pid(j));
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
static int exec_simple(struct node *cmd, const char *text)
{
    size_t nassign = count_assignments(cmd);
    size_t argc;

    shell.subst_status = -1;
    char **argv = expand_words(cmd->simple.words + nassign,
                               cmd->simple.nwords - nassign, &argc);
    struct saved_fds save = {{-1, -1, -1}};
    int status;

    if (argc == 0) {
        /* only assignments and/or redirections; the status is that of
         * the last $(...), if there was one */
        do_assignments(cmd->simple.words, nassign, false);
        status = shell.subst_status >= 0 ? shell.subst_status : 0;
        if (apply_redirs(cmd->simple.redirs, cmd->simple.nredirs, &save) < 0)
            status = 1;
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

    struct job *j = job_new(text);
    pid_t pid = fork_child(j, true, true);
    if (pid == 0) {
        do_assignments(cmd->simple.words, nassign, true);
        if (apply_redirs(cmd->simple.redirs, cmd->simple.nredirs, NULL) < 0)
            sh_child_exit(1);
        exec_external(argv);
    }
    argv_free(argv);
    return job_foreground(j, false);
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
    shell.subst_status = -1;
    char **argv = expand_words(cmd->simple.words + nassign,
                               cmd->simple.nwords - nassign, &argc);
    do_assignments(cmd->simple.words, nassign, true);
    if (apply_redirs(cmd->simple.redirs, cmd->simple.nredirs, NULL) < 0)
        sh_child_exit(1);
    if (argc == 0)
        sh_child_exit(shell.subst_status >= 0 ? shell.subst_status : 0);

    builtin_fn fn = builtin_find(argv[0]);
    if (fn) {
        int status = fn((int)argc, argv);
        argv_free(argv);
        sh_child_exit(status_after_builtin(status));
    }
    exec_external(argv);
}

/* ---- pipelines and lists ---- */

/* Run every command of a pipeline as one job, in the foreground (returns
 * its status) or in the background (returns 0). */
static int run_job(struct node *pl, bool fg)
{
    size_t n = pl->pipe.n;
    struct job *j = job_new(pl->pipe.text);
    int in = -1; /* read end of the previous pipe */

    for (size_t i = 0; i < n; i++) {
        int fds[2] = {-1, -1};
        if (i + 1 < n && pipe(fds) < 0) {
            sh_perror("pipe");
            break;
        }
        pid_t pid = fork_child(j, fg, i == 0);
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
    }
    if (in >= 0) /* only left open if we stopped early */
        close(in);

    if (job_last_pid(j) == 0) /* nothing started: drop the job */
        return jobs_wait(j);
    if (fg)
        return job_foreground(j, false);
    announce_background(j);
    return 0;
}

static int exec_pipeline(struct node *pl)
{
    int status;
    struct node *cmd = pl->pipe.cmds[0];

    if (pl->pipe.n == 1 && cmd->kind == N_SIMPLE)
        status = exec_simple(cmd, pl->pipe.text);
    else
        status = run_job(pl, true);
    if (pl->pipe.negate)
        status = !status;
    shell.last_status = status;
    return status;
}

static void run_background(struct list_item *item)
{
    shell.last_status = 0;
    if (item->node->kind == N_PIPELINE) {
        run_job(item->node, false);
        return;
    }
    /* `a && b &` needs a shell of its own to make the decisions. */
    struct job *j = job_new(item->text);
    pid_t pid = fork_child(j, false, true);
    if (pid == 0) {
        int status = exec_node(item->node);
        sh_child_exit(status_after_builtin(status));
    }
    if (pid < 0) {
        shell.last_status = jobs_wait(j);
        return;
    }
    announce_background(j);
}

char *exec_capture(const char *cmd)
{
    int fds[2];

    shell.subst_status = 1;
    if (pipe(fds) < 0) {
        sh_perror("pipe");
        return xstrdup("");
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        sh_perror("fork");
        close(fds[0]);
        close(fds[1]);
        return xstrdup("");
    }
    if (pid == 0) {
        close(fds[0]);
        if (fds[1] != 1) {
            if (dup2(fds[1], 1) < 0)
                sh_child_exit(1);
            close(fds[1]);
        }
        enter_subshell();
        int status = sh_run_line(cmd);
        sh_child_exit(status_after_builtin(status));
    }
    close(fds[1]);

    size_t len = 0, cap = 256;
    char *out = xmalloc(cap);
    for (;;) {
        if (cap - len < 2)
            out = xrealloc(out, cap *= 2);
        ssize_t r = read(fds[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
        } else if (r == 0) {
            break;
        } else if (errno != EINTR) {
            sh_perror("read");
            break;
        }
    }
    close(fds[0]);
    while (len > 0 && out[len - 1] == '\n')
        len--;
    out[len] = '\0';

    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            sh_perror("waitpid");
            return out;
        }
    }
    shell.subst_status = exec_status(st);
    return out;
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
