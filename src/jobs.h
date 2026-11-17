#ifndef OMSH_JOBS_H
#define OMSH_JOBS_H

#include <stdbool.h>
#include <sys/types.h>

/*
 * Jobs. Every pipeline the shell forks is one job. With job control on,
 * a job is also one process group; without it (scripts, subshells) its
 * processes stay in the shell's group and only the table is used.
 */

struct job;

/* Install the SIGCHLD handler. Called once at startup in every mode. */
void jobs_init_signals(void);
/* Take the terminal for an interactive shell and turn on job control.
 * Returns false (and leaves job control off) if that is not possible. */
bool jobs_init_terminal(void);
/* At exit: hang up stopped jobs, give the terminal back, free the table. */
void jobs_shutdown(void);
/* In a forked child: drop the parent's table without touching the jobs. */
void jobs_forget(void);

/* Start a new, empty job for the command text cmd. */
struct job *job_new(const char *cmd);
/*
 * Fork one process of job j. With job control the child joins the job's
 * process group (both sides call setpgid, so it does not matter who runs
 * first), takes the terminal if foreground, and gets default signals.
 * Returns the pid in the parent, 0 in the child, -1 on failure.
 */
pid_t job_fork(struct job *j, bool foreground);
/* Give the job the terminal (with job control) and wait until it exits or
 * stops. With cont, send SIGCONT first. Returns the shell exit status. */
int job_foreground(struct job *j, bool cont);
/* Leave the job running in the background. With cont, send SIGCONT. */
void job_background(struct job *j, bool cont);

int job_id(const struct job *j);
const char *job_cmd(const struct job *j);
pid_t job_last_pid(const struct job *j);
/* Send sig to every process of the job. Returns 0 or -1 with errno set. */
int job_kill(struct job *j, int sig);

/*
 * Find a job from a spec such as %2, %%, %+, %-, %name or (with
 * allow_number) a bare number. NULL spec means the current job. Prints
 * "who: ..." and returns NULL if there is no such job.
 */
struct job *jobs_find(const char *spec, const char *who, bool allow_number);
/* The job a process belongs to, or NULL. */
struct job *jobs_find_pid(pid_t pid);

/* Collect status changes without blocking and, with notify, print Done
 * and Stopped notices. Finished jobs are removed from the table. */
void jobs_reap(bool notify);
/* The `jobs` builtin. */
void jobs_print(void);
/* Block until j (or with NULL every background job) has finished or
 * stopped. Returns j's status, or 0 for NULL. */
int jobs_wait(struct job *j);
bool jobs_any_stopped(void);

#endif
