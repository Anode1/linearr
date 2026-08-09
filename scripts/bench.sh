#!/bin/sh
# bench.sh: the same fit, in C and in the languages people would otherwise
# write it in.
#
# The job is identical in every case: read GROUP,VALUE,<terms>, fit one ordinary
# least-squares line per group, write the coefficient table. Every result is
# compared against linearr's own table before its time is reported, because a
# speed number nobody checked is a speed number for a different answer.
#
#   sh scripts/bench.sh [TERMS] [GROUPS] [ROWS]
#   sh scripts/bench.sh 35 580 1000000        # the defaults
#
# Needs numpy, pandas and scikit-learn on some python3 for the Python rows and
# a JDK for the Java one; whatever is missing is skipped and the C row still
# runs. Generated data goes to a temp directory and is deleted on exit. Nothing
# here touches the repository.
set -e

TERMS=${1:-35}
GROUPS=${2:-580}
ROWS=${3:-1000000}

root=$(cd "$(dirname "$0")/.." && pwd)
bin=$root/linearr
[ -x "$bin" ] || { echo "build first: make" >&2; exit 1; }

PYTHON=${PYTHON:-python3}
have_py=1
$PYTHON -c "import numpy, pandas, sklearn" 2>/dev/null || have_py=0
[ "$have_py" = 1 ] || echo "note: $PYTHON has no numpy/pandas/sklearn; skipping the Python rows"

have_java=1
command -v javac >/dev/null 2>&1 || have_java=0
[ "$have_java" = 1 ] || echo "note: no javac; skipping the Java row"

if /usr/bin/time -f %M true 2>/dev/null; then have_rss=1; else have_rss=0
    echo "note: /usr/bin/time not available; reporting wall time only"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

run() {                                    # run CMD..., echo "seconds rss_kb"
    if [ "$have_rss" = 1 ]; then
        /usr/bin/time -f "%e %M" "$@" 2>&1 >/dev/null | tail -1
    else
        s=$(date +%s); "$@" >/dev/null 2>&1; echo "$(( $(date +%s) - s )) -"
    fi
}

report() {                                 # report NAME "SECONDS RSSKB" [NOTE]
    set -- "$1" $2 "$3"
    if [ "$2" = "" ]; then printf '  %-16s %10s %12s   %s\n' "$1" "?" "" "$4"
    else printf '  %-16s %9ss %10s MB   %s\n' "$1" "$2" "$(( ${3:-0} / 1024 ))" "$4"; fi
}

# One group's coefficients differ from the next, so a per-group fit is a real
# fit and not one line repeated. Indicators at p=0.3, a little noise so the
# residual degrees of freedom mean something.
echo "generating $ROWS rows, $TERMS terms, $GROUPS groups ..."
awk -v terms="$TERMS" -v groups="$GROUPS" -v rows="$ROWS" 'BEGIN {
    srand(7)
    for (g = 1; g <= groups; g++) {
        b[g, 0] = 2 + rand() * 6
        for (j = 1; j <= terms; j++) b[g, j] = (rand() < 0.7) ? int(rand() * 1600) / 100 : 0
    }
    printf "GROUP,LOS"
    for (j = 1; j <= terms; j++) printf ",term_%d", j
    printf "\n"
    for (i = 0; i < rows; i++) {
        g = int(rand() * groups) + 1
        y = b[g, 0]; line = ""
        for (j = 1; j <= terms; j++) {
            x = (rand() < 0.3) ? 1 : 0
            y += b[g, j] * x
            line = line "," x
        }
        printf "G%04d,%.4f%s\n", g, y + (rand() - 0.5) * 0.5, line
    }
}' > "$tmp/train.csv"
echo "  $(du -h "$tmp/train.csv" | cut -f1) on disk"
echo

cat > "$tmp/fit.py" <<'PY'
"""The same job, three ways Python is actually written for it."""
import sys
import numpy as np
import pandas as pd

variant, infile, outfile = sys.argv[1], sys.argv[2], sys.argv[3]


def write(path, terms, coefs):
    with open(path, "w") as fh:
        fh.write("GROUP,Intercept," + ",".join(terms) + "\n")
        for g in sorted(coefs):
            fh.write(g + "," + ",".join(repr(float(v)) for v in coefs[g]) + "\n")


if variant in ("pandas-lstsq", "sklearn"):
    df = pd.read_csv(infile, comment="#")
    gcol, ycol, terms = df.columns[0], df.columns[1], list(df.columns[2:])
    coefs = {}
    if variant == "sklearn":
        from sklearn.linear_model import LinearRegression
    for g, sub in df.groupby(gcol, sort=False):
        X = sub[terms].to_numpy(dtype=np.float64)
        y = sub[ycol].to_numpy(dtype=np.float64)
        if variant == "sklearn":
            m = LinearRegression().fit(X, y)
            coefs[g] = np.concatenate(([m.intercept_], m.coef_))
        else:
            A = np.column_stack([np.ones(len(X)), X])
            coefs[g] = np.linalg.lstsq(A, y, rcond=None)[0]
    write(outfile, terms, coefs)

elif variant == "stream-numpy":            # the algorithmic twin of linearr
    acc, terms = {}, None
    for chunk in pd.read_csv(infile, comment="#", chunksize=200000):
        if terms is None:
            gcol, ycol, terms = chunk.columns[0], chunk.columns[1], list(chunk.columns[2:])
        for g, sub in chunk.groupby(gcol, sort=False):
            X = sub[terms].to_numpy(dtype=np.float64)
            y = sub[ycol].to_numpy(dtype=np.float64)
            A = np.column_stack([np.ones(len(X)), X])
            a = acc.get(g)
            if a is None:
                acc[g] = [A.T @ A, A.T @ y]
            else:
                a[0] += A.T @ A
                a[1] += A.T @ y
    write(outfile, terms,
          {g: np.linalg.lstsq(m, v, rcond=None)[0] for g, (m, v) in acc.items()})
else:
    sys.exit("unknown variant " + variant)
PY

cat > "$tmp/fit_pure.py" <<'PY'
"""The same method with nothing imported that is not in the standard library:
stream the file, accumulate X'X and X'y per group in lists, Gauss-Jordan at the
end. What the language costs when no C library is standing in for it."""
import csv
import sys


def solve(a, b, p):
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(p):
        piv = max(range(col, p), key=lambda r: abs(m[r][col]))
        if abs(m[piv][col]) < 1e-12:
            continue
        m[col], m[piv] = m[piv], m[col]
        d = m[col][col]
        for j in range(col, p + 1):
            m[col][j] /= d
        for r in range(p):
            if r == col:
                continue
            f = m[r][col]
            if f:
                for j in range(col, p + 1):
                    m[r][j] -= f * m[col][j]
    return [m[i][p] for i in range(p)]


acc = {}
with open(sys.argv[1], newline="") as fh:
    rd = csv.reader(fh)
    terms = next(rd)[2:]
    p = len(terms) + 1
    for row in rd:
        if not row or row[0].startswith("#"):
            continue
        y = float(row[1])
        x = [1.0] + [float(v) for v in row[2:]]
        a = acc.get(row[0])
        if a is None:
            a = acc[row[0]] = ([[0.0] * p for _ in range(p)], [0.0] * p)
        xtx, xty = a
        for i in range(p):
            xi = x[i]
            if xi:
                ri = xtx[i]
                for j in range(p):
                    ri[j] += xi * x[j]
                xty[i] += xi * y
with open(sys.argv[2], "w") as out:
    out.write("GROUP,Intercept," + ",".join(terms) + "\n")
    for g in sorted(acc):
        out.write(g + "," + ",".join(repr(v) for v in solve(*acc[g], p)) + "\n")
PY

cat > "$tmp/agree.py" <<'PY'
"""Do the two coefficient tables agree? Prints the worst relative difference."""
import sys
import numpy as np
import pandas as pd


def load(p):
    d = pd.read_csv(p, comment="#")
    return d.set_index(d.columns[0]).sort_index().astype(np.float64)


a, b = load(sys.argv[1]), load(sys.argv[2])
i = a.index.intersection(b.index)
if len(i) != len(a) or len(i) != len(b):
    print("GROUP SETS DIFFER"); sys.exit(1)
d = np.abs(a.loc[i].to_numpy() - b.loc[i].to_numpy())
print("%.1e" % (d / np.maximum(np.abs(a.loc[i].to_numpy()), 1.0)).max())
PY

if [ "$have_java" = 1 ]; then
cat > "$tmp/Fit.java" <<'JAVA'
// The same job, written the way a Java program of this kind is normally
// written: BufferedReader, split(","), parseDouble, one accumulator per group,
// Gauss-Jordan at the end. No tricks the C and Python versions do not get.
import java.io.*;
import java.util.*;

public class Fit {
    public static void main(String[] args) throws IOException {
        BufferedReader r = new BufferedReader(new FileReader(args[0]), 1 << 20);
        String[] hf = r.readLine().split(",");
        String[] terms = Arrays.copyOfRange(hf, 2, hf.length);
        int p = terms.length + 1;
        HashMap<String, double[][]> xtx = new HashMap<>();
        HashMap<String, double[]>   xty = new HashMap<>();
        double[] x = new double[p];
        String line;
        while ((line = r.readLine()) != null) {
            if (line.isEmpty() || line.charAt(0) == '#') continue;
            String[] f = line.split(",");
            double y = Double.parseDouble(f[1]);
            x[0] = 1.0;
            for (int j = 1; j < p; j++) x[j] = Double.parseDouble(f[j + 1]);
            double[][] m = xtx.get(f[0]);
            double[] v;
            if (m == null) { m = new double[p][p]; xtx.put(f[0], m);
                             v = new double[p];    xty.put(f[0], v); }
            else           { v = xty.get(f[0]); }
            for (int i = 0; i < p; i++) {
                double xi = x[i];
                double[] mi = m[i];
                for (int j = 0; j < p; j++) mi[j] += xi * x[j];
                v[i] += xi * y;
            }
        }
        r.close();
        StringBuilder sb = new StringBuilder("GROUP,Intercept");
        for (String t : terms) sb.append(',').append(t);
        sb.append('\n');
        String[] groups = xtx.keySet().toArray(new String[0]);
        Arrays.sort(groups);
        for (String g : groups) {
            double[] b = solve(xtx.get(g), xty.get(g), p);
            sb.append(g);
            for (int i = 0; i < p; i++) sb.append(',').append(b[i]);
            sb.append('\n');
        }
        try (Writer w = new BufferedWriter(new FileWriter(args[1]))) {
            w.write(sb.toString());
        }
    }

    // A column with no pivot stays 0, which is the answer linearr gives for a
    // term the data cannot identify.
    static double[] solve(double[][] a, double[] rhs, int p) {
        double[][] m = new double[p][p + 1];
        for (int i = 0; i < p; i++) {
            System.arraycopy(a[i], 0, m[i], 0, p);
            m[i][p] = rhs[i];
        }
        for (int col = 0; col < p; col++) {
            int best = col;
            for (int i = col + 1; i < p; i++)
                if (Math.abs(m[i][col]) > Math.abs(m[best][col])) best = i;
            if (Math.abs(m[best][col]) < 1e-12) continue;
            double[] t = m[col]; m[col] = m[best]; m[best] = t;
            double d = m[col][col];
            for (int j = col; j <= p; j++) m[col][j] /= d;
            for (int i = 0; i < p; i++) {
                if (i == col) continue;
                double f = m[i][col];
                if (f == 0.0) continue;
                for (int j = col; j <= p; j++) m[i][j] -= f * m[col][j];
            }
        }
        double[] b = new double[p];
        for (int i = 0; i < p; i++) b[i] = m[i][p];
        return b;
    }
}
JAVA
javac -d "$tmp" "$tmp/Fit.java" 2>/dev/null || { have_java=0; echo "note: javac failed; skipping the Java row"; }
fi

echo "FIT: $ROWS rows, $TERMS terms, $GROUPS groups"
printf '  %-16s %10s %13s   %s\n' fitter seconds "peak RSS" "agrees with linearr to"
report "linearr (C)" "$(run sh -c "'$bin' -t '$tmp/train.csv' > '$tmp/c.csv'")" ""

if [ "$have_java" = 1 ]; then
    t=$(run java -Xmx8g -cp "$tmp" Fit "$tmp/train.csv" "$tmp/java.csv")
    if [ "$have_py" = 1 ]; then
        report "java" "$t" "$($PYTHON "$tmp/agree.py" "$tmp/c.csv" "$tmp/java.csv")"
    else
        report "java" "$t" "(no python to check it with)"
    fi
fi

if [ "$have_py" = 1 ]; then
    for v in stream-numpy pandas-lstsq sklearn; do
        t=$(run "$PYTHON" "$tmp/fit.py" "$v" "$tmp/train.csv" "$tmp/py.csv")
        report "$v" "$t" "$($PYTHON "$tmp/agree.py" "$tmp/c.csv" "$tmp/py.csv")"
    done
fi

# The row that shows what the language costs when no C library is standing in
# for it: the same method, in Python, with nothing imported that is not in the
# standard library. This is the one that takes minutes, so it is last.
if [ "${SKIP_SLOW:-0}" != 1 ]; then
    t=$(run "$PYTHON" "$tmp/fit_pure.py" "$tmp/train.csv" "$tmp/pure.csv")
    if [ "$have_py" = 1 ]; then
        report "python, no numpy" "$t" "$($PYTHON "$tmp/agree.py" "$tmp/c.csv" "$tmp/pure.csv")"
    else
        report "python, no numpy" "$t" "(nothing to check it with)"
    fi
fi

echo
echo "The C row's memory is the point: it is the model's size, not the file's."
