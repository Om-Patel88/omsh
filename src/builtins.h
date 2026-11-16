#ifndef OMSH_BUILTINS_H
#define OMSH_BUILTINS_H

typedef int (*builtin_fn)(int argc, char **argv);

/* Look up a builtin by name; NULL if name is not one. */
builtin_fn builtin_find(const char *name);

#endif
