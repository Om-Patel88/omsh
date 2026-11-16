#ifndef OMSH_EXEC_H
#define OMSH_EXEC_H

#include "parser.h"

/* Run an AST and return its exit status. Updates shell.last_status. */
int exec_node(struct node *n);

/* Turn a wait(2) status into a shell exit status (128+sig if killed). */
int exec_status(int wstatus);

#endif
