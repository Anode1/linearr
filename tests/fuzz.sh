#!/bin/sh
# fuzz.sh: feed the program hostile input under a sanitizer and fail on any
# crash or sanitizer report.
#
#   sh tests/fuzz.sh [ROUNDS] [BINARY]     (default 5000, ./linearr_ut_asan)
#
# What it is for: every wrong-number defect this project has had arrived through
# the reader, on input nobody wrote a test for. The unit tests use well-formed
# files by construction, so they cannot reach a NUL in the middle of a field, a
# row of 4000 commas, or a number with 900 digits of exponent. This does.
#
# An exit code of 0 or 1 is a PASS: the program either answered or refused, and
# both are correct behaviour. Anything else -- a signal, an ASan report, a UBSan
# abort -- fails the round and the offending input is kept in the temp directory,
# whose path is printed. The input is generated from a seeded PRNG, so a failing
# round reproduces exactly: pass the printed seed back as the third argument.
set -e

ROUNDS=${1:-5000}
BIN=${2:-./linearr_ut_asan}
SEED=${3:-1}

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# The binary under test is a sanitizer build by default, because a plain build
# passes silently on exactly the reads this exists to find.
if [ ! -x "$BIN" ]; then
    echo "fuzz: $BIN not built -- run 'make ut-asan' first (or pass a binary)" >&2
    exit 77
fi

tmp=$(mktemp -d)
keep=0
trap '[ "$keep" = 1 ] || rm -rf "$tmp"' EXIT

# Random bytes, biased toward the things a CSV reader has to survive: commas,
# newlines, NULs, quotes, minus signs, digits, and the letters of "nan"/"inf".
gen() {
    awk -v seed="$1" -v n="$2" 'BEGIN {
        srand(seed)
        split(",\n\r\"\047-+.eE0123456789naif#, \t\\", ch, "")
        nch = length(",\n\r\"\047-+.eE0123456789naif#, \t\\")
        for (i = 0; i < n; i++) {
            r = rand()
            if (r < 0.02)      printf "%s", "\000"
            else if (r < 0.05) printf "%s", "1e" int(rand() * 4000)
            else if (r < 0.08) { for (j = 0; j < int(rand() * 900); j++) printf "9" }
            else if (r < 0.12) printf "\n"
            else               printf "%s", ch[int(rand() * nch) + 1]
        }
        printf "\n"
    }'
}

fail=0
round=0
while [ "$round" -lt "$ROUNDS" ]; do
    round=$((round + 1))
    s=$((SEED + round))
    f=$tmp/in.csv
    # Half the rounds are pure noise, which the reader refuses early; the other
    # half start from a VALID header so the corruption lands in the ROWS and the
    # parser, the accumulators and the solver are reached at all. Noise alone
    # never gets past "needs a header", and the defects this project has had
    # were all past that point.
    if [ $((s % 2)) -eq 0 ]; then
        gen "$s" $((20 + s % 400)) > "$f" 2>/dev/null
    else
        printf 'group,y,x1,x2\n' > "$f"
        gen "$s" $((20 + s % 400)) >> "$f" 2>/dev/null
    fi

    # Both readers: the training path and the scoring path (stdin), because they
    # are different code and only one of them is exercised by a file argument.
    for mode in train score; do
        set +e
        if [ "$mode" = train ]; then
            "$BIN" -t "$f" > "$tmp/out" 2>/dev/null
        else
            "$BIN" -c "$f" A x=1 > "$tmp/out" 2>/dev/null
        fi
        rc=$?
        set -e
        # An ORACLE, not just a crash check. Crashes are the easy half; this
        # project's defects have all been a wrong number printed with exit 0,
        # which no sanitizer can see. What must never appear on stdout: nan or
        # inf. Both mean the arithmetic gave up and the program published the
        # wreckage instead of refusing.
        if [ "$rc" = 0 ] && grep -qiE '(^|[,= ])[-+]?(nan|inf)' "$tmp/out"; then
            echo "fuzz: FAIL round $round ($mode) printed nan/inf with exit 0, seed $s" >&2
            cp "$f" "$tmp/fail-$round-$mode-nan.csv"
            keep=1; fail=$((fail + 1))
        fi
        # 0 answered, 1 refused, 2 usage: all are decisions. 77 is a skip.
        case "$rc" in
            0|1|2) ;;
            *) echo "fuzz: FAIL round $round ($mode) exit $rc, seed $s" >&2
               cp "$f" "$tmp/fail-$round-$mode.csv"
               keep=1; fail=$((fail + 1)) ;;
        esac
    done

    # The same bytes through stdin, which has its own reader.
    set +e
    "$BIN" -c example/coefficients.csv < "$f" >/dev/null 2>&1
    rc=$?
    set -e
    case "$rc" in
        0|1|2) ;;
        *) echo "fuzz: FAIL round $round (stdin) exit $rc, seed $s" >&2
           cp "$f" "$tmp/fail-$round-stdin.csv"
           keep=1; fail=$((fail + 1)) ;;
    esac
done

if [ "$fail" -gt 0 ]; then
    echo "fuzz: $fail failing rounds; inputs kept in $tmp" >&2
    exit 1
fi
echo "fuzz: $ROUNDS rounds, 3 readers each, no crash and no sanitizer report ($BIN)"
