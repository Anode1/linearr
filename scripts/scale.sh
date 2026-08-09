#!/bin/sh
# scale.sh: measure the claim instead of repeating it.
#
# The README says memory is a function of the model and not of the data. That is
# the kind of sentence every project writes, so this script tries to falsify it:
# it fits the same 200-term model over row counts an order of magnitude apart and
# prints peak RSS for each. If the numbers move with the rows, the claim is wrong
# and the README has to change.
#
#   sh scripts/scale.sh [TERMS] [GROUPS] [SMALL_ROWS] [BIG_ROWS]
#   sh scripts/scale.sh 200 500 10000 100000        # the defaults
#
# Generated data goes to a temp directory and is deleted on exit. Nothing here
# touches the repository.
set -e

TERMS=${1:-200}
GROUPS=${2:-500}
SMALL=${3:-10000}
BIG=${4:-100000}

root=$(cd "$(dirname "$0")/.." && pwd)
bin=$root/linearr
[ -x "$bin" ] || { echo "build first: make" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# GNU time reports peak RSS in KB with %M. Without it, report time only rather
# than printing a number we did not measure.
if /usr/bin/time -f %M true 2>/dev/null; then
    have_rss=1
else
    have_rss=0
    echo "note: /usr/bin/time not available; reporting wall time only"
fi

run() {                                    # run CMD..., echo "seconds rss_kb"
    if [ "$have_rss" = 1 ]; then
        /usr/bin/time -f "%e %M" "$@" 2>&1 >/dev/null | tail -1
    else
        s=$(date +%s)
        "$@" >/dev/null 2>&1
        echo "$(( $(date +%s) - s )) -"
    fi
}

gen_train() {                              # gen_train FILE ROWS
    awk -v terms="$TERMS" -v rows="$2" -v seed=7 'BEGIN {
        srand(seed)
        printf "group,value"
        for (j = 1; j <= terms; j++) printf ",term_%d", j
        printf "\n"
        for (j = 1; j <= terms; j++) b[j] = int(rand() * 2000) / 100
        for (i = 0; i < rows; i++) {
            y = 4.25; line = ""
            for (j = 1; j <= terms; j++) {
                x = (rand() < 0.3) ? 1 : 0
                y += b[j] * x
                line = line "," x
            }
            printf "G001,%.4f%s\n", y, line
        }
    }' > "$1"
}

gen_table() {                              # gen_table FILE  (GROUPS groups wide)
    awk -v terms="$TERMS" -v groups="$GROUPS" -v seed=11 'BEGIN {
        srand(seed)
        printf "group,intercept"
        for (j = 1; j <= terms; j++) printf ",term_%d", j
        printf "\n"
        for (g = 1; g <= groups; g++) {
            printf "G%04d,%.4f", g, 3 + rand() * 5
            for (j = 1; j <= terms; j++)
                printf ",%.4f", (rand() < 0.25) ? int(rand() * 1600) / 100 : 0
            printf "\n"
        }
    }' > "$1"
}

gen_cases() {                              # gen_cases FILE COUNT
    awk -v terms="$TERMS" -v groups="$GROUPS" -v n="$2" -v seed=13 'BEGIN {
        srand(seed)
        for (i = 0; i < n; i++) {
            printf "G%04d", int(rand() * groups) + 1
            for (j = 1; j <= terms; j++) printf ",%d", (rand() < 0.3) ? 1 : 0
            printf "\n"
        }
    }' > "$1"
}

echo "linearr scale check: $TERMS terms, $GROUPS groups"
echo

printf "generating training data (%s and %s rows) ... " "$SMALL" "$BIG"
gen_train "$tmp/small.csv" "$SMALL"
gen_train "$tmp/big.csv"   "$BIG"
echo "done ($(du -h "$tmp/small.csv" | cut -f1), $(du -h "$tmp/big.csv" | cut -f1))"

echo "FIT: the same $TERMS-term model, ${SMALL} rows then ${BIG}:"
printf "  %-12s %s\n" "rows" "seconds  peak RSS (KB)"
printf "  %-12s %s\n" "$SMALL"  "$(run "$bin" -t "$tmp/small.csv" -g G001)"
printf "  %-12s %s\n" "$BIG"    "$(run "$bin" -t "$tmp/big.csv"   -g G001)"
echo "  ^ RSS should be flat: 10x the data, the same memory."
echo

printf "generating a %s-group table and cases ... " "$GROUPS"
gen_table "$tmp/coef.csv"
gen_cases "$tmp/cases.csv" "$BIG"
printf 'coef.file = %s\ntrim.file =\n' "$tmp/coef.csv" > "$tmp/system.properties"
echo "done"

echo "SCORE: $BIG cases against $GROUPS groups:"
cd "$tmp"
printf "  %-12s %s\n" "cases" "seconds  peak RSS (KB)"
printf "  %-12s %s\n" "$BIG" "$(run sh -c "'$bin' < '$tmp/cases.csv' > /dev/null")"
echo
echo "The coefficient table is the only thing that grows with the problem:"
echo "  $GROUPS groups x ($TERMS + 1) doubles = about $(( GROUPS * (TERMS + 1) * 8 / 1024 )) KB, held once."
