#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "history.h"
#include "lineedit.h"
#include "shell.h"

#define OUT_FD STDERR_FILENO /* the prompt goes to stderr, as in bash */

enum {
    CTRL_A = 1, CTRL_B = 2, CTRL_C = 3, CTRL_D = 4, CTRL_E = 5, CTRL_F = 6,
    CTRL_H = 8, CTRL_K = 11, CTRL_L = 12, CTRL_N = 14, CTRL_P = 16,
    CTRL_U = 21, CTRL_W = 23, ESC = 27, BACKSPACE = 127
};

struct editor {
    const char *prompt;
    char *buf;
    size_t len, cap, pos; /* pos is the cursor, 0..len */
    size_t hidx;          /* history entry shown; history_len() = new line */
    char *saved;          /* the new line, kept while browsing history */
};

static void write_all(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(OUT_FD, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void write_str(const char *s)
{
    write_all(s, strlen(s));
}

/* Redraw the prompt and buffer and put the cursor in place. */
static void refresh(struct editor *e)
{
    char move[32];
    write_str("\r");
    write_str(e->prompt);
    write_all(e->buf, e->len);
    write_str("\x1b[K\r");
    size_t col = strlen(e->prompt) + e->pos;
    if (col > 0) {
        snprintf(move, sizeof move, "\x1b[%zuC", col);
        write_str(move);
    }
}

static void reserve(struct editor *e, size_t n)
{
    if (e->len + n + 1 > e->cap) {
        while (e->len + n + 1 > e->cap)
            e->cap = e->cap ? e->cap * 2 : 64;
        e->buf = xrealloc(e->buf, e->cap);
    }
}

static void set_text(struct editor *e, const char *s)
{
    size_t n = strlen(s);
    e->len = 0;
    reserve(e, n);
    memcpy(e->buf, s, n + 1);
    e->len = e->pos = n;
}

static void insert(struct editor *e, char c)
{
    reserve(e, 1);
    memmove(e->buf + e->pos + 1, e->buf + e->pos, e->len - e->pos + 1);
    e->buf[e->pos++] = c;
    e->len++;
}

/* Delete n bytes starting at from. */
static void delete_range(struct editor *e, size_t from, size_t n)
{
    memmove(e->buf + from, e->buf + from + n, e->len - from - n + 1);
    e->len -= n;
}

static void history_move(struct editor *e, int dir)
{
    size_t n = history_len();
    if (dir < 0 && e->hidx == 0)
        return;
    if (dir > 0 && e->hidx >= n)
        return;
    if (e->hidx == n) { /* leaving the new line: remember it */
        free(e->saved);
        e->saved = xstrdup(e->buf);
    }
    e->hidx = dir < 0 ? e->hidx - 1 : e->hidx + 1;
    set_text(e, e->hidx == n ? e->saved : history_get(e->hidx));
}

static int read_byte(char *c)
{
    for (;;) {
        ssize_t r = read(STDIN_FILENO, c, 1);
        if (r == 1)
            return 1;
        if (r < 0 && errno == EINTR)
            continue;
        return 0;
    }
}

/* Handle the rest of an escape sequence (arrows, Home, End, Delete). */
static void handle_escape(struct editor *e)
{
    char a, b, c;
    if (!read_byte(&a) || !read_byte(&b))
        return;
    if (a == 'O') { /* some terminals send ESC O H / ESC O F */
        if (b == 'H')
            e->pos = 0;
        else if (b == 'F')
            e->pos = e->len;
        return;
    }
    if (a != '[')
        return;
    if (b >= '0' && b <= '9') {
        if (!read_byte(&c) || c != '~')
            return;
        if (b == '3' && e->pos < e->len)
            delete_range(e, e->pos, 1);
        else if (b == '1' || b == '7')
            e->pos = 0;
        else if (b == '4' || b == '8')
            e->pos = e->len;
        return;
    }
    switch (b) {
    case 'A': history_move(e, -1); break;
    case 'B': history_move(e, 1); break;
    case 'C': if (e->pos < e->len) e->pos++; break;
    case 'D': if (e->pos > 0) e->pos--; break;
    case 'H': e->pos = 0; break;
    case 'F': e->pos = e->len; break;
    }
}

/* Edit a line until Enter (returns 1) or end of input (returns 0). */
static int edit(struct editor *e)
{
    char c;

    refresh(e);
    while (read_byte(&c)) {
        switch (c) {
        case '\r':
        case '\n':
            write_str("\r\n");
            return 1;
        case CTRL_D:
            if (e->len == 0)
                return 0;
            if (e->pos < e->len)
                delete_range(e, e->pos, 1);
            break;
        case CTRL_C: /* drop the line and start over */
            write_str("^C\r\n");
            e->len = e->pos = 0;
            e->buf[0] = '\0';
            e->hidx = history_len();
            break;
        case BACKSPACE:
        case CTRL_H:
            if (e->pos > 0) {
                e->pos--;
                delete_range(e, e->pos, 1);
            }
            break;
        case CTRL_A: e->pos = 0; break;
        case CTRL_E: e->pos = e->len; break;
        case CTRL_B: if (e->pos > 0) e->pos--; break;
        case CTRL_F: if (e->pos < e->len) e->pos++; break;
        case CTRL_P: history_move(e, -1); break;
        case CTRL_N: history_move(e, 1); break;
        case CTRL_U:
            delete_range(e, 0, e->pos);
            e->pos = 0;
            break;
        case CTRL_K:
            e->len = e->pos;
            e->buf[e->len] = '\0';
            break;
        case CTRL_W: { /* delete the word before the cursor */
            size_t start = e->pos;
            while (start > 0 && e->buf[start - 1] == ' ')
                start--;
            while (start > 0 && e->buf[start - 1] != ' ')
                start--;
            delete_range(e, start, e->pos - start);
            e->pos = start;
            break;
        }
        case CTRL_L:
            write_str("\x1b[H\x1b[2J");
            break;
        case ESC:
            handle_escape(e);
            break;
        default:
            if ((unsigned char)c >= 32)
                insert(e, c);
            break;
        }
        refresh(e);
    }
    return 0;
}

static char *read_plain(const char *prompt)
{
    char *line = NULL;
    size_t cap = 0;

    write_str(prompt);
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return NULL;
    }
    if (n > 0 && line[n - 1] == '\n')
        line[n - 1] = '\0';
    return line;
}

char *lineedit_read(const char *prompt)
{
    struct termios orig, raw;

    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &orig) != 0)
        return read_plain(prompt);

    raw = orig;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0)
        return read_plain(prompt);

    struct editor e = {0};
    e.prompt = prompt;
    reserve(&e, 0);
    e.buf[0] = '\0';
    e.hidx = history_len();

    int ok = edit(&e);
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &orig) != 0)
        sh_perror("tcsetattr");
    free(e.saved);
    if (!ok) {
        write_str("\n");
        free(e.buf);
        return NULL;
    }
    return e.buf;
}
