#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "jobs.h"
#include "shell.h"

struct job {
    int id;
    pid_t pid;
    char *cmd;
    bool done;
    int status; /* raw wait status once done */
};

static struct job *jobs;
static size_t njobs;

int jobs_add(pid_t pid, const char *cmd)
{
    int id = njobs ? jobs[njobs - 1].id + 1 : 1;
    jobs = xrealloc(jobs, (njobs + 1) * sizeof *jobs);
    jobs[njobs++] = (struct job){id, pid, xstrdup(cmd), false, 0};
    return id;
}

/* Poll every running job once. */
static void update(bool block)
{
    for (size_t i = 0; i < njobs; i++) {
        if (jobs[i].done)
            continue;
        int st;
        pid_t r;
        do
            r = waitpid(jobs[i].pid, &st, block ? 0 : WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == jobs[i].pid || (r < 0 && errno == ECHILD)) {
            jobs[i].done = true;
            jobs[i].status = r < 0 ? 0 : st;
        }
    }
}

static void describe(const struct job *j, char *buf, size_t len)
{
    if (!j->done)
        snprintf(buf, len, "Running");
    else if (WIFSIGNALED(j->status))
        snprintf(buf, len, "Killed (signal %d)", WTERMSIG(j->status));
    else if (WEXITSTATUS(j->status) != 0)
        snprintf(buf, len, "Exit %d", WEXITSTATUS(j->status));
    else
        snprintf(buf, len, "Done");
}

static void print_job(size_t i)
{
    char state[64];
    /* '+' marks the most recent job, '-' the one before it */
    char mark = i + 1 == njobs ? '+' : i + 2 == njobs ? '-' : ' ';
    describe(&jobs[i], state, sizeof state);
    printf("[%d]%c  %-20s %s\n", jobs[i].id, mark, state, jobs[i].cmd);
}

static void drop_done(void)
{
    size_t k = 0;
    for (size_t i = 0; i < njobs; i++) {
        if (jobs[i].done)
            free(jobs[i].cmd);
        else
            jobs[k++] = jobs[i];
    }
    njobs = k;
}

void jobs_reap(bool notify)
{
    update(false);
    if (notify) {
        for (size_t i = 0; i < njobs; i++)
            if (jobs[i].done)
                print_job(i);
        fflush(stdout);
    }
    drop_done();
}

void jobs_print(void)
{
    update(false);
    for (size_t i = 0; i < njobs; i++)
        print_job(i);
    drop_done();
}

void jobs_wait_all(void)
{
    update(true);
    drop_done();
}

void jobs_free(void)
{
    for (size_t i = 0; i < njobs; i++)
        free(jobs[i].cmd);
    free(jobs);
    jobs = NULL;
    njobs = 0;
}
