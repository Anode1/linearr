#!/bin/sh
# java-check.sh: fit every example with both implementations and diff.
#
# java/ is meant to be read beside the C, line for line. Nothing checked that
# it still BEHAVED like the C, and it had stopped: the pinned notes, the
# response line and the residual SD were all missing from its output, each
# added to the C at some point and never carried across. A twin that is only
# claimed to be a twin drifts.
#
# Skipped, not failed, where there is no JDK: the C build must not require one.
#
#   sh scripts/java-check.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if ! command -v javac >/dev/null 2>&1 || ! command -v java >/dev/null 2>&1; then
    echo "java: no JDK, skipped"
    exit 0
fi
if [ ! -x ./linearr ]; then
    echo "java: ./linearr is not built, skipped"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT HUP TERM

javac -d "$tmp/classes" java/*.java 2>"$tmp/javac.err" || {
    echo "java: javac failed"; cat "$tmp/javac.err"; exit 1; }
# Notes are not warnings. The raw Hashtable and Vector are the point of the
# file, so javac's "uses unchecked or unsafe operations" note is expected and
# permanent; anything at warning or error level is not.
if grep -E "(warning|error):" "$tmp/javac.err" >/dev/null 2>&1; then
    echo "java: javac is not warning-free"
    grep -E "(warning|error):" "$tmp/javac.err"
    exit 1
fi

pass=0; fail=0; skip=0
for f in example/*.csv; do
    # Only training files: a training header is group, the value, then terms,
    # and the scoring examples have neither.
    head=$(grep -v '^#' "$f" | head -1)
    case "$head" in group,*) ;; *) skip=$((skip+1)); continue ;; esac
    # A coefficient table also begins with "group,". It is a model, not training
    # data, and fitting it produces nonsense in both implementations rather than
    # a disagreement worth reading.
    case "$head" in group,intercept,*) skip=$((skip+1)); continue ;; esac
    # And a training file needs a group, a value and at least one term, so
    # anything narrower is not one. The trim table is two columns.
    #
    # Counted with tr | wc -w and not wc -l: tr turns N fields into N-1
    # newlines, so the first version of this dropped every THREE-column file,
    # which is anscombe.csv and curve.csv, from both gates. It reported
    # agreement on what was left and said nothing about what it had skipped.
    [ "$(printf '%s' "$head" | tr ',' ' ' | wc -w)" -ge 3 ] || { skip=$((skip+1)); continue; }
    # The Java fits every group in one pass and takes no options, so compare it
    # against the C's default. Both streams, since the summary is on stderr.
    ./linearr -t "$f"                     >"$tmp/c.out"  2>"$tmp/c.err"  || true
    java -cp "$tmp/classes" Linearr "$f"  >"$tmp/j.out"  2>"$tmp/j.err"  || true
    if cmp -s "$tmp/c.out" "$tmp/j.out" && cmp -s "$tmp/c.err" "$tmp/j.err"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "  DIFFERS  $f"
        diff "$tmp/c.out" "$tmp/j.out" | sed 's/^/      out: /' | head -6 || true
        diff "$tmp/c.err" "$tmp/j.err" | sed 's/^/      err: /' | head -6 || true
    fi
done
echo "java: $pass agreed, $fail differed, $skip not a training file"
[ "$fail" -eq 0 ]
