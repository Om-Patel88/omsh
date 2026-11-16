#ifndef OMSH_LINEEDIT_H
#define OMSH_LINEEDIT_H

/*
 * Read one line with prompt. On a terminal this is a small raw-mode
 * editor with history; otherwise it falls back to getline.
 * Returns a malloc'd line without the newline, or NULL at end of input.
 */
char *lineedit_read(const char *prompt);

#endif
