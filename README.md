# omsh

[![CI](https://github.com/Om-Patel88/omsh/actions/workflows/ci.yml/badge.svg)](https://github.com/Om-Patel88/omsh/actions/workflows/ci.yml)

omsh is a small Unix shell written in C11 and plain POSIX. It has a hand-written
lexer and recursive descent parser, pipelines, redirection, command
substitution and full job control (Ctrl-Z, `fg`, `bg`, `jobs`). It builds with
no dependencies beyond libc and runs on Linux and macOS.

I wrote it to understand what actually happens between pressing Enter and a
process running: process groups, who owns the terminal, how signals reach a
pipeline, and how a shell reaps children without racing them.

```
$ ./omsh
omsh$ echo "hello $USER" | tr a-z A-Z
HELLO OMPATEL
omsh$ n=$(ls src | wc -l); echo "$n source files"
      21 source files
omsh$ sleep 30 | cat
^Z
[1]+  Stopped              sleep 30 | cat
omsh$ bg
[1]+ sleep 30 | cat &
omsh$ jobs
[1]+  Running              sleep 30 | cat
omsh$ kill %1
omsh$
[1]+  Killed (signal 15)   sleep 30 | cat
```

## Features

- **Quoting:** single quotes, double quotes, backslash escapes, and adjacent
  pieces like `a"b"'c'` joined into one word.
- **Expansion:** `$VAR`, `${VAR}`, `$?`, `$$`, `$!`, `$0`-`$9`, `$#`, `~`,
  nested `$(...)`, field splitting on `IFS`, and globbing with `glob(3)`.
- **Command structure:** pipelines of any length, `&&`, `||`, `;`, `&`, `!`,
  and `( subshells )`.
- **Redirection:** `<`, `>`, `>>`, `2>`, `2>>` and `2>&1`.
- **Job control:** one process group per pipeline, terminal handoff with
  `tcsetpgrp`, Ctrl-Z / Ctrl-C going to the foreground job only, `Done` and
  `Stopped` notices, and a warning before exiting with stopped jobs.
- **Builtins:** `cd` (with `cd -`), `pwd`, `exit`, `export`, `unset`, `echo`,
  `type`, `history`, `source` / `.`, `jobs`, `fg`, `bg`, `wait`, `kill`, `true`
  and `false`.
- **Line editor:** written from scratch in raw terminal mode, with no
  readline. It supports history on the arrow keys and Emacs-style Ctrl keys
  (A, E, B, F, U, K, W, L).
- **Continuation lines:** an unclosed quote or a trailing `|` / `&&` / `||`
  asks for more input with `> `.
- **Ways to run it:** `omsh -c 'cmd'`, `omsh script.sh` or piped stdin.
  `~/.omshrc` is loaded for interactive sessions.
- **Exit codes:** POSIX values, so 127 means not found, 126 means not
  executable, 2 means a syntax error and 128+N means killed by signal N.

## Build and run

```sh
make            # builds ./omsh
./omsh
```

Requires a C11 compiler and GNU make. Tested with clang on macOS and with gcc and
clang on Ubuntu in CI.

## How it works

A command line goes through four stages:

```
line -> lexer -> parser (AST) -> expansion -> executor
```

| File | Job |
| --- | --- |
| `src/lexer.c` | Splits input into tokens, resolves quoting, finds `$(...)` bodies |
| `src/parser.c` | Recursive descent into an AST of lists, and/or chains, pipelines, subshells |
| `src/expand.c` | Variables, `~`, command substitution, field splitting, globbing |
| `src/exec.c` | Forks pipelines, wires pipes and redirections, runs builtins in-process |
| `src/jobs.c` | Job table, process groups, terminal ownership, reaping |
| `src/builtins.c` | The builtin commands |
| `src/lineedit.c` | Raw-mode line editor and history recall |
| `src/shell.c` | Shell state, reading scripts and continuation lines, error messages |
| `src/vars.c` | Shell and environment variables |
| `src/history.c` | Command history |
| `src/main.c` | Startup, `-c` and script modes, the interactive loop |

The grammar:

```
list      := and_or ((';' | '&' | NEWLINE) and_or)* [';' | '&']
and_or    := pipeline (('&&' | '||') pipeline)*
pipeline  := ['!'] command ('|' command)*
command   := simple | '(' list ')' redirect*
simple    := (WORD | redirect)+
```

Some decisions that took the most thought:

- **Both parent and child call `setpgid`.** Without that, the parent can try
  to hand the terminal to a process group that doesn't exist yet, depending on
  which process the scheduler runs first.
- **The SIGCHLD handler only sets a flag.** All `waitpid` calls happen in the
  main loop, so the handler never touches the job table or allocates memory.
- **A builtin with no pipeline runs inside the shell.** `cd` and `export` have
  to change the shell's own state, so their redirections are applied and then
  undone around the call. In a pipeline they run in a forked child like
  anything else.
- **Job control is off unless stdin is a terminal.** Scripts and `-c` run
  everything in the shell's own process group, which is why the test suite can
  run headless.

[docs/DESIGN.md](docs/DESIGN.md) covers each stage in more detail.

## Testing

```sh
make test       # script tests: stdout, stderr and exit status per case
make unit       # unit tests for the lexer and parser
make ptytest    # drives omsh through a pseudo-terminal: Ctrl-Z, fg, bg, Ctrl-C, history
make asan       # rebuild with AddressSanitizer and UBSan; later test runs use it
```

CI runs on every push:

- `gcc` and `clang` builds on Ubuntu.
- The script tests under valgrind, failing on any memory error or definite leak.
- An ASan, UBSan and LeakSanitizer build.
- A macOS build.

## Limitations

- No `$((...))` arithmetic, backquotes, `if` / `for` / `while`, functions,
  or here-documents. omsh is a job control and process management project,
  not a full POSIX sh.
- The line editor treats input as single-byte text, so multibyte UTF-8 moves
  the cursor wrongly.
- There is no tab completion.
