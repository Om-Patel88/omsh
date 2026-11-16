#ifndef OMSH_VARS_H
#define OMSH_VARS_H

#include <stddef.h>

/*
 * Shell variables. Exported variables live in the process environment
 * (setenv), so children see them without extra work. Unexported ones live
 * in a small table owned by the shell.
 */

const char *var_get(const char *name);
/* Set a variable, keeping it exported if it already was. */
void var_set(const char *name, const char *value);
/* Export name, optionally giving it a new value. */
void var_export(const char *name, const char *value);
void var_unset(const char *name);
/* True if s is a valid variable name. */
int var_valid_name(const char *s, size_t len);
void vars_free(void);

#endif
