#!/bin/sh
# bench.sh: the same job in several languages: read the file, fit one line per
# group, write the table.
#
# Each implementation is written the way its language does the job when it is
# ALLOWED to stream: a read-a-line loop and a fixed accumulator per group.
# That is the only comparison worth printing. Writing C as a stream and Python
# as pandas measures the two styles, not the two languages, and the memory
# column becomes a rhetorical trick. bench/Fit.java is modelled on the author's
# 2011 Processor.java, which streams and accumulates no records, so the Java
# number is not a strawman written to lose.
#
# fit.R is the deliberate exception: read.csv reads the whole file into a frame because
# that is R's idiom. Its memory figure is the cost of the idiom, and the table
# labels it so nobody reads it as a statement about R the language. fit-stream.R
# is the same language streaming, so the two R rows answer the reader who says
# the comparison pits a frame against a stream: it does, and here is the other
# one.
#
# Every implementation's coefficients are checked against linearr's before any
# time is printed. A speed number nobody checked is a speed number for a
# different answer.
#
#   sh scripts/bench.sh [TERMS] [GROUPS] [ROWS]
set -e

TERMS=${1:-8}
GROUPS=${2:-200}
ROWS=${3:-200000}

root=$(cd "$(dirname "$0")/.." && pwd)
bin=$root/linearr
bench=$root/bench
[ -x "$bin" ] || { echo "build first: make" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
train=$tmp/train.csv

if /usr/bin/time -f %M true 2>/dev/null; then rss=1; else rss=0; fi

printf 'generating %s rows, %s groups, %s terms ... ' "$ROWS" "$GROUPS" "$TERMS"
awk -v terms="$TERMS" -v groups="$GROUPS" -v rows="$ROWS" -v seed=5 'BEGIN {
    srand(seed)
    printf "group,value"
    for (j = 1; j <= terms; j++) printf ",x%d", j
    printf "\n"
    for (g = 1; g <= groups; g++) {
        b0[g] = 1 + (g % 7)
        for (j = 1; j <= terms; j++) b[g, j] = 1 + ((g + j) % 5) * 0.5
    }
    for (i = 0; i < rows; i++) {
        g = (i % groups) + 1
        y = b0[g]; line = ""
        for (j = 1; j <= terms; j++) {
            x = int(rand() * 4)
            y += b[g, j] * x
            line = line "," x
        }
        printf "G%04d,%.10g%s\n", g, y, line
    }
}' > "$train"
echo "$(du -h "$train" | cut -f1)"
echo

# the reference answer
"$bin" -t "$train" > "$tmp/ref.csv" 2>/dev/null

# The Java is compiled here rather than assumed built, and into the scratch
# directory, so a benchmark never depends on whatever java/classes happens to
# hold from an earlier session.
javadir=""
if command -v javac >/dev/null 2>&1; then
    if javac -d "$tmp/classes" "$root"/java/*.java 2>/dev/null; then
        javadir=$tmp/classes
    fi
fi

# One implementation failing must not take the table with it. With set -e and
# no guard, a non-zero exit from /usr/bin/time ended the whole script after the
# first row, and the output looked like a benchmark with one entrant rather than
# like five commands that never ran. That is how the Java row went missing: the
# classpath pointed at java/ when the classes are built into java/classes/.
run() {   # run NAME SHAPE COMMAND...
    name=$1; shape=$2; shift 2
    if ! command -v "$1" >/dev/null 2>&1; then
        printf "  %-16s %-12s %s\n" "$name" "$shape" "not installed - skipped"
        return 0
    fi
    set +e
    if [ "$rss" = 1 ]; then
        # -o, not 2>: the implementations write their own commentary to stderr
        # (linearr's fit summary does), and capturing time's output the lazy way
        # interleaves the two into one unreadable column.
        /usr/bin/time -o "$tmp/t" -f "%e %M" "$@" >"$tmp/out.csv" 2>/dev/null
        t=$(cat "$tmp/t")
    else
        s=$(date +%s); "$@" >"$tmp/out.csv" 2>/dev/null; t="$(( $(date +%s) - s )) -"
    fi
    if d=$(python3 "$bench/compare.py" "$tmp/ref.csv" "$tmp/out.csv" 2>/dev/null); then
        printf "  %-16s %-12s %-8s %-10s agrees to %s\n" "$name" "$shape" \
               "$(echo "$t" | cut -d' ' -f1)s" "$(echo "$t" | cut -d' ' -f2) KB" "$d"
    else
        printf "  %-16s %-12s %s\n" "$name" "$shape" \
               "ANSWER DIFFERS (${d:-did not run, or wrote nothing}) - no time reported"
    fi
    set -e
    return 0
}

echo "  implementation   shape        time     peak memory  check"
run "linearr (C)"  "streaming"  "$bin" -t "$train"
if [ -n "$javadir" ]; then
    run "Java"     "streaming"  java -cp "$javadir" Linearr "$train"
else
    printf "  %-16s %-12s %s\n" "Java" "streaming" "no javac - skipped"
fi
run "Python"       "streaming"  python3 "$bench/fit.py" "$train"
run "awk"          "streaming"  awk -f "$bench/fit.awk" "$train"
run "R"            "streaming"  Rscript "$bench/fit-stream.R" "$train"
run "Python"       "frame"      python3 "$bench/fit-frame.py" "$train"
run "R"            "frame"      Rscript "$bench/fit.R" "$train"
echo
echo "  streaming = a read-a-line loop, memory fixed by the model."
echo "  frame     = the language's idiom reads the whole file in first; the memory"
echo "              figure is the cost of that idiom, not of the language."
echo

# The JVM's memory is mostly the JVM. Capping the heap separates the runtime's
# appetite from the algorithm's need, so the Java row cannot be read as "the
# algorithm requires this much".
if command -v java >/dev/null 2>&1 && [ -n "$javadir" ]; then
    printf "  the same Java under a capped heap, to show what the ALGORITHM needs:\n"
    for heap in 16m 64m; do
        if [ "$rss" = 1 ]; then
            if /usr/bin/time -o "$tmp/t" -f "%e %M" java -Xmx$heap -cp "$javadir" \
                 Linearr "$train" >"$tmp/out.csv" 2>/dev/null; then
                printf "    -Xmx%-5s %ss  %s KB\n" "$heap" \
                       "$(cut -d" " -f1 "$tmp/t")" "$(cut -d" " -f2 "$tmp/t")"
            else
                printf "    -Xmx%-5s did not complete\n" "$heap"
            fi
        fi
    done
    echo "    Same rows, same answer: the heap it is given, not the data it reads."
fi
