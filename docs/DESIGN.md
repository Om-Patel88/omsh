# omsh design

omsh is a small interactive Unix shell written in C11 against POSIX.1-2008.
It builds with `cc` on macOS and Linux and has no dependencies beyond libc.

## Pipeline of a command line

```
input line -> lexer (tokens) -> parser (AST) -> expansion -> executor
```

1. **Lexer** (`src/lexer.c`) turns a line into tokens: WORD, `|`, `&&`, `||`,
   `;`, `&`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`, `(`, `)` and END.
   Quoting is resolved here: single quotes are literal, double quotes allow
   `$VAR` / `${VAR}` / `$?` / `$$` expansion and `\"`, `\\`, `\$` escapes,
   and a backslash outside quotes escapes the next character. Each word
   carries flags saying which parts were quoted, so later stages know not to
   glob quoted text. Unterminated quotes are a syntax error, not a crash.
   `#` starts a comment at the start of a word.
2. **Parser** (`src/parser.c`) is recursive descent over this grammar:

   ```
   list      := and_or ((';' | '&') and_or)* [';' | '&']
   and_or    := pipeline (('&&' | '||') pipeline)*
   pipeline  := ['!'] command ('|' command)*
   command   := simple | '(' list ')' redirect*
   simple    := (WORD | redirect)+
   redirect  := ('<' | '>' | '>>' | '2>' | '2>>') WORD | '2>&1'
   ```

   Every AST node is heap allocated and released by one `ast_free`.
3. **Expansion** (`src/expand.c`): variables, leading `~`, then pathname
   globbing with `glob(3)` for unquoted words that contain `*?[`. A glob
   with no matches stays literal, as in POSIX sh.
4. **Executor** (`src/exec.c`) walks the AST. Builtins that change shell
   state (`cd`, `exit`, `export`, `unset`, `jobs`, `fg`, `bg`) run in the
   shell process when they are not part of a pipeline. Everything else is
   `fork` + `execvp`. A pipeline of N commands makes N-1 pipes; every child
   joins one process group whose id is the first child's pid.

## Job control

Modelled on the job control chapter of the glibc manual.

- At startup an interactive shell waits until it is in the foreground,
  puts itself in its own process group, takes the terminal with
  `tcsetpgrp`, and ignores SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU.
- Children reset those signals to SIG_DFL before `exec`.
- Both parent and child call `setpgid` to avoid the race where the parent
  hands the terminal to a group that does not exist yet.
- A foreground job gets the terminal; the shell waits with
  `waitpid(-pgid, &st, WUNTRACED)` until every process has exited or the
  job has stopped, then takes the terminal back and restores its saved
  termios.
- A job table (`src/jobs.c`) records id, pgid, per-process pid/status and
  the command text. SIGCHLD only sets a flag; the main loop reaps with
  `waitpid(-1, WNOHANG | WUNTRACED | WCONTINUED)` before each prompt and
  prints `[1]+ Done  sleep 5` style notices. No allocation happens inside a
  signal handler.
- `fg %n` sends SIGCONT and waits in the foreground; `bg %n` sends SIGCONT
  and leaves it in the background; Ctrl-Z stops the foreground job and adds
  it to the table.
- When stdin is not a terminal (scripts, tests) job control is turned off
  and the shell runs everything in its own process group.

## Builtins

`cd [dir|-]`, `pwd`, `exit [n]`, `export NAME[=value]`, `unset NAME`,
`jobs`, `fg [%n]`, `bg [%n]`, `echo [-n]`, `type NAME`, `history`,
`source FILE` / `. FILE`, `true`, `false`.

## Input

Interactive mode uses a small line reader (`src/lineedit.c`) in raw mode:
left/right, backspace, Ctrl-A/E/U/K/W, up/down history, Ctrl-D on an empty
line to exit. When stdin is not a tty, input is read with `getline`.
`omsh -c 'cmd'` and `omsh script.sh` are supported. `~/.omshrc` is sourced
at interactive start.

## Exit status

`$?` follows POSIX: the last command's status, 128+signal when killed by a
signal, 127 for command not found, 126 for not executable, 2 for syntax
errors.

## Testing

- `tests/run.sh` runs `.sh` cases under `tests/cases/` through omsh and
  compares stdout, stderr and exit status with expected files.
- Unit tests in `tests/unit/` for the lexer and parser.
- `make asan` builds with `-fsanitize=address,undefined`; CI also runs the
  suite under valgrind on Linux with `--error-exitcode=1`.
