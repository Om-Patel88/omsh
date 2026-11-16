#include <stdlib.h>
#include <string.h>

#include "history.h"
#include "shell.h"

#define HISTORY_MAX 1000

static char *entries[HISTORY_MAX];
static size_t count;
static size_t dropped; /* entries removed from the front so far */

void history_add(const char *line)
{
    /* skip blank lines and immediate repeats */
    if (line[strspn(line, " \t")] == '\0')
        return;
    if (count && strcmp(entries[count - 1], line) == 0)
        return;
    if (count == HISTORY_MAX) {
        free(entries[0]);
        memmove(entries, entries + 1, (count - 1) * sizeof *entries);
        count--;
        dropped++;
    }
    entries[count++] = xstrdup(line);
}

size_t history_len(void)
{
    return count;
}

const char *history_get(size_t i)
{
    return i < count ? entries[i] : NULL;
}

size_t history_number(size_t i)
{
    return dropped + i + 1;
}

void history_free(void)
{
    for (size_t i = 0; i < count; i++)
        free(entries[i]);
    count = 0;
}
