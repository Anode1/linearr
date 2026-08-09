#!/usr/bin/env python3
"""compare.py REF.CSV OTHER.CSV: max relative difference between two
coefficient tables. Prints the number and exits 1 if it exceeds the tolerance.

This is what makes the benchmark a benchmark: a speed number nobody checked is
a speed number for a different answer."""
import sys

TOL = 1e-9


def read(path):
    rows = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] == "#":
                continue
            parts = line.split(",")
            try:
                rows[parts[0]] = [float(v) for v in parts[1:]]
            except ValueError:
                continue          # the header
    return rows


a, b = read(sys.argv[1]), read(sys.argv[2])
if set(a) != set(b):
    print("GROUPS DIFFER (%d vs %d)" % (len(a), len(b)))
    sys.exit(1)

worst = 0.0
for g in a:
    if len(a[g]) != len(b[g]):
        print("WIDTH DIFFERS for %s" % g)
        sys.exit(1)
    for x, y in zip(a[g], b[g]):
        d = abs(x - y) / max(1.0, abs(x), abs(y))
        worst = max(worst, d)

print("%.2g" % worst)
sys.exit(0 if worst <= TOL else 1)
