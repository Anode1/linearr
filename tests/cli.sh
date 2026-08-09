#!/bin/sh
# cli.sh: black-box tests: the built binary, driven through the shell, the way
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

# an argument is scored
check "argument"      "$("$bin" "$CASE1")" "$EXPECT1"
check "argument exit" "$?" "0"

# a pipe is a filter
check "pipe" "$(printf '%s\n%s\n' "$CASE1" "$CASE1" | "$bin" | wc -l | tr -d ' ')" "2"
check "redirect" "$(printf '%s\n' "$CASE1" > "$tmp/c.csv"; "$bin" < "$tmp/c.csv")" "$EXPECT1"

# Comments and blank lines are skipped, not scored and not complained about.
check "pipe skips comments" \
    "$(printf '# a note\n\n%s\n' "$CASE1" | "$bin" | wc -l | tr -d ' ')" "1"

# an empty pipe is an ordinary outcome, not an error
out=$(printf '' | "$bin" 2>&1); rc=$?
check "empty pipe output" "$out" ""
check "empty pipe exit"   "$rc"  "0"

# a bad row is reported, and the exit code says so
set +e
out=$(printf '001,1,2\n' | "$bin" 2>&1); rc=$?
set -e
check "bad row exit" "$rc" "1"
case "$out" in *"cannot score"*) ok ;; *) no "bad row message: got [$out]" ;; esac

# A bad row must not throw away the good ones around it.
check "bad row does not stop the batch" \
    "$(printf '%s\n001,1,2\n%s\n' "$CASE1" "$CASE1" 2>/dev/null | "$bin" 2>/dev/null | wc -l | tr -d ' ')" "2"

# -h
check "-h exit" "$("$bin" -h >/dev/null 2>&1; echo $?)" "0"
case "$("$bin" -h 2>&1)" in usage:*) ok ;; *) no "-h prints usage" ;; esac

# THE regression: a bare run on a terminal
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
    # timeout exits 124 when it had to kill the command; that is the hang.
    check "bare run does not hang" "$rc" "0"
    out=$(head -1 "$tmp/bare.txt" | tr -d '\r')
    case "$out" in usage:*) ok ;; *) no "bare run prints usage: got [$out]" ;; esac
else
    skip=$((skip+2)); echo "  SKIP bare-run-on-a-terminal (no pty or no timeout here)"
fi

# naming the terms instead of counting commas
check "named form" "$("$bin" 001 Cardioversion=1 icu_indicator=1)" "$EXPECT1"
# The two ways of writing one case must agree, or having two is a liability.
check "named form agrees with the row form" \
    "$("$bin" 001 Cardioversion=1 icu_indicator=1)" "$("$bin" "$CASE1")"
check "named form is case-insensitive" \
    "$("$bin" 001 CARDIOVERSION=1 icu_indicator=1)" "$EXPECT1"

# --terms: the answer to "what am I supposed to type?"
check "--terms exit" "$("$bin" --terms >/dev/null 2>&1; echo $?)" "0"
case "$("$bin" --terms)" in *"24 terms and 12 groups"*) ok ;;
    *) no "--terms reports the size of the model" ;; esac
case "$("$bin" --terms)" in *icu_indicator*) ok ;;
    *) no "--terms lists the term names" ;; esac

# errors name the thing that was wrong
# The old message was "cannot score: <the whole case>" whatever went wrong,
# which sent the user to check data that was never the problem.
set +e
for probe in "001 nosuchterm=1|nosuchterm" \
             "999 icu_indicator=1|no group" \
             "001 icu_indicator=yes|not a finite number"; do
    args=${probe%|*}; want=${probe#*|}
    out=$("$bin" $args 2>&1)
    case "$out" in *"$want"*) ok ;; *) no "error names '$want': got [$out]" ;; esac
done
set -e

# it runs from somewhere else, like an installed program
# This is the defect that made the tool usable only inside its own source tree.
check "runs from another directory" "$(cd "$tmp" && "$bin" 001 Cardioversion=1 icu_indicator=1)" "$EXPECT1"
# Coefficients are written at full precision, so compare the VALUES. The old
# %.4f made this a string match, and made any coefficient below 5e-5 a zero.
coefs() { tail -1 | cut -d, -f2- | tr ',' '\n' | awk '{printf "%.9f\n", $1}' | paste -sd' ' -; }
check "-t finds its example from another directory" \
    "$(cd "$tmp" && "$bin" -t example/simple-train.csv -g A 2>/dev/null | coefs)" \
    "5.000000000 2.500000000 1.500000000"

# a missing table is one fatal message, not one complaint per row
printf 'coef.file = definitely-not-here.csv\n' > "$tmp/system.properties"
set +e
out=$(cd "$tmp" && printf '%s\n%s\n%s\n' "$CASE1" "$CASE1" "$CASE1" | "$bin" 2>&1); rc=$?
set -e
check "missing table exits nonzero" "$rc" "1"
check "missing table is reported once, not per row" \
    "$(printf '%s' "$out" | grep -c 'definitely-not-here')" "1"
case "$out" in *"looked in"*) ok ;; *) no "missing table says where it looked: got [$out]" ;; esac
rm -f "$tmp/system.properties"

# fit, then score against what was fitted
"$bin" -t example/simple-train.csv -g A 2>/dev/null > "$tmp/coef.csv"
check "fit writes a header" "$(head -1 "$tmp/coef.csv")" "GROUP,Intercept,km,stops"
check "fit writes the row"  "$(coefs < "$tmp/coef.csv")" "5.000000000 2.500000000 1.500000000"

# THE ROUND TRIP, which %.4f used to break silently: a coefficient below 5e-5
# was written as 0.0000, so a fit reporting R2=1.0000 published a constant model.
printf 'GROUP,VALUE,bytes\nA,3.0,0\nA,3.15,100000\nA,3.30,200000\nA,3.45,300000\n' > "$tmp/tiny.csv"
"$bin" -t "$tmp/tiny.csv" -g A 2>/dev/null > "$tmp/tiny_coef.csv"
printf 'coef.file = %s/tiny_coef.csv\ntrim.file =\n' "$tmp" > "$tmp/system.properties"
check "a tiny coefficient survives the round trip" \
    "$(cd "$tmp" && "$bin" A bytes=1000000)" "A prediction=4.5000 trim=4.5"
rm -f "$tmp/system.properties"

# The fit summary goes to stderr, so stdout stays a clean coefficient file.
case "$("$bin" -t example/simple-train.csv -g A 2>&1 >/dev/null)" in
    *"R2="*) ok ;; *) no "fit summary on stderr" ;;
esac

printf 'coef.file = %s\ntrim.file =\n' "$tmp/coef.csv" > "$tmp/system.properties"
check "score against the fitted table" \
    "$(cd "$tmp" && "$bin" 'A,10,3')" "A prediction=34.5000 trim=34.5"

# an unreadable table named in the config is an error, not a shrug
printf 'coef.file = %s/nope.csv\n' "$tmp" > "$tmp/system.properties"
set +e
(cd "$tmp" && "$bin" 'A,10,3' >/dev/null 2>&1); rc=$?
set -e
check "missing coefficient file exits nonzero" "$rc" "1"

# the build's own claims
# Twice now this Makefile has said it did something and not done it: the default
# goal was `modeclean`, so a bare `make` deleted the build and exited 0; and
# CPPFLAGS was recorded nowhere, so the README's own command for setting the
# term ceiling printed "Nothing to be done" and left the previous binary in
# place. Both were silent successes. They are assertions now.
if command -v make >/dev/null 2>&1; then
    src=$tmp/src; mkdir -p "$src"
    cp "$root"/*.c "$root"/*.h "$root"/Makefile "$src"/
    cp -r "$root/conf" "$root/example" "$src"/ 2>/dev/null || true
    mkdir -p "$src/tests"; cp "$root/tests/cli.sh" "$src/tests/" 2>/dev/null || true

    (cd "$src" && make clean >/dev/null 2>&1; make >/dev/null 2>&1)
    if [ -x "$src/linearr" ]; then ok; else no "make (default goal) builds the binary"; fi

    # A changed ceiling must reach the objects. A 32-term build has to refuse a
    # 100-term table; if CPPFLAGS is ignored, the stale 256-term binary accepts
    # it and this passes for the wrong reason.
    awk 'BEGIN{ printf "GROUP,Intercept"; for(i=1;i<=100;i++) printf ",t%d", i; printf "\n";
                printf "G"; for(i=0;i<=100;i++) printf ",1"; printf "\n" }' > "$src/c100.csv"
    printf 'coef.file = c100.csv\ntrim.file =\n' > "$src/system.properties"

    (cd "$src" && ./linearr --terms >/dev/null 2>&1) \
        && ok || no "the default build accepts a 100-term table"

    (cd "$src" && make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32' >/dev/null 2>&1)
    set +e
    (cd "$src" && ./linearr --terms >/dev/null 2>&1); rc=$?
    set -e
    check "CPPFLAGS reaches the objects (a 32-term build refuses 100 terms)" "$rc" "1"
else
    skip=$((skip+3)); echo "  SKIP build-claims (no make)"
fi

# What the reviewers found: each of these printed a confident wrong answer.
# An over-long stdin line must not become two predictions.
python3 -c "
import sys
r1='001'+',1'*24; r1=r1+' '*(70000-len(r1)); r2='002'+',2'*24
sys.stdout.write(r1+r2+chr(10))" > "$tmp/split.txt" 2>/dev/null || true
if [ -s "$tmp/split.txt" ]; then
    set +e
    out=$("$bin" < "$tmp/split.txt" 2>/dev/null); rc=$?
    set -e
    check "an over-long stdin line yields no prediction at all" "$out" ""
    check "and it is an error" "$rc" "1"
else
    skip=$((skip+2)); echo "  SKIP over-long-stdin (no python3)"
fi

# A write that failed is not a success.
if [ -w /dev/full ]; then
    set +e
    "$bin" -t example/simple-train.csv -g A > /dev/full 2>/dev/null; rc=$?
    set -e
    check "a failed write to a full disk exits nonzero" "$rc" "1"
else
    skip=$((skip+1)); echo "  SKIP /dev/full"
fi

# nan must not reach a coefficient table.
printf 'GROUP,VALUE,x\nA,1,1\nA,nan,2\nA,3,3\n' > "$tmp/nan.csv"
set +e
"$bin" -t "$tmp/nan.csv" -g A >/dev/null 2>&1; rc=$?
set -e
check "a nan in training is refused" "$rc" "1"
set +e
out=$("$bin" 001 icu_indicator=nan 2>&1); rc=$?
set -e
check "a nan on the command line is refused" "$rc" "1"

# A FIFO must not take the process away and never give it back.
if command -v mkfifo >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    mkfifo "$tmp/fifo" 2>/dev/null || true
    set +e
    timeout 5 "$bin" -c "$tmp/fifo" --no-trim --terms >/dev/null 2>&1; rc=$?
    set -e
    check "a FIFO is refused promptly, not waited on" "$rc" "1"
else
    skip=$((skip+1)); echo "  SKIP fifo"
fi

# Options that used to be accepted and silently dropped.
set +e
"$bin" -g 001 </dev/null >/dev/null 2>&1; rc=$?
set -e
check "-g without -t is an error" "$rc" "1"

# fitting every group in one pass
"$bin" -t example/train.csv > "$tmp/all.csv" 2>/dev/null
check "a fit-all writes one row per group" \
    "$(grep -c '^00' "$tmp/all.csv")" "2"
check "and the round trip reproduces the reference prediction" \
    "$("$bin" -c "$tmp/all.csv" --no-trim 001 Cardioversion=1 icu_indicator=1)" \
    "001 prediction=19.9611 trim=20.0"
check "-g '*' still pools on request" \
    "$("$bin" -t example/train.csv -g '*' 2>/dev/null | grep -c '^\*,')" "1"

# a pinned term is marked, so a redirect does not launder it into a zero
check "the fitted table records which zeroes are silences" \
    "$("$bin" -t example/train.csv 2>/dev/null | grep -c '^# pinned')" "2"
case "$("$bin" -t example/train.csv 2>/dev/null | grep '^# pinned 001')" in
    *constant*) ok ;; *) no "pinned note names the reason" ;;
esac
# and the note is a comment, so the table still reads straight back
"$bin" -t example/train.csv > "$tmp/pinned.csv" 2>/dev/null
check "a table with pinned notes still loads" \
    "$("$bin" -c "$tmp/pinned.csv" --no-trim 001 Cardioversion=1 icu_indicator=1)" \
    "001 prediction=19.9611 trim=20.0"

# tables that used to be misread silently
printf 'GROUP,Intercept,a\nX,10,1\nX,999,1\n' > "$tmp/dup.csv"
set +e
out=$("$bin" -c "$tmp/dup.csv" --no-trim X a=0 2>&1); rc=$?
set -e
check "a duplicate group code is refused" "$rc" "1"
case "$out" in *twice*) ok ;; *) no "duplicate group message: got [$out]" ;; esac

printf 'GROUP,Intercept,a\n#X,10,1\nY,20,1\n' > "$tmp/hash.csv"
set +e
out=$("$bin" -c "$tmp/hash.csv" --no-trim Y a=0 2>&1); rc=$?
set -e
check "a data row disguised as a comment is refused" "$rc" "1"

printf 'X,10,1\nY,20,1\n' > "$tmp/nohdr.csv"
set +e
out=$("$bin" -c "$tmp/nohdr.csv" --no-trim X a=0 2>&1); rc=$?
set -e
check "a headerless coefficient file is refused" "$rc" "1"
case "$out" in *header*) ok ;; *) no "headerless message names the header: got [$out]" ;; esac

# a config value that cannot be honoured is an error, not a silent default
printf 'coef.file = %s/dup2.csv\ntrim.file =\npredict.scale = 99\n' "$tmp" > "$tmp/system.properties"
printf 'GROUP,Intercept,a\nX,10,1\n' > "$tmp/dup2.csv"
set +e
(cd "$tmp" && "$bin" X a=1 >/dev/null 2>&1); rc=$?
set -e
check "an out-of-range predict.scale is refused" "$rc" "1"
rm -f "$tmp/system.properties"

# the two case forms must agree about whitespace
check "the named form tolerates a trailing space, as the row form does" \
    "$("$bin" 001 'icu_indicator=1 ' 2>&1)" "$("$bin" '001,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0 ' 2>&1)"

# --- the three ways to have no trim table, and the one that is not a way ------
# system.properties said "comment this out and the trim point is just the
# prediction". It is not: an absent key means the built-in default, so the table
# loads and the trim is applied. The file now says so; this makes sure it stays
# true whichever way the behaviour changes.
mkdir -p "$tmp/tf/conf"
printf 'GROUP,Intercept,a\nX,10,1\n'      > "$tmp/tf/conf/coefficients.csv"
printf 'GROUP,trim_addition\nX,500\n'     > "$tmp/tf/conf/trim_additions.csv"

printf 'coef.file = conf/coefficients.csv\n# trim.file = conf/trim_additions.csv\n' \
    > "$tmp/tf/system.properties"
check "a commented-out trim.file still loads the default table" \
    "$(cd "$tmp/tf" && "$bin" X a=0)" "X prediction=10.0000 trim=510.0"

printf 'coef.file = conf/coefficients.csv\ntrim.file =\n' > "$tmp/tf/system.properties"
check "an EMPTY trim.file is how you turn it off" \
    "$(cd "$tmp/tf" && "$bin" X a=0)" "X prediction=10.0000 trim=10.0"

printf 'coef.file = conf/coefficients.csv\n' > "$tmp/tf/system.properties"
check "--no-trim turns it off too" \
    "$(cd "$tmp/tf" && "$bin" --no-trim X a=0)" "X prediction=10.0000 trim=10.0"

echo "cliut: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
