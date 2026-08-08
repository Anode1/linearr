/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* regress.c -- see regress.h. Online centered co-moments, then Gauss-Jordan
 * with partial pivoting on the equilibrated system. */
#include "regress.h"
#include "common.h"

#include <math.h>
#include <string.h>

/* Rank tolerance on the EQUILIBRATED matrix, whose diagonal is all ones. A
 * pivot this small means the column is a linear combination of the others to
 * within twelve digits. Because the matrix is scaled first, this is a pure
 * rank statement and carries no units -- which is the whole point; the absolute
 * version of this constant deleted well-identified columns for being measured
 * in the wrong unit. */
#define RANK_EPS 1e-12

/* Past this, the coefficients' trailing digits are noise. Normal equations
 * square the condition number of the design, so a reported 1e8 here is roughly
 * cond(X) = 1e4 and about half the mantissa is gone. */
#define CONDITION_WARN 1e8

size_t regress_storage(int nvars) {
    if (nvars < 1) return 0;
    return (size_t)nvars * (size_t)nvars      /* c   */
         + (size_t)nvars                      /* mean */
         + (size_t)nvars;                     /* cxy  */
}

size_t regress_solve_storage(int nvars) {
    if (nvars < 1) return 0;
    return (size_t)nvars * ((size_t)nvars + 1);   /* [C | Cxy], augmented */
}

int regress_init(struct regress *r, int nvars, double *storage) {
    size_t need;

    if (nvars < 1 || nvars > REGRESS_MAX_VARS || !storage) {
        debug("regress: %d terms is outside 1..%d, or no storage",
              nvars, REGRESS_MAX_VARS);
        return -1;
    }
    need = regress_storage(nvars);
    memset(storage, 0, need * sizeof *storage);

    r->nvars = nvars;
    r->n     = 0;
    r->my    = 0.0;
    r->cyy   = 0.0;
    r->c     = storage;
    r->mean  = storage + (size_t)nvars * (size_t)nvars;
    r->cxy   = r->mean + nvars;
    return 0;
}

int regress_add(struct regress *r, const double *x, double y) {
    int i, j, p = r->nvars;
    double dy;

    /* Refuse non-finite input at the door. One nan admitted here reaches every
     * coefficient, and the resulting table of "nan" used to load cleanly and
     * score "prediction=nan" with exit 0. */
    if (!isfinite(y)) { debug("regress: response is not finite"); return -1; }
    for (i = 0; i < p; i++)
        if (!isfinite(x[i])) { debug("regress: term %d is not finite", i + 1); return -1; }

    r->n++;
    dy = y - r->my;

    /* Online covariance: each co-moment is updated with one deviation taken
     * BEFORE its mean moves and one taken AFTER, which is what makes the
     * running form exact rather than merely close. */
    for (i = 0; i < p; i++) {
        double dxi = x[i] - r->mean[i];
        for (j = 0; j < p; j++) {
            double dxj_new = x[j] - (r->mean[j] + (x[j] - r->mean[j]) / (double)r->n);
            r->c[(size_t)i * (size_t)p + (size_t)j] += dxi * dxj_new;
        }
        r->cxy[i] += dxi * (y - (r->my + dy / (double)r->n));
    }
    for (i = 0; i < p; i++) r->mean[i] += (x[i] - r->mean[i]) / (double)r->n;
    r->my  += dy / (double)r->n;
    r->cyy += dy * (y - r->my);
    return 0;
}

int regress_solve(const struct regress *r, double *beta, double *scratch,
                  struct regress_fit *fit) {
    int    p = r->nvars;
    int    stride = p + 1;                 /* augmented column at the end */
    int    pivot_col[REGRESS_MAX_VARS];
    double d[REGRESS_MAX_VARS];            /* equilibration scale per column */
    double pivmax = 0.0, pivmin = 0.0;
    int    rank = 0, col, i, j, k;

    if (!r->n || p < 1 || !scratch) return -1;

    for (i = 0; i <= p; i++) beta[i] = 0.0;
    if (fit) for (i = 0; i < p; i++) fit->term[i] = REGRESS_COLLINEAR;

    /* Equilibrate: divide row i and column j by the square roots of their own
     * variances, so the matrix has a unit diagonal and the pivot test below is
     * about rank rather than about units. A column with no variance at all gets
     * scale 0 and is pinned -- correctly: it is collinear with the intercept. */
    for (i = 0; i < p; i++) {
        double cii = r->c[(size_t)i * (size_t)p + (size_t)i];
        d[i] = (cii > 0.0) ? sqrt(cii) : 0.0;
    }
    for (i = 0; i < p; i++) {
        for (j = 0; j < p; j++) {
            double v = r->c[(size_t)i * (size_t)p + (size_t)j];
            scratch[(size_t)i * (size_t)stride + (size_t)j] =
                (d[i] > 0.0 && d[j] > 0.0) ? v / (d[i] * d[j]) : 0.0;
        }
        scratch[(size_t)i * (size_t)stride + (size_t)p] =
            (d[i] > 0.0) ? r->cxy[i] / d[i] : 0.0;
    }

    for (col = 0; col < p; col++) {
        int    best = rank;
        double piv;

        if (rank >= p) break;
        for (i = rank; i < p; i++)
            if (fabs(scratch[(size_t)i * (size_t)stride + (size_t)col]) >
                fabs(scratch[(size_t)best * (size_t)stride + (size_t)col])) best = i;

        piv = scratch[(size_t)best * (size_t)stride + (size_t)col];
        if (d[col] == 0.0 || fabs(piv) <= RANK_EPS) {
            /* Two reasons, and they are not the same claim. A column with no
             * variance carries no evidence about anything. A column collinear
             * with another has evidence that cannot be attributed -- and which
             * of the pair keeps it depends on the order they were listed in. */
            if (fit) fit->term[col] = (d[col] == 0.0) ? REGRESS_CONSTANT
                                                      : REGRESS_COLLINEAR;
            debug("regress: term %d is %s, pinned to 0", col + 1,
                  d[col] == 0.0 ? "constant" : "collinear with another");
            continue;                        /* beta stays 0 */
        }

        if (best != rank)
            for (j = col; j <= p; j++) {
                double tmp = scratch[(size_t)rank * (size_t)stride + (size_t)j];
                scratch[(size_t)rank * (size_t)stride + (size_t)j] =
                    scratch[(size_t)best * (size_t)stride + (size_t)j];
                scratch[(size_t)best * (size_t)stride + (size_t)j] = tmp;
            }

        piv = scratch[(size_t)rank * (size_t)stride + (size_t)col];
        if (pivmax == 0.0 || fabs(piv) > pivmax) pivmax = fabs(piv);
        if (pivmin == 0.0 || fabs(piv) < pivmin) pivmin = fabs(piv);

        for (j = col; j <= p; j++) scratch[(size_t)rank * (size_t)stride + (size_t)j] /= piv;

        for (i = 0; i < p; i++) {
            double f;
            if (i == rank) continue;
            f = scratch[(size_t)i * (size_t)stride + (size_t)col];
            if (f == 0.0) continue;
            for (j = col; j <= p; j++)
                scratch[(size_t)i * (size_t)stride + (size_t)j] -=
                    f * scratch[(size_t)rank * (size_t)stride + (size_t)j];
        }
        pivot_col[rank] = col;
        if (fit) fit->term[col] = REGRESS_FITTED;
        rank++;
    }

    /* Unscale: the solved vector is in equilibrated units. */
    for (k = 0; k < rank; k++) {
        int c = pivot_col[k];
        beta[c + 1] = scratch[(size_t)k * (size_t)stride + (size_t)p] / d[c];
    }

    /* The intercept falls out of the means, so it is never a candidate for
     * pinning and never competes with a constant regressor. */
    beta[0] = r->my;
    for (i = 0; i < p; i++) beta[0] -= beta[i + 1] * r->mean[i];

    for (i = 0; i <= p; i++)
        if (!isfinite(beta[i])) { debug("regress: coefficient %d is not finite", i); return -1; }

    if (fit) {
        fit->pinned = p - rank;
        fit->df     = r->n - (long)rank - 1;
        fit->condition = (pivmin > 0.0) ? pivmax / pivmin : 1.0;

        /* SSE from centered quantities: the residual is orthogonal to the
         * fitted columns, so SSE = Cyy - b'Cxy exactly, with no cancellation
         * against a mean that was never subtracted. */
        if (r->cyy > 0.0) {
            double sse = r->cyy;
            for (i = 0; i < p; i++) sse -= beta[i + 1] * r->cxy[i];

            /* Two different things look alike here and must not be conflated.
             * A fit that is exact leaves an SSE of a few ulps, negative as
             * often as positive -- that is rounding, and R2 really is 1. An
             * SSE meaningfully below zero is arithmetic that has lost its
             * meaning, and the honest answer is to say so rather than clamp to
             * zero and report a perfect score, which is what the old code did:
             * it turned a true R2 of 0 into a printed 1.0000. */
            if (sse < -1e-9 * r->cyy) {
                fit->r2 = -1.0;          /* not computable to useful precision */
            } else {
                if (sse < 0.0) sse = 0.0;
                fit->r2 = 1.0 - sse / r->cyy;
                if (fit->r2 < 0.0) fit->r2 = 0.0;
            }
        } else {
            fit->r2 = -1.0;               /* the response never varies */
        }
    }
    return 0;
}
