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
check "fit writes a header" "$(head -1 "$tmp/coef.csv")" "group,intercept,km,stops"
check "fit writes the row"  "$(coefs < "$tmp/coef.csv")" "5.000000000 2.500000000 1.500000000"

# THE ROUND TRIP, which %.4f used to break silently: a coefficient below 5e-5
# was written as 0.0000, so a fit reporting R2=1.0000 published a constant model.
printf 'group,value,bytes\nA,3.0,0\nA,3.15,100000\nA,3.30,200000\nA,3.45,300000\n' > "$tmp/tiny.csv"
"$bin" -t "$tmp/tiny.csv" -g A 2>/dev/null > "$tmp/tiny_coef.csv"
printf 'coef.file = %s/tiny_coef.csv\ntrim.file =\n' "$tmp" > "$tmp/system.properties"
check "a tiny coefficient survives the round trip" \
    "$(cd "$tmp" && "$bin" A bytes=1000000)" "A prediction=4.5000"
rm -f "$tmp/system.properties"

# The fit summary goes to stderr, so stdout stays a clean coefficient file.
case "$("$bin" -t example/simple-train.csv -g A 2>&1 >/dev/null)" in
    *"R2="*) ok ;; *) no "fit summary on stderr" ;;
esac

printf 'coef.file = %s\ntrim.file =\n' "$tmp/coef.csv" > "$tmp/system.properties"
check "score against the fitted table" \
    "$(cd "$tmp" && "$bin" 'A,10,3')" "A prediction=34.5000"

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

    (cd "$src" && env -u MAKEFLAGS -u MAKELEVEL sh -c 'make clean >/dev/null 2>&1; make >/dev/null 2>&1')
    if [ -x "$src/linearr" ]; then ok; else no "make (default goal) builds the binary"; fi

    # A changed ceiling must reach the objects. A 32-term build has to refuse a
    # 100-term table; if CPPFLAGS is ignored, the stale 256-term binary accepts
    # it and this passes for the wrong reason.
    awk 'BEGIN{ printf "group,intercept"; for(i=1;i<=100;i++) printf ",t%d", i; printf "\n";
                printf "G"; for(i=0;i<=100;i++) printf ",1"; printf "\n" }' > "$src/c100.csv"
    printf 'coef.file = c100.csv\ntrim.file =\n' > "$src/system.properties"

    (cd "$src" && ./linearr --terms >/dev/null 2>&1) \
        && ok || no "the default build accepts a 100-term table"

    (cd "$src" && env -u MAKEFLAGS -u MAKELEVEL \
        make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32' >/dev/null 2>&1)
    set +e
    (cd "$src" && ./linearr --terms >/dev/null 2>&1); rc=$?
    set -e
    check "CPPFLAGS reaches the objects (a 32-term build refuses 100 terms)" "$rc" "1"

    # And the project's OWN flags have to reach the compiler with it. They once
    # did not: `.build-flags` was a prerequisite of the %.o rule, make caches the
    # directory at startup so a file $(shell) created during parsing was invisible
    # to it, the rule was rejected as inapplicable, and make's BUILT-IN %.o rule
    # ran instead, without -std=c99, without -W -Wall, without -MMD. A
    # deliberately uninitialised variable then compiled with zero diagnostics.
    # env -u MAKEFLAGS: run under `make check` this inherits the parent's flags,
    # including -w, so the first line of output is "Entering directory" and the
    # test reads make's chatter instead of a compile line. A test of the build
    # must not depend on how the test itself was started.
    (cd "$src" && env -u MAKEFLAGS -u MAKELEVEL make clean >/dev/null 2>&1)
    line=$(cd "$src" && env -u MAKEFLAGS -u MAKELEVEL \
           make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32' 2>&1 \
           | grep -m1 -- ' -c ')
    case "$line" in
        *-std=c99*) ok ;; *) no "the build uses -std=c99: got [$line]" ;;
    esac
    case "$line" in
        *-Wall*) ok ;; *) no "the build uses -Wall: got [$line]" ;;
    esac
    # The TEST build must be warning-free too. It compiles every source afresh
    # with -DUNIT_TEST, so it sees code the object build never does, and it had
    # six warnings while the README's first style rule says a warning is a
    # defect. Checking one of the two builds is half a gate. In the scratch
    # copy, never in the live tree: an earlier version of this check ran
    # `make clean` where the binary under test lives and deleted it mid-run.
    # `|| true`: grep -c exits 1 when the count is zero, and under `set -e` that
    # made a clean build kill the test run with no output at all.
    wn=$(cd "$src" && env -u MAKEFLAGS -u MAKELEVEL sh -c \
         'make clean >/dev/null 2>&1; make ut 2>&1 | grep -c "warning:" || true')
    check "the test build is warning-free" "$wn" "0"

    # A real warning must actually surface, not merely appear on the command line.
    printf '\nstatic int cli_probe(void) { int x; return x; }\n' >> "$src/utils.c"
    n=$(cd "$src" && env -u MAKEFLAGS -u MAKELEVEL \
        make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32' 2>&1 | grep -c warning)
    [ "$n" -ge 1 ] && ok || no "an uninitialised variable produces a warning (got $n)"
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
printf 'group,value,x\nA,1,1\nA,nan,2\nA,3,3\n' > "$tmp/nan.csv"
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
    "001 prediction=19.9611"
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
    "001 prediction=19.9611"

# tables that used to be misread silently
printf 'group,intercept,a\nX,10,1\nX,999,1\n' > "$tmp/dup.csv"
set +e
out=$("$bin" -c "$tmp/dup.csv" --no-trim X a=0 2>&1); rc=$?
set -e
check "a duplicate group code is refused" "$rc" "1"
case "$out" in *twice*) ok ;; *) no "duplicate group message: got [$out]" ;; esac

printf 'group,intercept,a\n#X,10,1\nY,20,1\n' > "$tmp/hash.csv"
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
printf 'group,intercept,a\nX,10,1\n' > "$tmp/dup2.csv"
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
printf 'group,intercept,a\nX,10,1\n'      > "$tmp/tf/conf/coefficients.csv"
printf 'GROUP,trim_addition\nX,500\n'     > "$tmp/tf/conf/trim_additions.csv"

printf 'coef.file = conf/coefficients.csv\n# trim.file = conf/trim_additions.csv\n' \
    > "$tmp/tf/system.properties"
check "a commented-out trim.file still loads the default table" \
    "$(cd "$tmp/tf" && "$bin" X a=0)" "X prediction=10.0000 trim=510.0"

printf 'coef.file = conf/coefficients.csv\ntrim.file =\n' > "$tmp/tf/system.properties"
check "an EMPTY trim.file is how you turn it off" \
    "$(cd "$tmp/tf" && "$bin" X a=0)" "X prediction=10.0000"

printf 'coef.file = conf/coefficients.csv\n' > "$tmp/tf/system.properties"
check "--no-trim turns it off too" \
    "$(cd "$tmp/tf" && "$bin" --no-trim X a=0)" "X prediction=10.0000"

# --- the README's teaching section must keep saying what the program says -----
# Three transcripts explain what least squares does when it cannot answer. They
# are the most quoted lines in the documentation and the easiest to leave stale.
check "piece one: the pair's effect goes to one column, and is marked" \
    "$("$bin" -t example/together.csv 2>/dev/null | tail -1)" "# pinned A: collinear headlights"
case "$("$bin" -t example/three-rows.csv 2>&1)" in
    *"no residual degrees of freedom"*) ok ;;
    *) no "piece two: three rows and three unknowns warns about df" ;;
esac
case "$("$bin" -t example/nearly-the-same.csv 2>&1)" in
    *"ill-conditioned"*) ok ;;
    *) no "piece three: near-duplicate columns are reported as ill-conditioned" ;;
esac
# NOT an exact comparison. This fit is the ill-conditioned one, and its trailing
# digits are the noise the section is about: Linux returns 2.00002262993 where
# macOS returns 2.00015811817, from the same source, because the two libms round
# differently and the design amplifies that. Asserting the digits would be
# asserting the opposite of what the example teaches. What must hold is that the
# answer is close to 2 and 3, and NOT close enough to be trusted at full width.
check "piece three lands near the truth but not on it" \
    "$("$bin" -t example/nearly-the-same.csv 2>/dev/null | tail -1 | awk -F, '
        { near = ($3 > 1.999 && $3 < 2.001 && $4 > 2.999 && $4 < 3.001)
          exact = ($3 > 1.999999 && $3 < 2.000001)
          print (near && !exact) ? "near but not exact" : "unexpected: " $3 " " $4 }')" \
    "near but not exact"

# --- residuals: where the model is wrong ------------------------------------
printf 'group,value,x\nA,26,-4\nA,19,-3\nA,14,-2\nA,11,-1\nA,10,0\nA,11,1\nA,14,2\nA,19,3\nA,26,4\n' \
    > "$tmp/curve.csv"
"$bin" -t "$tmp/curve.csv" --residuals "$tmp/r.csv" >/dev/null 2>&1
check "a residual file has a row per training row, plus a header" \
    "$(wc -l < "$tmp/r.csv" | tr -d ' ')" "10"
check "and names its columns" "$(head -1 "$tmp/r.csv")" "group,observed,predicted,residual"
# The residuals must be real numbers from the fitted line. They were once read
# from uninitialised xmalloc memory and came out around 1e161.
# Not an exact zero: all three columns are rounded to 12 significant digits, so
# the identity holds to about 1e-12 rather than to the bit.
check "the residuals are the observed minus the predicted" \
    "$(awk -F, 'NR==2{d=$2-$3-$4; if (d<0) d=-d; print (d < 1e-9) ? "ok" : "off by " d}' "$tmp/r.csv")" \
    "ok"
case "$(awk -F, 'NR==2{print ($4>0 && $4<100) ? "sane" : "WILD " $4}' "$tmp/r.csv")" in
    sane) ok ;; *) no "residuals are of a plausible size" ;;
esac
# --residuals needs -t, and covers every group
set +e
"$bin" --residuals "$tmp/x.csv" 001 icu_indicator=1 >/dev/null 2>&1; rc=$?
set -e
check "--residuals without -t is an error" "$rc" "1"

# --- groups: the same terms, different coefficients ------------------------
check "a per-group fit recovers each route's own line" \
    "$("$bin" -t example/routes.csv 2>/dev/null | tail -3 | tr '\n' ' ')" \
    "city,5,3,2 suburb,4,2,1.5 highway,8,1,5 "
check "pooling them gives one line that is none of the three" \
    "$("$bin" -t example/routes.csv -g '*' 2>/dev/null | tail -1)" \
    "*,5.66666666667,2,2.83333333333"
# and the pooled fit's error is visible where R2 is not alarming
case "$("$bin" -t example/routes.csv -g '*' 2>&1 >/dev/null)" in
    *"resid SD=7.693"*) ok ;; *) no "the pooled fit reports its residual SD" ;;
esac

# --- --qr: the same answer, without squaring the condition number ----------
check "--qr recovers the ill-conditioned design far more closely" \
    "$("$bin" -t example/nearly-the-same.csv --qr 2>/dev/null | tail -1 | awk -F, '
        { print ($3 > 1.9999999 && $3 < 2.0000001 && $4 > 2.9999999 && $4 < 3.0000001) \
                ? "within 1e-7" : "off: " $3 " " $4 }')" \
    "within 1e-7"
# and it does not warn, because at cond(X) rather than cond(X'X) it need not
check "--qr does not warn where the normal equations do" \
    "$("$bin" -t example/nearly-the-same.csv --qr 2>&1 >/dev/null | grep -c warning)" "0"
case "$("$bin" -t example/nearly-the-same.csv 2>&1 >/dev/null)" in
    *"Try --qr"*) ok ;; *) no "the ill-conditioned warning points at --qr" ;;
esac
# the summary must name the solver, since the two report cond= on different scales
case "$("$bin" -t example/routes.csv --qr 2>&1 >/dev/null)" in
    *"(QR)"*) ok ;; *) no "the summary names the solver" ;;
esac
# on a well-conditioned design the two solvers agree
check "both solvers agree where conditioning does not matter" \
    "$("$bin" -t example/routes.csv --qr 2>/dev/null | tail -3 | tr '\n' ' ')" \
    "$("$bin" -t example/routes.csv 2>/dev/null | tail -3 | tr '\n' ' ')"

# --- the residual check names the term whose shape is wrong -----------------
case "$("$bin" -t example/curve.csv --residuals "$tmp/c.csv" 2>&1 >/dev/null)" in
    *"correlate with x squared"*) ok ;;
    *) no "a parabola fitted with a line is reported" ;;
esac
# a correct model must not be warned about, or the warning means nothing
case "$("$bin" -t example/routes.csv --residuals "$tmp/rr.csv" 2>&1 >/dev/null)" in
    *warning*) no "routes.csv is linear and must not warn" ;;
    *) ok ;;
esac

echo "cliut: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
