#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "exec.h"
#include "jobs.h"
#include "shell.h"

enum proc_state { P_RUNNING, P_STOPPED, P_DONE };

struct proc {
    pid_t pid;
    enum proc_state state;
    int status; /* raw wait status of the last stop or exit */
};

struct job {
    int id;
    pid_t pgid;          /* 0 without job control */
    char *cmd;
    struct proc *procs;
    size_t nprocs;
    bool foreground;     /* the shell is waiting for it right now */
    bool notified;       /* the user has seen its current state */
    unsigned long seq;   /* bigger = more recent; picks the %+ and %- jobs */
    struct termios tmodes;
    bool has_tmodes;     /* tmodes was saved when the job stopped */
};

/* Jobs in order of id. Pointers, so a job stays put while the array grows. */
static struct job **table;
static size_t njobs;
static unsigned long next_seq;

static int tty_fd = -1;
static pid_t shell_pgid;
static pid_t orig_pgrp; /* group that owned the terminal when we started */
static struct termios shell_tmodes;

static volatile sig_atomic_t child_changed = 1;

static void on_sigchld(int sig)
{
    (void)sig;
    child_changed = 1;
}

static void set_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) < 0)
        sh_perror("sigaction");
}

void jobs_init_signals(void)
{
    set_signal(SIGCHLD, on_sigchld);
}

static const int keyboard_signals[] = {
    SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU,
};
#define NKEYBOARD (sizeof keyboard_signals / sizeof keyboard_signals[0])

bool jobs_init_terminal(void)
{
    int fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
    if (fd < 0)
        return false;

    /* If we were started in the background, stop until we are put in the
     * foreground instead of fighting over the terminal. */
    for (;;) {
        pid_t fg = tcgetpgrp(fd);
        if (fg < 0)
            goto fail; /* not our controlling terminal */
        if (fg == getpgrp())
            break;
        if (kill(-getpgrp(), SIGTTIN) < 0)
            goto fail;
    }

    for (size_t i = 0; i < NKEYBOARD; i++)
        set_signal(keyboard_signals[i], SIG_IGN);

    orig_pgrp = getpgrp();
    shell_pgid = getpid();
    /* A session leader already leads its own group and may not move. */
    if (orig_pgrp != shell_pgid && setpgid(0, shell_pgid) < 0) {
        sh_perror("setpgid");
        goto fail;
    }
    if (tcsetpgrp(fd, shell_pgid) < 0 || tcgetattr(fd, &shell_tmodes) < 0) {
        sh_perror("terminal");
        goto fail;
    }
    tty_fd = fd;
    shell.job_control = true;
    return true;

fail:
    close(fd);
    return false;
}

/* ---- the table ---- */

struct job *job_new(const char *cmd)
{
    struct job *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->id = njobs ? table[njobs - 1]->id + 1 : 1;
    j->cmd = xstrdup(cmd);
    j->foreground = true;
    j->notified = true;
    table = xrealloc(table, (njobs + 1) * sizeof *table);
    table[njobs++] = j;
    return j;
}

static void job_free(struct job *j)
{
    free(j->cmd);
    free(j->procs);
    free(j);
}

static void remove_job(struct job *j)
{
    for (size_t i = 0; i < njobs; i++) {
        if (table[i] == j) {
            memmove(table + i, table + i + 1,
                    (njobs - i - 1) * sizeof *table);
            njobs--;
            break;
        }
    }
    job_free(j);
}

void jobs_forget(void)
{
    for (size_t i = 0; i < njobs; i++)
        job_free(table[i]);
    free(table);
    table = NULL;
    njobs = 0;
}

void jobs_shutdown(void)
{
    for (size_t i = 0; i < njobs; i++) {
        struct job *j = table[i];
        bool stopped = false;
        for (size_t k = 0; k < j->nprocs; k++)
            stopped |= j->procs[k].state == P_STOPPED;
        /* Stopped jobs would wait forever: hang them up and wake them. */
        if (stopped) {
            job_kill(j, SIGHUP);
            job_kill(j, SIGCONT);
        }
    }
    if (tty_fd >= 0) {
        /* Best effort: the old group may be gone, and then there is
         * nobody to hand the terminal to anyway. */
        if (orig_pgrp != shell_pgid && tcsetpgrp(tty_fd, orig_pgrp) < 0)
            errno = 0;
        close(tty_fd);
        tty_fd = -1;
    }
    jobs_forget();
}

static void add_proc(struct job *j, pid_t pid)
{
    j->procs = xrealloc(j->procs, (j->nprocs + 1) * sizeof *j->procs);
    j->procs[j->nprocs++] = (struct proc){pid, P_RUNNING, 0};
    if (shell.job_control && j->pgid == 0)
        j->pgid = pid;
}

pid_t job_fork(struct job *j, bool foreground)
{
    /* Anything left in our stdio buffers would be written twice. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        sh_perror("fork");
        return -1;
    }
    if (pid == 0) {
        if (shell.job_control) {
            pid_t pgid = j->pgid ? j->pgid : getpid();
            if (setpgid(0, pgid) < 0)
                sh_perror("setpgid");
            /* Before SIGTTOU goes back to default, or this would stop us. */
            if (foreground && tcsetpgrp(tty_fd, pgid) < 0)
                sh_perror("tcsetpgrp");
            for (size_t i = 0; i < NKEYBOARD; i++)
                set_signal(keyboard_signals[i], SIG_DFL);
            set_signal(SIGCHLD, SIG_DFL);
            close(tty_fd);
            tty_fd = -1;
            /* Commands run inside this process stay in its group. */
            shell.job_control = false;
        }
        jobs_forget();
        return 0;
    }
    add_proc(j, pid);
    /* EACCES: the child already ran exec; ESRCH: it already exited.
     * Either way it did its own setpgid first. */
    if (shell.job_control && setpgid(pid, j->pgid) < 0 && errno != EACCES &&
        errno != ESRCH)
        sh_perror("setpgid");
    return pid;
}

int job_id(const struct job *j)
{
    return j->id;
}

const char *job_cmd(const struct job *j)
{
    return j->cmd;
}

pid_t job_last_pid(const struct job *j)
{
    return j->nprocs ? j->procs[j->nprocs - 1].pid : 0;
}

struct job *jobs_find_pid(pid_t pid)
{
    for (size_t i = 0; i < njobs; i++)
        for (size_t k = 0; k < table[i]->nprocs; k++)
            if (table[i]->procs[k].pid == pid)
                return table[i];
    return NULL;
}

static bool job_done(const struct job *j)
{
    for (size_t k = 0; k < j->nprocs; k++)
        if (j->procs[k].state != P_DONE)
            return false;
    return true;
}

/* Stopped: not finished, and nothing in it is still running. */
static bool job_stopped(const struct job *j)
{
    bool any = false;
    for (size_t k = 0; k < j->nprocs; k++) {
        if (j->procs[k].state == P_RUNNING)
            return false;
        any |= j->procs[k].state == P_STOPPED;
    }
    return any;
}

static int job_status(const struct job *j)
{
    if (j->nprocs == 0)
        return 1; /* the first fork failed */
    for (size_t k = 0; k < j->nprocs; k++)
        if (j->procs[k].state == P_STOPPED)
            return 128 + WSTOPSIG(j->procs[k].status);
    return exec_status(j->procs[j->nprocs - 1].status);
}

/* ---- waiting ---- */

static void mark(pid_t pid, int st)
{
    struct job *j = jobs_find_pid(pid);
    if (!j)
        return;
    for (size_t k = 0; k < j->nprocs; k++) {
        struct proc *p = &j->procs[k];
        if (p->pid != pid)
            continue;
        if (WIFSTOPPED(st)) {
            p->state = P_STOPPED;
            p->status = st;
            j->notified = false;
        } else if (WIFCONTINUED(st)) {
            p->state = P_RUNNING;
        } else {
            p->state = P_DONE;
            p->status = st;
            j->notified = false;
        }
    }
}

/* waitpid says we have no children, so nothing in the table can still be
 * running. Mark it all finished rather than wait forever. */
static void lost_children(void)
{
    for (size_t i = 0; i < njobs; i++)
        for (size_t k = 0; k < table[i]->nprocs; k++)
            if (table[i]->procs[k].state != P_DONE)
                table[i]->procs[k].state = P_DONE;
}

/* Wait for one status change and record it. Returns false when there was
 * nothing to collect (WNOHANG) or no children are left. */
static bool wait_change(int flags)
{
    int st;
    pid_t pid = waitpid(-1, &st, flags);
    if (pid < 0) {
        if (errno == EINTR)
            return true;
        if (errno != ECHILD)
            sh_perror("waitpid");
        lost_children();
        return false;
    }
    if (pid == 0)
        return false;
    mark(pid, st);
    return true;
}

static void poll_children(void)
{
    child_changed = 0;
    while (wait_change(WNOHANG | WUNTRACED | WCONTINUED))
        ;
}

int job_kill(struct job *j, int sig)
{
    if (j->pgid > 0)
        return kill(-j->pgid, sig);
    int rc = 0;
    for (size_t k = 0; k < j->nprocs; k++)
        if (j->procs[k].state != P_DONE && kill(j->procs[k].pid, sig) < 0)
            rc = -1;
    return rc;
}

static void continue_job(struct job *j)
{
    if (job_kill(j, SIGCONT) < 0)
        sh_perror("kill");
    for (size_t k = 0; k < j->nprocs; k++)
        if (j->procs[k].state == P_STOPPED)
            j->procs[k].state = P_RUNNING;
    j->notified = true;
}

/* ---- printing ---- */

/* '+' for the current job, '-' for the previous one. */
static char job_mark(const struct job *j)
{
    unsigned long first = 0, second = 0;
    for (size_t i = 0; i < njobs; i++) {
        unsigned long s = table[i]->foreground ? 0 : table[i]->seq;
        if (s > first) {
            second = first;
            first = s;
        } else if (s > second) {
            second = s;
        }
    }
    if (j->seq && j->seq == first)
        return '+';
    if (j->seq && j->seq == second)
        return '-';
    return ' ';
}

static void print_job(FILE *f, const struct job *j)
{
    char state[64];

    if (job_done(j)) {
        int st = j->nprocs ? j->procs[j->nprocs - 1].status : 0;
        if (WIFSIGNALED(st))
            snprintf(state, sizeof state, "Killed (signal %d)",
                     WTERMSIG(st));
        else if (WEXITSTATUS(st) != 0)
            snprintf(state, sizeof state, "Exit %d", WEXITSTATUS(st));
        else
            snprintf(state, sizeof state, "Done");
    } else if (job_stopped(j)) {
        snprintf(state, sizeof state, "Stopped");
    } else {
        snprintf(state, sizeof state, "Running");
    }
    fprintf(f, "[%d]%c  %-20s %s\n", j->id, job_mark(j), state, j->cmd);
}

void jobs_reap(bool notify)
{
    if (child_changed)
        poll_children();
    fflush(stdout);
    for (size_t i = 0; i < njobs;) {
        struct job *j = table[i];
        if (j->foreground) {
            i++;
            continue;
        }
        if (job_done(j)) {
            if (notify)
                print_job(stderr, j);
            remove_job(j);
            continue;
        }
        if (!j->notified && notify && job_stopped(j))
            print_job(stderr, j);
        j->notified = true;
        i++;
    }
}

void jobs_print(void)
{
    poll_children();
    for (size_t i = 0; i < njobs; i++)
        if (!table[i]->foreground)
            print_job(stdout, table[i]);
    for (size_t i = 0; i < njobs;) {
        table[i]->notified = true;
        if (!table[i]->foreground && job_done(table[i]))
            remove_job(table[i]);
        else
            i++;
    }
}

/* ---- foreground and background ---- */

static void take_terminal(struct job *j)
{
    if (tcsetpgrp(tty_fd, shell_pgid) < 0)
        sh_perror("tcsetpgrp");
    if (j && job_stopped(j)) {
        if (tcgetattr(tty_fd, &j->tmodes) == 0)
            j->has_tmodes = true;
    }
    if (tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes) < 0)
        sh_perror("tcsetattr");
}

int job_foreground(struct job *j, bool cont)
{
    bool use_tty = shell.job_control && j->pgid > 0;

    j->foreground = true;
    if (use_tty) {
        if (cont && j->has_tmodes &&
            tcsetattr(tty_fd, TCSADRAIN, &j->tmodes) < 0)
            sh_perror("tcsetattr");
        if (tcsetpgrp(tty_fd, j->pgid) < 0)
            sh_perror("tcsetpgrp");
    }
    if (cont)
        continue_job(j);

    int flags = shell.job_control ? WUNTRACED : 0;
    while (!job_done(j) && !job_stopped(j))
        if (!wait_change(flags))
            break;
    if (use_tty)
        take_terminal(j);

    int status = job_status(j);
    if (job_stopped(j)) {
        j->foreground = false;
        j->seq = ++next_seq;
        j->notified = true;
        fflush(stdout);
        fputc('\n', stderr);
        print_job(stderr, j);
    } else {
        remove_job(j);
    }
    return status;
}

void job_background(struct job *j, bool cont)
{
    j->foreground = false;
    j->seq = ++next_seq;
    if (cont)
        continue_job(j);
    shell.last_bg_pid = job_last_pid(j);
}

/* ---- lookup and the wait builtin ---- */

static struct job *job_by_rank(int rank)
{
    struct job *best[2] = {NULL, NULL};
    for (size_t i = 0; i < njobs; i++) {
        struct job *j = table[i];
        if (j->foreground)
            continue;
        if (!best[0] || j->seq > best[0]->seq) {
            best[1] = best[0];
            best[0] = j;
        } else if (!best[1] || j->seq > best[1]->seq) {
            best[1] = j;
        }
    }
    return best[rank];
}

static bool all_digits(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

struct job *jobs_find(const char *spec, const char *who, bool allow_number)
{
    struct job *j = NULL;
    const char *s = spec;

    if (!spec || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0 ||
        strcmp(spec, "%") == 0) {
        j = job_by_rank(0);
        if (!j)
            sh_error("%s: current: no such job", who);
        return j;
    }
    if (strcmp(spec, "%-") == 0) {
        j = job_by_rank(1);
    } else if (*s == '%' || allow_number) {
        if (*s == '%')
            s++;
        if (all_digits(s)) {
            int id = atoi(s);
            for (size_t i = 0; i < njobs && !j; i++)
                if (table[i]->id == id && !table[i]->foreground)
                    j = table[i];
        } else if (*spec == '%') {
            /* %name: the job whose command starts with name */
            for (size_t i = 0; i < njobs && !j; i++)
                if (!table[i]->foreground &&
                    strncmp(table[i]->cmd, s, strlen(s)) == 0)
                    j = table[i];
        }
    }
    if (!j)
        sh_error("%s: %s: no such job", who, spec);
    return j;
}

static bool any_running(void)
{
    for (size_t i = 0; i < njobs; i++)
        if (!table[i]->foreground && !job_done(table[i]) &&
            !job_stopped(table[i]))
            return true;
    return false;
}

int jobs_wait(struct job *j)
{
    int flags = shell.job_control ? WUNTRACED : 0;

    if (!j) {
        while (any_running() && wait_change(flags))
            ;
        return 0;
    }
    while (!job_done(j) && !job_stopped(j) && wait_change(flags))
        ;
    int status = job_status(j);
    if (job_done(j))
        remove_job(j);
    return status;
}

bool jobs_any_stopped(void)
{
    for (size_t i = 0; i < njobs; i++)
        if (job_stopped(table[i]))
            return true;
    return false;
}
