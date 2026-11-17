#!/bin/sh
#
# Run each tests/cases/NAME.sh through omsh and compare against:
#   NAME.out     expected stdout (empty if missing)
#   NAME.status  expected exit status (0 if missing)
#   NAME.err     expected stderr (only checked if present)
#
# Every case runs in its own empty temp directory with stdin from /dev/null.
# Set OMSH_WRAPPER to run omsh under another tool, e.g.
#   OMSH_WRAPPER="valgrind -q --leak-check=full --error-exitcode=99"
# Exit status 99 and valgrind or sanitizer reports on stderr count as
# failures even for cases that do not check stderr.

root=$(cd "$(dirname "$0")/.." && pwd)
omsh=${OMSH:-$root/omsh}
cases=$root/tests/cases

if [ ! -x "$omsh" ]; then
    echo "run.sh: $omsh not found; run make first" >&2
    exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/omsh-tests.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT TERM

pass=0
fail=0

for script in "$cases"/*.sh; do
    name=$(basename "$script" .sh)
    dir=$tmp/work/$name
    mkdir -p "$dir"
    out=$tmp/$name.stdout
    err=$tmp/$name.stderr

    # OMSH_WRAPPER is intentionally unquoted so it splits into words.
    (cd "$dir" && $OMSH_WRAPPER "$omsh" "$script" >"$out" 2>"$err" </dev/null)
    status=$?

    want_status=0
    [ -f "$cases/$name.status" ] && want_status=$(cat "$cases/$name.status")
    want_out=$cases/$name.out
    [ -f "$want_out" ] || want_out=/dev/null

    problems=""
    cmp -s "$out" "$want_out" || problems="$problems stdout"
    if [ -f "$cases/$name.err" ] && ! cmp -s "$err" "$cases/$name.err"; then
        problems="$problems stderr"
    fi
    # 99 is the --error-exitcode CI gives valgrind: memory errors or leaks.
    if [ "$status" = 99 ] && [ "$want_status" != 99 ]; then
        problems="$problems valgrind(status 99)"
    elif [ "$status" != "$want_status" ]; then
        problems="$problems status(got $status, want $want_status)"
    fi
    # Reports from forked children do not change the exit status.
    if grep -Eq '^==[0-9]+==|ERROR: (Address|Leak)Sanitizer|runtime error:' \
        "$err"; then
        problems="$problems sanitizer"
    fi

    if [ -z "$problems" ]; then
        echo "PASS $name"
        pass=$((pass + 1))
    else
        echo "FAIL $name:$problems"
        fail=$((fail + 1))
        case $problems in *stdout*)
            diff -u "$want_out" "$out" | sed 's/^/    /' | head -20 ;;
        esac
        case $problems in *stderr*|*sanitizer*|*valgrind*)
            if [ -f "$cases/$name.err" ]; then
                diff -u "$cases/$name.err" "$err"
            else
                cat "$err"
            fi | sed 's/^/    /' | head -20 ;;
        esac
    fi
done

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
