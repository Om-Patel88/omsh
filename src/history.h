#ifndef OMSH_HISTORY_H
#define OMSH_HISTORY_H

#include <stddef.h>

/* In-memory command history, oldest entry first. */
void history_add(const char *line);
size_t history_len(void);
/* Entry i (0 = oldest), or NULL if out of range. */
const char *history_get(size_t i);
/* Number shown by `history` for entry i (keeps counting after old
 * entries are dropped). */
size_t history_number(size_t i);
void history_free(void);

#endif
