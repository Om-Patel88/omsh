#ifndef OMSH_CHECK_H
#define OMSH_CHECK_H

#include <stdio.h>

static int checks_run, checks_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks_run++;                                                     \
        if (!(cond)) {                                                    \
            checks_failed++;                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,        \
                    __LINE__, #cond);                                     \
        }                                                                 \
    } while (0)

#define CHECK_DONE(name)                                                  \
    (printf("%s: %d/%d checks passed\n", name,                            \
            checks_run - checks_failed, checks_run),                      \
     checks_failed ? 1 : 0)

#endif
