/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* regress.c -- see regress.h. Normal equations accumulated one observation at a
 * time, then Gauss-Jordan with partial pivoting. */
#include "regress.h"
#include "common.h"

#include <math.h>
#include <string.h>

int regress_init(struct regress *r, int nvars) {
    if (nvars < 0 || nvars > REGRESS_MAX_VARS) return -1;
    memset(r, 0, sizeof *r);
    r->nvars = nvars;
    return 0;
}

int regress_add(struct regress *r, const double *x, double y) {
    double t[REGRESS_MAX_TERMS];
    int p = r->nvars + 1;
    int i, j;

    t[0] = 1.0;                                     /* the intercept column */
    for (i = 0; i < r->nvars; i++) t[i + 1] = x[i];

    for (i = 0; i < p; i++) {
        for (j = 0; j < p; j++) r->xtx[i][j] += t[i] * t[j];
        r->xty[i] += t[i] * y;
    }
    r->sy  += y;
    r->syy += y * y;
    r->n++;
    return 0;
}

int regress_solve(const struct regress *r, double *beta) {
    double a[REGRESS_MAX_TERMS][REGRESS_MAX_TERMS + 1];   /* [X'X | X'y] */
    int    pivot_col[REGRESS_MAX_TERMS];
    int    p = r->nvars + 1;
    int    rank = 0, col, i, j, k;
    double scale = 0.0, eps;

    if (r->n <= 0) return -1;

    for (i = 0; i < p; i++) {
        for (j = 0; j < p; j++) a[i][j] = r->xtx[i][j];
        a[i][p] = r->xty[i];
        if (fabs(r->xtx[i][i]) > scale) scale = fabs(r->xtx[i][i]);
    }
    /* Singular means "this column carries no information the others don't", and
     * that verdict has to be relative to the size of the numbers involved. */
    eps = (scale > 0.0 ? scale : 1.0) * 1e-12;

    for (i = 0; i < p; i++) beta[i] = 0.0;

    for (col = 0; col < p; col++) {
        int best = rank;
        double piv;

        for (i = rank; i < p; i++)
            if (fabs(a[i][col]) > fabs(a[best][col])) best = i;

        if (rank >= p || fabs(a[best][col]) <= eps) {
            debug("regress: term %d is unidentified, pinned to 0", col);
            continue;                        /* leave beta[col] at 0 */
        }

        if (best != rank)
            for (j = col; j <= p; j++) {
                double tmp = a[rank][j]; a[rank][j] = a[best][j]; a[best][j] = tmp;
            }

        piv = a[rank][col];
        for (j = col; j <= p; j++) a[rank][j] /= piv;

        for (i = 0; i < p; i++) {
            double f;
            if (i == rank) continue;
            f = a[i][col];
            if (f == 0.0) continue;
            for (j = col; j <= p; j++) a[i][j] -= f * a[rank][j];
        }
        pivot_col[rank] = col;
        rank++;
    }

    for (k = 0; k < rank; k++) beta[pivot_col[k]] = a[k][p];
    return p - rank;
}

double regress_r2(const struct regress *r, const double *beta) {
    int    p = r->nvars + 1;
    int    i;
    double sst, sse = r->syy;

    if (r->n <= 0) return -1.0;
    sst = r->syy - (r->sy * r->sy) / (double)r->n;
    if (sst <= 0.0) return -1.0;               /* y never varies: no R^2 */

    /* The residual sum of squares without holding a single row: the fitted beta
     * satisfies the normal equations, so SSE = y'y - beta'X'y falls out of the
     * cross-products we already carry. */
    for (i = 0; i < p; i++) sse -= beta[i] * r->xty[i];
    if (sse < 0.0) sse = 0.0;                  /* rounding, not a negative SSE */

    return 1.0 - sse / sst;
}
