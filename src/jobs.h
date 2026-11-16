#ifndef OMSH_JOBS_H
#define OMSH_JOBS_H

#include <stdbool.h>
#include <sys/types.h>

/*
 * Table of background jobs. For now each job is a single forked process
 * that runs one list item; there is no terminal control yet.
 */

/* Record a new background job and return its job number. */
int jobs_add(pid_t pid, const char *cmd);
/* Collect finished jobs without blocking. With notify, print a Done line
 * for each one. Finished jobs are removed from the table. */
void jobs_reap(bool notify);
/* The `jobs` builtin: list every job, then forget the finished ones. */
void jobs_print(void);
/* Block until every background job has finished. */
void jobs_wait_all(void);
void jobs_free(void);

#endif
