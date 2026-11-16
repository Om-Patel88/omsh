#ifndef OMSH_EXPAND_H
#define OMSH_EXPAND_H

#include <stddef.h>

#include "lexer.h"

/*
 * Expand words into a NULL-terminated argv: variables, a leading ~, then
 * globbing of unquoted *?[. Unquoted words that expand to nothing are
 * dropped. *argc gets the number of strings.
 */
char **expand_words(struct word *const *words, size_t n, size_t *argc);

/*
 * Expand one word to a single string with no globbing, starting skip bytes
 * into its first segment (used for the value part of NAME=value).
 */
char *expand_word(const struct word *w, size_t skip);

void argv_free(char **argv);

#endif
