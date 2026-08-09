#!/usr/bin/env python3
"""fit.py -- the Python baseline for scripts/bench.sh.

Streaming on purpose: `for line in f`, one row folded into its group's
cross-products and dropped. NOT pandas. Writing this as
`pd.read_csv(...).groupby(...)` and the C as a stream would compare the two
STYLES, not the two languages -- and pandas' number would be about pandas.
The idiomatic-and-materialising case is fit.R, which says so.
"""
import sys


def solve(xtx, xty, n):
    m = [row[:] + [xty[i]] for i, row in enumerate(xtx)]
    scale = max((abs(xtx[i][i]) for i in range(n)), default=0.0)
    eps = (scale if scale > 0 else 1.0) * 1e-12
    rank, pivot_col = 0, [0] * n
    for col in range(n):
        if rank >= n:
            break
        best = max(range(rank, n), key=lambda i: abs(m[i][col]))
        if abs(m[best][col]) <= eps:
            continue
        m[rank], m[best] = m[best], m[rank]
        piv = m[rank][col]
        for j in range(col, n + 1):
            m[rank][j] /= piv
        for i in range(n):
            if i == rank:
                continue
            f = m[i][col]
            if f == 0.0:
                continue
            for j in range(col, n + 1):
                m[i][j] -= f * m[rank][j]
        pivot_col[rank] = col
        rank += 1
    b = [0.0] * n
    for k in range(rank):
        b[pivot_col[k]] = m[k][n]
    return b


def main(path):
    groups, names, p = {}, None, 0
    with open(path) as f:
        for line in f:
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
            acc = groups.get(fields[0])
            if acc is None:
                acc = ([[0.0] * (p + 1) for _ in range(p + 1)], [0.0] * (p + 1))
                groups[fields[0]] = acc
            xtx, xty = acc
            y = float(fields[1])
            t = [1.0] + [float(v) for v in fields[2:]]
            for i in range(p + 1):
                ti = t[i]
                row = xtx[i]
                for j in range(p + 1):
                    row[j] += ti * t[j]
                xty[i] += ti * y

    out = sys.stdout
    out.write("GROUP,Intercept," + ",".join(names) + "\n")
    for g, (xtx, xty) in groups.items():
        b = solve(xtx, xty, p + 1)
        out.write(g + "," + ",".join(repr(v) for v in b) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
