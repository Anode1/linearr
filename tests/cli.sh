#!/bin/sh
# cli.sh -- black-box tests: the built binary, driven through the shell, the way
# a user meets it. `make ut` cannot see any of this. It exists because of one
# defect it would have caught instantly: a bare `./linearr` sat reading stdin
# and looked hung, and only printed its usage after a Ctrl-C. Every unit test
# was green, because a unit test never has a terminal on stdin.
#
#   make cliut          # or: sh tests/cli.sh
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
bin=$root/linearr
[ -x "$bin" ] || { echo "build first: make" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0; fail=0; skip=0
ok()   { pass=$((pass+1)); }
no()   { fail=$((fail+1)); echo "  FAIL $1"; }
check(){ if [ "$2" = "$3" ]; then ok; else no "$1: expected [$3], got [$2]"; fi; }

CASE1='001,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0'
EXPECT1='001 prediction=19.9611 trim=46.5'

cd "$root"

# --- an argument is scored -------------------------------------------------
check "argument"      "$("$bin" "$CASE1")" "$EXPECT1"
check "argument exit" "$?" "0"

# --- a pipe is a filter ----------------------------------------------------
check "pipe" "$(printf '%s\n%s\n' "$CASE1" "$CASE1" | "$bin" | wc -l | tr -d ' ')" "2"
check "redirect" "$(printf '%s\n' "$CASE1" > "$tmp/c.csv"; "$bin" < "$tmp/c.csv")" "$EXPECT1"

# Comments and blank lines are skipped, not scored and not complained about.
check "pipe skips comments" \
    "$(printf '# a note\n\n%s\n' "$CASE1" | "$bin" | wc -l | tr -d ' ')" "1"

# --- an empty pipe is an ordinary outcome, not an error --------------------
out=$(printf '' | "$bin" 2>&1); rc=$?
check "empty pipe output" "$out" ""
check "empty pipe exit"   "$rc"  "0"

# --- a bad row is reported, and the exit code says so ----------------------
set +e
out=$(printf '001,1,2\n' | "$bin" 2>&1); rc=$?
set -e
check "bad row exit" "$rc" "1"
case "$out" in *"cannot score"*) ok ;; *) no "bad row message: got [$out]" ;; esac

# A bad row must not throw away the good ones around it.
check "bad row does not stop the batch" \
    "$(printf '%s\n001,1,2\n%s\n' "$CASE1" "$CASE1" 2>/dev/null | "$bin" 2>/dev/null | wc -l | tr -d ' ')" "2"

# --- -h ---------------------------------------------------------------------
check "-h exit" "$("$bin" -h >/dev/null 2>&1; echo $?)" "0"
case "$("$bin" -h 2>&1)" in usage:*) ok ;; *) no "-h prints usage" ;; esac

# --- THE regression: a bare run on a terminal ------------------------------
# It must print the usage and exit at once. Reading stdin here is the bug: the
# program looks hung. Needs a pty; if we cannot allocate one, say SKIP rather
# than quietly passing.
flavour=
if   script -qec true /dev/null >/dev/null 2>&1; then flavour=linux   # util-linux
elif script -q /dev/null true   >/dev/null 2>&1; then flavour=bsd     # BSD / macOS
fi

if [ -n "$flavour" ] && command -v timeout >/dev/null 2>&1; then
    set +e
    if [ "$flavour" = linux ]; then
        timeout 5 script -qec "$bin" /dev/null > "$tmp/bare.txt" 2>&1
    else
        timeout 5 script -q /dev/null "$bin"   > "$tmp/bare.txt" 2>&1
    fi
    rc=$?
    set -e
    # timeout exits 124 when it had to kill the command -- that is the hang.
    check "bare run does not hang" "$rc" "0"
    out=$(head -1 "$tmp/bare.txt" | tr -d '\r')
    case "$out" in usage:*) ok ;; *) no "bare run prints usage: got [$out]" ;; esac
else
    skip=$((skip+2)); echo "  SKIP bare-run-on-a-terminal (no pty or no timeout here)"
fi

# --- fit, then score against what was fitted -------------------------------
"$bin" -t example/simple-train.csv -g A 2>/dev/null > "$tmp/coef.csv"
check "fit writes a header" "$(head -1 "$tmp/coef.csv")" "GROUP,Intercept,km,stops"
check "fit writes the row"  "$(tail -1 "$tmp/coef.csv")" "A,5.0000,2.5000,1.5000"

# The fit summary goes to stderr, so stdout stays a clean coefficient file.
case "$("$bin" -t example/simple-train.csv -g A 2>&1 >/dev/null)" in
    *"R2="*) ok ;; *) no "fit summary on stderr" ;;
esac

printf 'coef.file = %s\ntrim.file =\n' "$tmp/coef.csv" > "$tmp/system.properties"
check "score against the fitted table" \
    "$(cd "$tmp" && "$bin" 'A,10,3')" "A prediction=34.5000 trim=34.5"

# --- an unreadable table named in the config is an error, not a shrug ------
printf 'coef.file = %s/nope.csv\n' "$tmp" > "$tmp/system.properties"
set +e
(cd "$tmp" && "$bin" 'A,10,3' >/dev/null 2>&1); rc=$?
set -e
check "missing coefficient file exits nonzero" "$rc" "1"

echo "cliut: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
