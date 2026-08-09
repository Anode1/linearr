#!/usr/bin/env python3
"""fit-frame.py: the same fit as fit.py, in the same language, on the same
machine, differing in ONE thing: it reads the whole file into memory first.

This is the control. Comparing streaming C against a materialising R would
confound the language with the style, and the memory column would be an
argument rather than a measurement. Holding language, machine and algorithm
fixed and varying only the style isolates what materialising actually costs.
"""
import sys
from fit import solve


def main(path):
    with open(path) as f:
        lines = f.readlines()                 # the whole file, on purpose

    names, p, rows = None, 0, []
    for line in lines:
        line = line.rstrip("\n")
        if not line or line[0] == "#":
            continue
        fields = line.split(",")
        if names is None:
            p = len(fields) - 2
            names = fields[2:]
            continue
        if len(fields) != p + 2:
            continue
        rows.append((fields[0], float(fields[1]), [float(v) for v in fields[2:]]))

    groups = {}
    for g, y, xs in rows:                     # every row still held
        acc = groups.get(g)
        if acc is None:
            acc = ([[0.0] * (p + 1) for _ in range(p + 1)], [0.0] * (p + 1))
            groups[g] = acc
        xtx, xty = acc
        t = [1.0] + xs
        for i in range(p + 1):
            ti, row = t[i], xtx[i]
            for j in range(p + 1):
                row[j] += ti * t[j]
            xty[i] += ti * y

    out = sys.stdout
    out.write("group,intercept," + ",".join(names) + "\n")
    for g, (xtx, xty) in groups.items():
        out.write(g + "," + ",".join("%.12g" % v for v in solve(xtx, xty, p + 1)) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
