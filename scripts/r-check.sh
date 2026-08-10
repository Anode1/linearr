#!/bin/sh
# r-check.sh: fit every example with R's lm() as well, and compare.
#
# The README claimed agreement with lm() to the printed digit. Nothing tested
# it, no gate could fail if it stopped being true, and a reviewer repeated it
# back as a property of the test suite. A claim about another program's output
# has to be run against that program or dropped.
#
# lm() is the reference here for a reason: it solves by QR with column pivoting
# (LINPACK's dqrdc2), which is a different method from the normal equations this
# program uses by default. Agreement between two different methods is evidence;
# agreement between two runs of the same one is not. So --qr is compared at a
# tight tolerance, and the default solver is REPORTED at whatever it manages,
# since squaring the condition number is exactly what it is documented to cost.
#
# Skipped, not failed, where there is no R: the C build must not require one.
#
#   sh scripts/r-check.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if ! command -v Rscript >/dev/null 2>&1; then
    echo "R: no Rscript, skipped"
    exit 0
fi
if [ ! -x ./linearr ]; then
    echo "R: ./linearr is not built, skipped"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT HUP TERM

cat > "$tmp/cmp.R" <<'RS'
# args: TRAIN.CSV  COEF.CSV  TOLERANCE
# Prints the worst relative difference between lm()'s coefficients and the
# coefficient file's, over every group, and exits 1 if it exceeds TOLERANCE.
args  <- commandArgs(trailingOnly = TRUE)
train <- read.csv(args[1], comment.char = "#", check.names = FALSE)
mine  <- read.csv(args[2], comment.char = "#", check.names = FALSE)
tol   <- as.numeric(args[3])

worst <- 0
for (i in seq_len(nrow(mine))) {
    g <- as.character(mine[[1]][i])
    d <- train[as.character(train[[1]]) == g, , drop = FALSE]
    y <- d[[2]]
    X <- as.matrix(d[, -(1:2), drop = FALSE])
    # Renamed, so a term called "1" or "x[2]" cannot become a formula. lm() on a
    # matrix names the coefficients (Intercept), Xc1, Xc2, ...
    colnames(X) <- paste0("c", seq_len(ncol(X)))
    m <- suppressWarnings(lm(y ~ X))
    b <- coef(m)
    # An aliased column is NA in R and 0 here, and both mean the same thing:
    # the data cannot identify it. This program says which, in a # pinned note.
    b[is.na(b)] <- 0
    theirs <- as.numeric(c(b[1], b[-1]))
    ours   <- as.numeric(mine[i, -1])
    if (length(theirs) != length(ours)) {
        cat("      group", g, ": lm() returned", length(theirs),
            "coefficients and this returned", length(ours), "\n")
        quit(status = 1)
    }
    # Relative to the coefficient, or absolute where it is zero.
    scale <- pmax(abs(theirs), 1e-8)
    rel   <- max(abs(theirs - ours) / scale)
    if (rel > worst) worst <- rel
}
cat(sprintf("%.2e", worst))
if (worst > tol) quit(status = 1)
RS

pass=0; fail=0
printf "%-28s %-12s %s\n" "file" "--qr vs lm()" "default vs lm()"
for f in example/*.csv; do
    head=$(grep -v '^#' "$f" | head -1)
    case "$head" in group,*) ;; *) continue ;; esac
    # A coefficient table also begins with "group,". It is a model, not training
    # data, and fitting it produces nonsense in both implementations rather than
    # a disagreement worth reading.
    case "$head" in group,intercept,*) continue ;; esac
    # And a training file needs a group, a value and at least one term, so
    # anything narrower is not one. The trim table is two columns.
    [ "$(printf '%s' "$head" | tr ',' '\n' | wc -l)" -ge 3 ] || continue
    # The refusal examples are meant to fail; they are not fits to compare.
    ./linearr -t "$f" --qr > "$tmp/qr.csv" 2>/dev/null || continue
    ./linearr -t "$f"       > "$tmp/ne.csv" 2>/dev/null || continue

    set +e
    q=$(Rscript --vanilla "$tmp/cmp.R" "$f" "$tmp/qr.csv" 1e-6 2>"$tmp/rerr"); qrc=$?
    n=$(Rscript --vanilla "$tmp/cmp.R" "$f" "$tmp/ne.csv" 1 2>/dev/null)
    set -e
    if [ "$qrc" -ne 0 ]; then
        fail=$((fail+1))
        printf "%-28s %-12s %s   DIFFERS\n" "$f" "${q:-error}" "${n:-error}"
        [ -s "$tmp/rerr" ] && sed 's/^/      /' "$tmp/rerr"
    else
        pass=$((pass+1))
        printf "%-28s %-12s %s\n" "$f" "$q" "${n:-error}"
    fi
done
echo "R: $pass agreed with lm() to 1e-6 under --qr, $fail differed"
[ "$fail" -eq 0 ]
