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

# The model these tests score against, named every time. There is no implicit
# default any more: scoring used to search a system.properties file and then a
# table shipped beside the binary, so a bare run answered from a demo model the
# reader had never seen. Absolute, because several tests run from $tmp.
DEMO_COEF=$root/example/coefficients.csv
DEMO_TRIM=$root/example/trim_additions.csv
score() { "$bin" -c "$DEMO_COEF" --trim "$DEMO_TRIM" "$@"; }

cd "$root"

# an argument is scored. The status is captured from the program: written as
# check "$?" on the line after, it was the status of the CHECK before it, which
# is 0 whenever check itself runs, so the assertion held no matter what the
# program returned.
out=$(score "$CASE1"); rc=$?
check "argument"      "$out" "$EXPECT1"
check "argument exit" "$rc" "0"

# a pipe is a filter
check "pipe" "$(printf '%s\n%s\n' "$CASE1" "$CASE1" | score | wc -l | tr -d ' ')" "2"
check "redirect" "$(printf '%s\n' "$CASE1" > "$tmp/c.csv"; score < "$tmp/c.csv")" "$EXPECT1"

# Comments and blank lines are skipped, not scored and not complained about.
check "pipe skips comments" \
    "$(printf '# a note\n\n%s\n' "$CASE1" | score | wc -l | tr -d ' ')" "1"

# an empty pipe is an ordinary outcome, not an error
out=$(printf '' | score 2>&1); rc=$?
check "empty pipe output" "$out" ""
check "empty pipe exit"   "$rc"  "0"

# a bad row is reported, and the exit code says so
set +e
out=$(printf '001,1,2\n' | score 2>&1); rc=$?
set -e
check "bad row exit" "$rc" "1"
case "$out" in *"cannot score"*) ok ;; *) no "bad row message: got [$out]" ;; esac

# A bad row must not throw away the good ones around it.
check "bad row does not stop the batch" \
    "$(printf '%s\n001,1,2\n%s\n' "$CASE1" "$CASE1" 2>/dev/null | score 2>/dev/null | wc -l | tr -d ' ')" "2"

# -h
check "-h exit" "$("$bin" -h >/dev/null 2>&1; echo $?)" "0"
case "$("$bin" -h 2>&1)" in usage:*) ok ;; *) no "-h prints usage" ;; esac

# Every synopsis line -h prints must work exactly as printed. Three of the four
# omitted the mandatory -c and failed with "no coefficient table", so the page
# contradicted its own body, which says -c is required.
"$bin" -h 2>&1 | sed -n 's/^usage: *//p;s/^       //p' | grep -q . && :
for form in "-c $DEMO_COEF 001 icu_indicator=1" \
            "-c $DEMO_COEF --terms" \
            "-t $root/example/simple-train.csv"; do
    set +e
    (cd "$tmp" && "$bin" $form >/dev/null 2>&1); rc=$?
    set -e
    check "the synopsis form '$(echo "$form" | cut -c1-18)...' runs" "$rc" "0"
done
set +e
(cd "$tmp" && "$bin" -c "$DEMO_COEF" < "$root/example/cases.csv" >/dev/null 2>&1); rc=$?
set -e
check "the synopsis stream form runs" "$rc" "0"

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
check "named form" "$(score 001 Cardioversion=1 icu_indicator=1)" "$EXPECT1"
# The two ways of writing one case must agree, or having two is a liability.
check "named form agrees with the row form" \
    "$(score 001 Cardioversion=1 icu_indicator=1)" "$(score "$CASE1")"
check "named form is case-insensitive" \
    "$(score 001 CARDIOVERSION=1 icu_indicator=1)" "$EXPECT1"

# --terms: the answer to "what am I supposed to type?"
check "--terms exit" "$(score --terms >/dev/null 2>&1; echo $?)" "0"
case "$(score --terms)" in *"24 terms and 12 groups"*) ok ;;
    *) no "--terms reports the size of the model" ;; esac
case "$(score --terms)" in *icu_indicator*) ok ;;
    *) no "--terms lists the term names" ;; esac

# errors name the thing that was wrong
# The old message was "cannot score: <the whole case>" whatever went wrong,
# which sent the user to check data that was never the problem.
set +e
for probe in "001 nosuchterm=1|nosuchterm" \
             "999 icu_indicator=1|no group" \
             "001 icu_indicator=yes|not a finite number"; do
    args=${probe%|*}; want=${probe#*|}
    out=$(score $args 2>&1)
    case "$out" in *"$want"*) ok ;; *) no "error names '$want': got [$out]" ;; esac
done
set -e

# it runs from somewhere else, like an installed program
# This is the defect that made the tool usable only inside its own source tree.
check "runs from another directory" "$(cd "$tmp" && score 001 Cardioversion=1 icu_indicator=1)" "$EXPECT1"
# Coefficients are written at full precision, so compare the VALUES. The old
# %.4f made this a string match, and made any coefficient below 5e-5 a zero.
coefs() { tail -1 | cut -d, -f2- | tr ',' '\n' | awk '{printf "%.9f\n", $1}' | paste -sd' ' -; }
check "-t finds its example from another directory" \
    "$(cd "$tmp" && "$bin" -t example/simple-train.csv -g A 2>/dev/null | coefs)" \
    "5.000000000 2.500000000 1.500000000"

# a missing table is one fatal message, not one complaint per row
set +e
out=$(cd "$tmp" && printf '%s\n%s\n%s\n' "$CASE1" "$CASE1" "$CASE1" \
      | "$bin" -c definitely-not-here.csv 2>&1); rc=$?
set -e
check "missing table exits nonzero" "$rc" "1"
check "missing table is reported once, not per row" \
    "$(printf '%s' "$out" | grep -c 'definitely-not-here')" "1"
case "$out" in *"looked in"*) ok ;; *) no "missing table says where it looked: got [$out]" ;; esac

# NO table at all is its own message, and it says what to do. There used to be a
# search here -- a system.properties file, then a table shipped beside the
# binary -- so a bare run scored against a demo model the reader had never seen,
# and the same command in two directories could answer with two different
# models without saying which.
set +e
out=$(cd "$tmp" && "$bin" 'A,10,3' 2>&1); rc=$?   # deliberately NO -c
set -e
check "no -c at all exits nonzero" "$rc" "1"
case "$out" in
    *"-c FILE"*) ok ;;
    *) no "no -c names the option: got [$out]" ;;
esac
case "$out" in
    *"-t TRAIN.CSV"*) ok ;;
    *) no "no -c says how to make one: got [$out]" ;;
esac

# fit, then score against what was fitted
"$bin" -t example/simple-train.csv -g A 2>/dev/null > "$tmp/coef.csv"
check "fit writes a header" "$(head -1 "$tmp/coef.csv")" "group,intercept,km,stops"
check "fit writes the row"  "$(coefs < "$tmp/coef.csv")" "5.000000000 2.500000000 1.500000000"

# THE ROUND TRIP, which %.4f used to break silently: a coefficient below 5e-5
# was written as 0.0000, so a fit reporting R2=1.0000 published a constant model.
printf 'group,value,bytes\nA,3.0,0\nA,3.15,100000\nA,3.30,200000\nA,3.45,300000\n' > "$tmp/tiny.csv"
"$bin" -t "$tmp/tiny.csv" -g A 2>/dev/null > "$tmp/tiny_coef.csv"
check "a tiny coefficient survives the round trip" \
    "$(cd "$tmp" && "$bin" -c tiny_coef.csv A bytes=1000000)" "A prediction=4.5000"

# The fit summary goes to stderr, so stdout stays a clean coefficient file.
case "$("$bin" -t example/simple-train.csv -g A 2>&1 >/dev/null)" in
    *"R2="*) ok ;; *) no "fit summary on stderr" ;;
esac

check "score against the fitted table" \
    "$(cd "$tmp" && "$bin" -c coef.csv 'A,10,3')" "A prediction=34.5000"

# an unreadable table named with -c is an error, not a shrug
set +e
(cd "$tmp" && "$bin" -c nope.csv 'A,10,3' >/dev/null 2>&1); rc=$?
set -e
check "missing coefficient file exits nonzero" "$rc" "1"

# --scale and --trim-scale, which were predict.scale and trim.scale in the
# properties file. Out of range is refused rather than quietly defaulted.
check "--scale sets the decimals" \
    "$(cd "$tmp" && "$bin" -c coef.csv --scale 1 'A,10,3')" "A prediction=34.5"
check "--scale 0 is whole numbers" \
    "$(cd "$tmp" && "$bin" -c coef.csv --scale 0 'A,10,3')" "A prediction=35"
set +e
(cd "$tmp" && "$bin" -c coef.csv --scale 12 'A,10,3' >/dev/null 2>&1); rc=$?
set -e
check "--scale outside 0..9 is refused" "$rc" "1"

# -y and --qr reach the fitter only: scoring takes its schema and its
# coefficients from the table named by -c, so both had nothing to act on and
# were accepted and dropped. The three refusals beside them in main.c exist to
# prevent exactly that silence; these two were added later and missed it.
for opt in "-y minutes" "--qr"; do
    set +e
    (cd "$tmp" && "$bin" -c coef.csv $opt 'A,10,3' >/dev/null 2>&1); rc=$?
    set -e
    check "$opt without -t is refused, not ignored" "$rc" "1"
done
set +e
("$bin" -t "$root/example/simple-train.csv" --qr >/dev/null 2>&1); rc=$?
set -e
check "--qr with -t is still accepted" "$rc" "0"

# Out of range was the only form caught, because the value was read with atoi,
# which cannot tell "abc" from 0. Every one of these published a prediction
# rounded to a scale nobody asked for and exited 0: "abc" and "" gave 0
# decimals, "3.9" gave 3, and "4294967300" wrapped to 4. Rounding is part of
# the answer here, so a scale the caller did not ask for is a wrong number.
for bad in abc 3.9 4294967300 '' -1 ' '; do
    set +e
    (cd "$tmp" && "$bin" -c coef.csv --scale "$bad" 'A,10,3' >/dev/null 2>&1); rc=$?
    set -e
    check "--scale '$bad' is refused, not silently rounded" "$rc" "1"
    set +e
    (cd "$tmp" && "$bin" -c coef.csv --trim-scale "$bad" 'A,10,3' >/dev/null 2>&1); rc=$?
    set -e
    check "--trim-scale '$bad' is refused, not silently rounded" "$rc" "1"
done

# the build's own claims
# Twice now this Makefile has said it did something and not done it: the default
# goal was `modeclean`, so a bare `make` deleted the build and exited 0; and
# CPPFLAGS was recorded nowhere, so the README's own command for setting the
# term ceiling printed "Nothing to be done" and left the previous binary in
# place. Both were silent successes. They are assertions now.
if command -v make >/dev/null 2>&1; then
    src=$tmp/src; mkdir -p "$src"
    mkdir -p "$src/c"; cp "$root"/c/*.c "$root"/c/*.h "$src/c"/
    cp "$root"/Makefile "$src"/
    cp -r "$root/example" "$src"/ 2>/dev/null || true
    mkdir -p "$src/tests"; cp "$root/tests/cli.sh" "$src/tests/" 2>/dev/null || true

    (cd "$src" && env -u MAKEFLAGS -u MAKELEVEL sh -c 'make clean >/dev/null 2>&1; make >/dev/null 2>&1')
    if [ -x "$src/linearr" ]; then ok; else no "make (default goal) builds the binary"; fi

    # A changed ceiling must reach the objects. A 32-term build has to refuse a
    # 100-term table; if CPPFLAGS is ignored, the stale 256-term binary accepts
    # it and this passes for the wrong reason.
    awk 'BEGIN{ printf "group,intercept"; for(i=1;i<=100;i++) printf ",t%d", i; printf "\n";
                printf "G"; for(i=0;i<=100;i++) printf ",1"; printf "\n" }' > "$src/c100.csv"
    (cd "$src" && ./linearr -c c100.csv --terms >/dev/null 2>&1) \
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
    # The probe goes into c/, where the sources are. It used to be written to
    # $src/utils.c, and when the sources moved it landed in a file nothing
    # compiles: grep then matched nothing, exited 1, and `set -e` ended this
    # whole script with no output and no FAIL line. `|| true` on the count, so
    # a probe that stops working fails ITS assertion instead of the run.
    printf '\nstatic int cli_probe(void) { int x; return x; }\n' >> "$src/c/utils.c"
    n=$(cd "$src" && env -u MAKEFLAGS -u MAKELEVEL \
        make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32' 2>&1 | grep -c warning || true)
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
    out=$(score < "$tmp/split.txt" 2>/dev/null); rc=$?
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
out=$(score 001 icu_indicator=nan 2>&1); rc=$?
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
score -g 001 </dev/null >/dev/null 2>&1; rc=$?
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

# a rounding that cannot be honoured is an error, not a silent default. The
# properties file used to accept predict.scale = 99 and quietly give four
# decimals, so the configuration said one thing and the program did another.
printf 'group,intercept,a\nX,10,1\n' > "$tmp/dup2.csv"
set +e
(cd "$tmp" && "$bin" -c dup2.csv --scale 99 X a=1 >/dev/null 2>&1); rc=$?
set -e
check "an out-of-range --scale is refused" "$rc" "1"

# the two case forms must agree about whitespace
check "the named form tolerates a trailing space, as the row form does" \
    "$(score 001 'icu_indicator=1 ' 2>&1)" "$(score '001,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0 ' 2>&1)"

# --- the trim table: named, or not there ------------------------------------
# There used to be three ways to have no trim table and they did not agree. A
# properties file said "comment this out and the trim point is just the
# prediction", and that was false: an absent key meant the BUILT-IN default, so
# the table loaded and the trim was applied. With the properties file gone there
# are two states and they are the two a reader would guess.
mkdir -p "$tmp/tf"
printf 'group,intercept,a\nX,10,1\n'  > "$tmp/tf/c.csv"
printf 'GROUP,trim_addition\nX,500\n' > "$tmp/tf/t.csv"

check "--trim names the table, and it is applied" \
    "$(cd "$tmp/tf" && "$bin" -c c.csv --trim t.csv X a=0)" "X prediction=10.0000 trim=510.0"
check "no --trim means the trim point is the prediction" \
    "$(cd "$tmp/tf" && "$bin" -c c.csv X a=0)" "X prediction=10.0000"
check "--no-trim says the same thing explicitly" \
    "$(cd "$tmp/tf" && "$bin" -c c.csv --no-trim X a=0)" "X prediction=10.0000"
set +e
(cd "$tmp/tf" && "$bin" -c c.csv --trim nope.csv X a=0 >/dev/null 2>&1); rc=$?
set -e
check "a --trim table that is not there is an error" "$rc" "1"

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
# --residuals needs -t
set +e
score --residuals "$tmp/x.csv" 001 icu_indicator=1 >/dev/null 2>&1; rc=$?
set -e
check "--residuals without -t is an error" "$rc" "1"

# --residuals WITH -g. This used to be refused, on the grounds that the residual
# pass covered every group; the real reason was that the single-group path had
# never been wired for a second pass. One group's residuals are exactly what is
# wanted once the summary has named the group that is wrong.
"$bin" -t example/anscombe.csv -g II --residuals "$tmp/a2.csv" >/dev/null 2>&1
check "-g with --residuals writes only that group" \
    "$(awk -F, 'NR>1 && $1!="II"' "$tmp/a2.csv" | wc -l | tr -d ' ')" "0"
check "-g with --residuals writes all of it" \
    "$(awk 'END{print NR-1}' "$tmp/a2.csv")" "11"
set +e
"$bin" -t example/anscombe.csv -g nosuch --residuals "$tmp/a3.csv" >/dev/null 2>&1; rc=$?
set -e
check "-g naming no group is an error" "$rc" "1"

# --- the certified sets ----------------------------------------------------
# example/longley.csv and example/wampler1.csv carry answers somebody else
# computed to fifteen digits. tests.c checks the solvers against the same
# numbers compiled in; these check that the FILES still hold the data those
# numbers belong to, which is the part a unit test cannot see.
check "Longley: the certified coefficients, to eleven digits" \
    "$("$bin" -t example/longley.csv 2>/dev/null | tail -1)" \
    "A,-3482258.6346,15.0618722714,-0.0358191792926,-2.02022980382,-1.03322686717,-0.0511041056535,1829.15146461"
check "Longley: and the certified residual SD" \
    "$("$bin" -t example/longley.csv 2>&1 >/dev/null | sed -n 's/.*resid SD=\([0-9.]*\).*/\1/p')" \
    "304.9"
check "Wampler1: QR recovers the exact quintic" \
    "$("$bin" -t example/wampler1.csv --qr 2>/dev/null | tail -1 | cut -d, -f6,7)" \
    "1,1"
# The certified residual is zero: the normal equations report something visible
# and QR reports nothing. Magnitudes, not digits -- how far each lands depends
# on fused multiply-add and summation order, so arm64 with clang gives 0.02304
# and 9.209e-11 where x86-64 with gcc gives 0.02282 and 6.663e-11, and
# comparing the printed strings failed a correct macOS build.
ne=$("$bin" -t example/wampler1.csv 2>&1 >/dev/null | sed -n 's/.*resid SD=\([0-9.e-]*\).*/\1/p')
qr=$("$bin" -t example/wampler1.csv --qr 2>&1 >/dev/null | sed -n 's/.*resid SD=\([0-9.e-]*\).*/\1/p')
check "Wampler1: the normal equations report a residual that is not there" \
    "$(awk -v v="$ne" 'BEGIN{print (v > 1e-3 && v < 1) ? "visible" : "got " v}')" "visible"
check "Wampler1: QR does not" \
    "$(awk -v v="$qr" 'BEGIN{print (v < 1e-8) ? "negligible" : "got " v}')" "negligible"
check "Wampler1: and QR is better by orders of magnitude" \
    "$(awk -v a="$ne" -v b="$qr" 'BEGIN{print (b < a/1e6) ? "yes" : "no"}')" "yes"

# Each group's residual checks are judged against ITS OWN spread. They used to
# be judged against the worst residual SD of any group in the file and the
# spread of every row together, so a group that fits to rounding error, sitting
# beside a noisy one, had its rounding error correlated with things. The file
# below did produce a spread warning that way, on data that has no such thing.
{
    echo "group,y,x"
    awk 'BEGIN{for(i=0;i<40;i++){x=i*0.7; printf "exact,%.17g,%.17g\n", 0.1+0.3*x, x}}'
    awk 'BEGIN{srand(3);for(i=0;i<40;i++) printf "noisy,%.17g,%d\n", 5+(rand()-0.5)*4000, i}'
} > "$tmp/mixed.csv"
check "an exactly fitting group is not judged by another group's error" \
    "$("$bin" -t "$tmp/mixed.csv" --residuals /dev/null 2>&1 >/dev/null | grep -c warning)" \
    "0"

# Written by -t from the training header, and read back. Without it a
# coefficient file says group,intercept,km,stops and nothing in it says the
# answer is in minutes. los_response_name() had no caller at all.
"$bin" -t example/simple-train.csv > "$tmp/m.csv" 2>/dev/null
check "the coefficient file records what it predicts" \
    "$(head -1 "$tmp/m.csv")" "# response: minutes"
check "and reading it back recovers the name" \
    "$("$bin" -c "$tmp/m.csv" --terms | sed -n 's/^it predicts: //p')" "minutes"
check "a coefficient file without the line still loads" \
    "$(grep -v '^# response' "$tmp/m.csv" > "$tmp/m2.csv"; "$bin" -c "$tmp/m2.csv" --terms | grep -c 'it predicts')" \
    "0"

# --- what it refuses to read, and what it says ------------------------------
# Every one of these used to produce the same sentence, "expected a group, a
# value, and N terms", whatever was actually wrong: an empty field, a short row,
# a category name, a quoted number and a semicolon-separated row were
# indistinguishable. On a file of ten million rows that is not a diagnosis, and
# a reviewer read it as the program crashing. It does not crash. It refuses,
# with a status of 1, and it now says which column and why.
says() {   # says LABEL FILE-CONTENT PATTERN
    printf '%b' "$2" > "$tmp/bad.csv"
    set +e
    msg=$("$bin" -t "$tmp/bad.csv" 2>&1 >/dev/null); rc=$?
    set -e
    if [ "$rc" != "1" ]; then no "$1: expected exit 1, got $rc"; return; fi
    case "$msg" in *"$3"*) ok ;; *) no "$1: expected [$3], got [$msg]" ;; esac
}
H='group,y,a,b\n'
says "an empty field names the column"        "${H}A,1,1,2\nA,2,,3\nA,3,2,4\n"      "term a is empty"
says "an empty field says there is no imputation" "${H}A,1,1,2\nA,2,,3\nA,3,2,4\n" "no imputation"
says "a category name is named"               "${H}A,1,red,2\nA,2,blue,3\n"         "term a is 'red'"
says "and says there is no encoding for it"   "${H}A,1,red,2\nA,2,blue,3\n"         "no encoding for a category name"
says "NA is not a number"                     "${H}A,1,NA,2\nA,2,1,3\n"             "term a is 'NA'"
says "a short row counts the fields"          "${H}A,1,1,2\nA,2,3\n"                "it has 3 fields and the header names 4"
says "a quoted group name is refused"         "${H}\"A\",1,1,2\n"                   "the group column is quoted"
says "a quoted number is refused"             "${H}A,\"1\",1,2\n"                   "is quoted"
says "a quoted comma is explained"            "${H}A,\"1,5\",1,2\n"                 "the row contains a quote"
says "a semicolon row is named as such"       "${H}A;1;1;2\n"                       "does have semicolons"
says "a non-numeric response is named"        "${H}A,high,1,2\nA,low,2,3\n"         "y is 'high'"

# A quoted group name used to be ACCEPTED, with the quotes kept, so "A" and A
# became two groups and nothing said so. That is worse than a refusal: a silent
# wrong answer.
printf 'group,y,a\nA,1,1\n"A",2,2\nA,3,3\n' > "$tmp/q.csv"
set +e
"$bin" -t "$tmp/q.csv" >/dev/null 2>&1; rc=$?
set -e
check "a quoted name does not become a second group" "$rc" "1"

# A semicolon HEADER was already named; the rows were not.
printf 'group;y;a;b\nA;1;1;2\n' > "$tmp/s.csv"
set +e
msg=$("$bin" -t "$tmp/s.csv" 2>&1 >/dev/null); rc=$?
set -e
case "$msg" in *"semicolon"*) ok ;; *) no "a semicolon header is named: got [$msg]" ;; esac

# The layout it used, said out loud. Nothing in the data can say which column is
# the response, so a file written in another order fits perfectly well and
# answers a different question. The only defence is to state what was taken.
check "the layout it read is reported" \
    "$("$bin" -t example/simple-train.csv 2>&1 >/dev/null | head -1)" \
    "reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column"

# CRLF is not a refusal: a file from a Windows editor reads normally.
printf 'group,y,a\r\nA,1,1\r\nA,2,2\r\nA,3,4\r\n' > "$tmp/crlf.csv"
check "CRLF line endings are read, not refused" \
    "$("$bin" -t "$tmp/crlf.csv" >/dev/null 2>&1; echo $?)" "0"

# --- naming the response ------------------------------------------------------
# The layout is positional, so a file written in another order fits perfectly
# well and answers a different question. A reviewer wrote the CSV a pandas user
# exports, site,dose,age,response, and got dose regressed on age and response:
# plausible coefficients, exit 0, nothing wrong on the face of it. -y names the
# column instead of counting to two.
printf 'site,dose,age,response\nn,10,55,3.1\nn,20,61,5.4\nn,30,48,7.9\nn,40,52,10.2\n' \
    > "$tmp/y.csv"
check "without -y the second column is the value, as before" \
    "$("$bin" -t "$tmp/y.csv" 2>&1 >/dev/null | head -1)" \
    "reading: column 1 is the group, 'dose' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column"
check "-y names it instead, and drops the advice it no longer needs" \
    "$("$bin" -t "$tmp/y.csv" -y response 2>&1 >/dev/null | head -1)" \
    "reading: column 1 is the group, 'response' is the value being predicted, and the other 2 columns are terms"
check "and the terms are every other column, in order" \
    "$("$bin" -t "$tmp/y.csv" -y response 2>/dev/null | grep '^group,')" \
    "group,intercept,dose,age"
check "--response is the long form" \
    "$("$bin" -t "$tmp/y.csv" --response response 2>/dev/null | grep '^group,')" \
    "group,intercept,dose,age"
check "the coefficient file records the named response" \
    "$("$bin" -t "$tmp/y.csv" -y response 2>/dev/null | head -1)" \
    "# response: response"
# The fit must be the real one, not a renaming: response = 0.235*dose - 0.011*age
check "and the coefficients are of that fit" \
    "$("$bin" -t "$tmp/y.csv" -y response 2>/dev/null | tail -1 | cut -d, -f3 | cut -c1-6)" \
    "0.2355"
set +e
out=$("$bin" -t "$tmp/y.csv" -y outcome 2>&1 >/dev/null); rc=$?
set -e
check "a name that is not in the header is an error" "$rc" "1"
case "$out" in *"has no column 'outcome'"*) ok ;; *) no "-y names the missing column: got [$out]" ;; esac
case "$out" in *"dose, age, response"*) ok ;; *) no "-y lists what the header does offer: got [$out]" ;; esac
set +e
out=$("$bin" -t "$tmp/y.csv" -y site 2>&1 >/dev/null); rc=$?
set -e
check "naming the group column is an error" "$rc" "1"
case "$out" in *"the first column is the group"*) ok ;; *) no "-y group message: got [$out]" ;; esac
# A model fitted with -y must score like any other.
"$bin" -t "$tmp/y.csv" -y response 2>/dev/null > "$tmp/ym.csv"
check "a model fitted with -y scores normally" \
    "$("$bin" -c "$tmp/ym.csv" n dose=25 age=50)" \
    "n prediction=6.6938"

# --- per-group statistics ---------------------------------------------------
# The summary reports the worst of each figure over the whole file, which for
# many groups says nothing about WHICH group. --stats is the table.
"$bin" -t example/routes.csv --stats "$tmp/st.csv" >/dev/null 2>&1
check "--stats writes a header" "$(head -1 "$tmp/st.csv")" \
    "group,rows,df,r2,resid_sd,cond,pinned"
check "--stats writes one row per group" "$(awk 'END{print NR-1}' "$tmp/st.csv")" "3"
check "--stats names the groups" "$(cut -d, -f1 "$tmp/st.csv" | tail -3 | tr '\n' ' ')" \
    "city suburb highway "
set +e
"$bin" --stats "$tmp/x.csv" 001 >/dev/null 2>&1; rc=$?
set -e
check "--stats without -t is an error" "$rc" "1"
# "-" is stdout, as it is stdin for -t. It used to open a FILE named "-", and
# one of those was committed to the repository.
( cd "$tmp" && rm -f -- - && "$bin" -t "$root/example/routes.csv" --stats - >/dev/null 2>&1 )
check "--stats - does not create a file called -" \
    "$( [ -e "$tmp/-" ] && echo present || echo absent )" "absent"

# Anscombe II is the canonical curve that a line cannot fit. The check must see
# it, and must NOT see anything in set I, which is the same summary statistics
# over data that is genuinely straight.
check "Anscombe II: the curvature check fires" \
    "$("$bin" -t example/anscombe.csv -g II --residuals /dev/null 2>&1 >/dev/null | grep -c 'wrong shape')" \
    "1"
check "Anscombe I: and stays quiet on the straight set" \
    "$("$bin" -t example/anscombe.csv -g I --residuals /dev/null 2>&1 >/dev/null | grep -c 'warning')" \
    "0"

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
    *"residuals still depend on x"*) ok ;;
    *) no "a parabola fitted with a line is reported" ;;
esac
# a correct model must not be warned about, or the warning means nothing
case "$("$bin" -t example/routes.csv --residuals "$tmp/rr.csv" 2>&1 >/dev/null)" in
    *warning*) no "routes.csv is linear and must not warn" ;;
    *) ok ;;
esac

# --- training from a pipe, which is what a cloud invocation looks like ------
check "-t - fits from stdin" \
    "$(cat example/routes.csv | "$bin" -t - 2>/dev/null | tail -1)" "highway,8,1,5"
# --residuals needs a second pass, and a pipe cannot be rewound. It must refuse
# BEFORE writing a table, not after.
set +e
out=$(cat example/routes.csv | "$bin" -t - --residuals "$tmp/p.csv" 2>/dev/null); rc=$?
set -e
check "--residuals from a pipe writes no table" "$out" ""
check "and exits nonzero" "$rc" "1"

# --- Anscombe's quartet: the same line four times ---------------------------
check "the quartet gives four near-identical lines" \
    "$("$bin" -t example/anscombe.csv 2>/dev/null | tail -4 | cut -d, -f2,3 |
       while IFS=, read -r a b; do printf '%.2f/%.2f ' "$a" "$b"; done)" \
    "3.00/0.50 3.00/0.50 3.00/0.50 3.00/0.50 "
# and only set II is reported: III and IV are influence, not shape, and this
# program has no measure of influence
out=$("$bin" -t example/anscombe.csv --residuals "$tmp/a.csv" 2>&1 >/dev/null)
case "$out" in *"group II"*) ok ;; *) no "the quartet's parabola is named" ;; esac
check "and no other set is reported" "$(printf '%s' "$out" | grep -c warning)" "1"

echo "cliut: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
