# fit.awk: the awk baseline for scripts/bench.sh. Streaming is awk's only
# idiom, which is the point: this is what the language does naturally, not a
# style chosen to flatter the benchmark.
#   awk -f fit.awk train.csv
BEGIN { FS = ","; hdr = 0 }
/^#/ || /^$/ { next }
hdr == 0 {
    p = NF - 2
    for (j = 1; j <= p; j++) name[j] = $(j + 2)
    hdr = 1
    next
}
NF == p + 2 {
    g = $1
    if (!(g in seen)) { seen[g] = 1; order[++ng] = g }
    t[0] = 1
    for (j = 1; j <= p; j++) t[j] = $(j + 2) + 0
    y = $2 + 0
    for (i = 0; i <= p; i++) {
        for (j = 0; j <= p; j++) xtx[g, i, j] += t[i] * t[j]
        xty[g, i] += t[i] * y
    }
}
END {
    line = "group,intercept"
    for (j = 1; j <= p; j++) line = line "," name[j]
    print line
    n = p + 1
    for (k = 1; k <= ng; k++) {
        g = order[k]
        scale = 0
        for (i = 0; i < n; i++) { v = xtx[g, i, i]; if (v < 0) v = -v; if (v > scale) scale = v }
        eps = (scale > 0 ? scale : 1) * 1e-12
        for (i = 0; i < n; i++) { for (j = 0; j < n; j++) m[i, j] = xtx[g, i, j]; m[i, n] = xty[g, i] }
        rank = 0
        for (col = 0; col < n && rank < n; col++) {
            best = rank
            for (i = rank; i < n; i++) {
                a = m[i, col]; if (a < 0) a = -a
                bb = m[best, col]; if (bb < 0) bb = -bb
                if (a > bb) best = i
            }
            a = m[best, col]; if (a < 0) a = -a
            if (a <= eps) continue
            for (j = col; j <= n; j++) { tmp = m[rank, j]; m[rank, j] = m[best, j]; m[best, j] = tmp }
            piv = m[rank, col]
            for (j = col; j <= n; j++) m[rank, j] /= piv
            for (i = 0; i < n; i++) {
                if (i == rank) continue
                f = m[i, col]
                if (f == 0) continue
                for (j = col; j <= n; j++) m[i, j] -= f * m[rank, j]
            }
            pc[rank] = col; rank++
        }
        for (i = 0; i < n; i++) b[i] = 0
        for (i = 0; i < rank; i++) b[pc[i]] = m[i, n]
        line = g
        for (i = 0; i < n; i++) line = line "," sprintf("%.12g", b[i])
        print line
    }
}
