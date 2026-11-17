# omsh design

omsh is a small interactive Unix shell written in C11 against POSIX.1-2008.
It builds with `cc` on macOS and Linux and has no dependencies beyond libc.

## Pipeline of a command line

```
input line -> lexer (tokens) -> parser (AST) -> expansion -> executor
```

1. **Lexer** (`src/lexer.c`) turns a line into tokens: WORD, `|`, `&&`, `||`,
   `;`, `&`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`, `(`, `)`, NEWLINE and END.
   Quoting is resolved here: single quotes are literal, double quotes allow
   `$VAR` / `${VAR}` / `$?` / `$$` / `$!` expansion and `\"`, `\\`, `\$`
   escapes, and a backslash outside quotes escapes the next character. Each
   word is a list of segments saying which parts were quoted, so later
   stages know not to glob or split quoted text. The body of a `$(...)`,
   found by matching parentheses while skipping quotes and nested `$(`, is
   kept as a segment of its own (one kind outside double quotes, another
   inside). Input that ends inside a quote or `$(` is the only lexing error,
   so it also means "this command continues on the next line". `#` starts a
   comment at the start of a word and runs to the end of its line.
2. **Parser** (`src/parser.c`) is recursive descent over this grammar:

   ```
   list      := and_or (sep and_or)* [sep]
   sep       := ';' | '&' | NEWLINE
   and_or    := pipeline (('&&' | '||') NEWLINE* pipeline)*
   pipeline  := ['!'] command ('|' NEWLINE* command)*
   command   := simple | '(' list ')' redirect*
   simple    := (WORD | redirect)+
   redirect  := ('<' | '>' | '>>' | '2>' | '2>>') WORD | '2>&1'
   ```

   Extra newlines are allowed where a list may start and after `;`, `&`,
   `|`, `&&` and `||`. Every AST node is heap allocated and released by one
   `ast_free`; pipelines keep their source text, which names their job.
3. **Expansion** (`src/expand.c`), per word: leading `~`, variables and
   command substitution, then field splitting, then pathname globbing with
   `glob(3)` for unquoted `*?[`. A glob with no matches stays literal, as
   in POSIX sh.
   - **Command substitution**: `$(...)` forks a copy of the shell with
     stdout on a pipe; the child runs the body with `sh_run_line` (job
     control off, its own empty job table) and exits, the parent reads to
     EOF, waits, and drops trailing newlines. Nesting works because the
     child lexes the body again. A command made only of assignments takes
     the status of its last substitution, so `x=$(false)` sets `$?` to 1.
     Backquotes are not supported.
   - **Field splitting**: unquoted variable and substitution results are
     split on `IFS` (space, tab, newline if unset; no splitting if empty).
     Runs of IFS white space separate fields and vanish at the edges; any
     other IFS character ends a field, even an empty one, so with `IFS=:`
     the value `a::b` gives `a`, ``, `b`. Quoted text never splits, so
     `"$X"` is one word while `$X` with `X="a b c"` is three. A word builds
     a list of fields, each with its own glob pattern.
   - `NAME=value` and redirection targets expand to one string with no
     splitting or globbing.
4. **Executor** (`src/exec.c`) walks the AST. A single simple command that
   is a builtin (or only assignments) runs in the shell process, so `cd`,
   `export` and friends work. Everything else is a job: one forked process
   per pipeline command (a builtin there runs in the child), N-1 pipes for
   N commands. A background `pipeline &` forks its commands directly, so
   `$!` is the real pid of the last one; a background `a && b &` forks one
   subshell that runs the whole list.

## Job control

Modelled on the job control chapter of the glibc manual, in `src/jobs.c`.

- At startup an interactive shell waits until it is in the foreground
  (sending itself SIGTTIN while `tcgetpgrp` names another group), ignores
  SIGINT, SIGQUIT, SIGTSTP, SIGTTIN and SIGTTOU, puts itself in its own
  process group (unless it already leads one as a session leader), takes
  the terminal with `tcsetpgrp` and saves its termios. If any step fails,
  job control stays off. At exit it gives the terminal back.
- One pipeline is one job is one process group, whose id is the first
  child's pid. Both parent and child call `setpgid`, so neither has to win
  the race. A foreground child also calls `tcsetpgrp` itself, before it
  resets the five signals (and SIGCHLD) to SIG_DFL and execs.
- A foreground job gets the terminal; the shell waits with
  `waitpid(-1, &st, WUNTRACED)`, recording each status against whichever
  job owns that pid, until every process of the job has exited or the job
  has stopped. Then it takes the terminal back and restores its termios,
  saving the job's own termios if it stopped.
- The table keeps, per job: id, pgid, command text, and per process its
  pid, state (running, stopped, done) and raw status, so a pipeline is done
  only when all of its processes are. Its status is the last process's.
- SIGCHLD only sets a `volatile sig_atomic_t` flag (installed with
  SA_RESTART). Before each prompt or script line the main loop reaps with
  `waitpid(-1, WNOHANG | WUNTRACED | WCONTINUED)` and, when interactive,
  prints `[1]+  Done  sleep 5` or Stopped notices. Nothing is allocated in
  a signal handler.
- Ctrl-Z stops the foreground job: the shell prints
  `[n]+  Stopped  cmd` and keeps it. `fg` hands back the terminal (and the
  saved termios) and sends SIGCONT to the group; `bg` sends SIGCONT and
  leaves it in the background. `%+` (the current job) is the one most
  recently stopped or put in the background, `%-` the one before it.
- Leaving with stopped jobs prints `There are stopped jobs.` once; a
  second `exit` or Ctrl-D in a row leaves, sending SIGHUP and SIGCONT to
  the stopped jobs on the way out.
- Without a terminal (scripts, tests, `-c`) and in forked children, job
  control is off: every process stays in the shell's group and the table
  is only used for waiting. As POSIX asks, background jobs then ignore
  SIGINT and SIGQUIT and read stdin from `/dev/null`.

## Builtins

`cd [dir|-]`, `pwd`, `exit [n]`, `export NAME[=value]`, `unset NAME`,
`jobs`, `fg [job]`, `bg [job ...]`, `wait [job|pid ...]`,
`kill [-s SIG | -SIG] job|pid ...` and `kill -l`, `echo [-n]`,
`type NAME`, `history`, `source FILE` / `. FILE`, `true`, `false`.

Job specs are `%n`, `%%` / `%+`, `%-` and `%name` (a prefix of the
command); `fg` and `bg` also take a bare job number, while for `kill` and
`wait` a bare number is a pid. Signals are named with or without `SIG` or
given by number. `kill` sends a signal to a job's whole process group, and
follows SIGTERM or SIGHUP with SIGCONT so a stopped job can act on it.

## Input

Interactive mode uses a small line reader (`src/lineedit.c`) in raw mode:
left/right, backspace, Ctrl-A/E/U/K/W, up/down history, Ctrl-D on an empty
line to exit. When stdin is not a tty, input is read with `getline`.
`omsh -c 'cmd'` and `omsh script.sh` are supported. `~/.omshrc` is sourced
at interactive start.

A line that ends inside a quote or `$(`, or with a trailing `|`, `&&` or
`||`, is not an error yet: the shell reads another line (prompting with
`PS2`, default `> `, when interactive), joins them with a newline and
tries again. At end of input the parser reports the error as usual.
`omsh -c` has no more lines to read, so there it is a syntax error.

## Exit status

`$?` follows POSIX: the last command's status, 128+signal when killed by a
signal or stopped (so 148 after Ctrl-Z), 127 for command not found, 126 for
not executable, 2 for syntax errors.

## Testing

- `tests/run.sh` runs `.sh` cases under `tests/cases/` through omsh and
  compares stdout, stderr and exit status with expected files. Status 99
  (valgrind's error code in CI) and valgrind or sanitizer reports on
  stderr fail a case even when its stderr is not compared, which catches
  reports from forked children.
- Unit tests in `tests/unit/` for the lexer and parser.
- `tests/pty_test.py` (`make ptytest`, Python standard library only) runs
  omsh on a pseudo-terminal: Ctrl-Z on `sleep 30`, `jobs`, `bg`, `fg`
  then Ctrl-C with the shell surviving, Ctrl-C at an empty prompt, up-arrow
  history and the stopped jobs warning. Control keys are resent if they
  arrive before the job owns the terminal.
- `make asan` builds with `-fsanitize=address,undefined`.
- CI (`.github/workflows/ci.yml`) builds and runs everything with gcc and
  clang on Linux, runs the script tests under valgrind
  (`--error-exitcode=99`, definite leaks are errors), runs the suite with
  ASan, UBSan and LSan on Linux, and builds and tests on macOS.
