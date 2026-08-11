/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* regress.c: see regress.h. Online centered co-moments, then Gauss-Jordan
 * with partial pivoting on the equilibrated system. */
#include "regress.h"
#include "common.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Rank tolerance on the EQUILIBRATED matrix, whose diagonal is all ones: a pure
 * rank statement carrying no units. The pivot is a residual VARIANCE fraction,
 * 1 - R^2 of that column on the others, so 1e-12 in variance is 1e-6 in the
 * column's own units. qr.c's QR_RANK_EPS is 1e-9 as a fraction of a NORM: 1e-6
 * against 1e-9 on one scale, so this solver pins about a thousand times more
 * readily, which is most of why the two disagree near rank deficiency. */
#define RANK_EPS 1e-12


/* Both matrices are row-major in a flat array the caller owns; the only place
 * that knows the layout. */
static double *row_of(double *m, int stride, int i) {
    return m + (size_t)i * (size_t)stride;
}

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
    double dnew[REGRESS_MAX_VARS];         /* the after-update deviation, per term */
    int    i, j, p = r->nvars;
    double dy, dy_new;

    /* Refuse non-finite input: one nan reaches every coefficient. */
    if (!isfinite(y)) { debug("regress: response is not finite"); return -1; }
    for (i = 0; i < p; i++)
        if (!isfinite(x[i])) { debug("regress: term %d is not finite", i + 1); return -1; }

    r->n++;
    dy = y - r->my;

    /* Online covariance: each co-moment takes a deviation from BEFORE its mean
     * moves and one from AFTER, making the running form exact. The AFTER one
     * depends on the column, not the cell: p per row, not p*p. */
    for (j = 0; j < p; j++)
        dnew[j] = x[j] - (r->mean[j] + (x[j] - r->mean[j]) / (double)r->n);
    dy_new = y - (r->my + dy / (double)r->n);

    for (i = 0; i < p; i++) {
        double  dxi = x[i] - r->mean[i];
        double *ci  = row_of(r->c, p, i);
        for (j = 0; j < p; j++) ci[j] += dxi * dnew[j];
        r->cxy[i] += dxi * dy_new;
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
     * variances, giving a unit diagonal so the pivot test is about rank, not
     * units. No variance means scale 0 and pinning: collinear with the
     * intercept. */
    for (i = 0; i < p; i++) {
        double cii = row_of(r->c, p, i)[i];
        /* An overflowed cross-product. A column near 1e160 squares to inf
         * during accumulation, sqrt(inf) is inf, every scaled entry becomes
         * inf/inf = NaN, and the pivot test compares NaN and is false --
         * unchecked, the WHOLE fit is refused as "not a finite line", good
         * columns included. Refusing beats pinning: the column is neither
         * CONSTANT nor COLLINEAR. */
        if (!isfinite(cii) || !isfinite(r->cxy[i])) {
            debug("regress: term %d overflowed its cross-products (its values "
                  "are near 1e160 or beyond); rescale that column, or use --qr, "
                  "which does not square them", i + 1);
            return -2;
        }
        d[i] = (cii > 0.0) ? sqrt(cii) : 0.0;
    }
    /* The RESPONSE side of the same overflow, invisible above: a y near 1e160
     * sends cyy to inf while every input passes isfinite, and sse = inf - inf =
     * NaN would return rc=0 with r2, rss and sigma all NaN. */
    if (!isfinite(r->cyy)) {
        debug("regress: the response overflowed its cross-products (its "
              "values are near 1e160 or beyond); rescale that column");
        return -3;
    }
    for (i = 0; i < p; i++) {
        const double *ci = row_of(r->c, p, i);
        double       *si = row_of(scratch, stride, i);
        for (j = 0; j < p; j++)
            si[j] = (d[i] > 0.0 && d[j] > 0.0) ? ci[j] / (d[i] * d[j]) : 0.0;
        si[p] = (d[i] > 0.0) ? r->cxy[i] / d[i] : 0.0;
    }

    for (col = 0; col < p; col++) {
        double *prow;                          /* the pivot row, once found */
        int     best = rank;
        double  piv;

        if (rank >= p) break;                  /* before any row is touched */
        piv = fabs(row_of(scratch, stride, rank)[col]);
        for (i = rank + 1; i < p; i++) {
            double v = fabs(row_of(scratch, stride, i)[col]);
            if (v > piv) { piv = v; best = i; }
        }

        if (d[col] == 0.0 || piv <= RANK_EPS) {
            /* Two reasons, not one claim: no variance is no evidence, collinear
             * is evidence that cannot be attributed. See enum regress_term. */
            if (fit) fit->term[col] = (d[col] == 0.0) ? REGRESS_CONSTANT
                                                      : REGRESS_COLLINEAR;
            debug("regress: term %d is %s, pinned to 0", col + 1,
                  d[col] == 0.0 ? "constant" : "collinear with another");
            continue;                        /* beta stays 0 */
        }

        prow = row_of(scratch, stride, rank);
        if (best != rank) {
            double *brow = row_of(scratch, stride, best);
            for (j = col; j <= p; j++) {
                double tmp = prow[j];
                prow[j]    = brow[j];
                brow[j]    = tmp;
            }
        }

        piv = prow[col];
        if (pivmax == 0.0 || fabs(piv) > pivmax) pivmax = fabs(piv);
        if (pivmin == 0.0 || fabs(piv) < pivmin) pivmin = fabs(piv);

        for (j = col; j <= p; j++) prow[j] /= piv;

        for (i = 0; i < p; i++) {
            double *irow;
            double  f;
            if (i == rank) continue;
            irow = row_of(scratch, stride, i);
            f    = irow[col];
            if (f == 0.0) continue;
            for (j = col; j <= p; j++) irow[j] -= f * prow[j];
        }
        pivot_col[rank] = col;
        if (fit) fit->term[col] = REGRESS_FITTED;
        rank++;
    }

    /* Unscale: the solved vector is in equilibrated units. */
    for (k = 0; k < rank; k++) {
        int c = pivot_col[k];
        beta[c + 1] = row_of(scratch, stride, k)[p] / d[c];
    }

    /* The intercept falls out of the means: never pinned, never competing with
     * a constant regressor. */
    beta[0] = r->my;
    for (i = 0; i < p; i++) beta[0] -= beta[i + 1] * r->mean[i];

    for (i = 0; i <= p; i++)
        if (!isfinite(beta[i])) { debug("regress: coefficient %d is not finite", i); return -1; }

    if (fit) {
        fit->pinned = p - rank;
        fit->sigma_is_bound = 0;
        fit->df     = r->n - (long long)rank - 1;
        fit->condition = (pivmin > 0.0) ? pivmax / pivmin : 1.0;

        /* SSE from centered quantities: the residual is orthogonal to the
         * fitted columns, so SSE = Cyy - b'Cxy. That subtraction cancels as R2
         * nears 1 and no single pass over X'X avoids it; centering removed a
         * different cancellation, against the MEAN. 200 rows of an exact
         * quadratic fitted with a line, x moved out:
         *
         *     x near    reported     truth     --qr
         *     1e4       0.3718       0.3745    0.3745
         *     1e5       0            0.3745    0.3746
         *     1e6       11.57        0.3745    0.3736
         *     1e7       1256         0.3745    0.5018
         *
         * The reported zero claims a perfect fit; the residual file from the
         * same run said 0.3745. The floor under sse is measurable: each y is
         * held to |my|*eps, and Cyy accumulates n of those against deviations
         * of size sqrt(Cyy/n), giving about |my| * eps * sqrt(n * Cyy). Same
         * 200 rows:
         *
         *     x near    |my|      Cyy       sse      true     floor
         *     1e3       5.0e6     6.7e9     444.39   444.39   0.0013
         *     1e4       5.0e8     6.7e11    444.61   444.39   1.28
         *     1e5       5.0e10    6.7e13    0        444.39   1.3e3
         *     1e6       5.0e12    6.7e15    0        444.41   1.3e6
         *
         * Above the floor, sse is printed. Below it, the floor is a true upper
         * bound and is reported instead: `resid SD<2.5`, not `resid SD=0`. --qr
         * carries the residual through the rotation and does not pay this. */
        fit->rss = -1.0;
        fit->sigma = -1.0;
        if (r->cyy > 0.0) {
            double sse = r->cyy;
            for (i = 0; i < p; i++) sse -= beta[i + 1] * r->cxy[i];

            /* An exact fit leaves an SSE of a few ulps, negative as often as
             * positive: rounding, and R2 really is 1. An SSE meaningfully below
             * zero has lost its meaning, and says so rather than clamping. */

            if (sse < -1e-9 * r->cyy) {
                fit->r2 = REGRESS_R2_LOST;
            } else {
                if (sse < 0.0) sse = 0.0;
                /* R2 survives: sse RELATIVE to cyy, so an sse wrong by orders
                 * of magnitude but negligible against cyy still gives an R2
                 * right to the digits printed. */
                fit->r2 = 1.0 - sse / r->cyy;
                if (fit->r2 < 0.0) fit->r2 = 0.0;
                /* TWO error sources in the floor. First, above: |my|*eps per
                 * row, accumulated n times by centring, vanishing when the mean
                 * is near zero. Second: sse is Cyy minus products each of size
                 * Cyy, so the subtraction carries about eps*Cyy however the
                 * response is centred. Summed, not maxed: both are present. */
                {   double floor = fabs(r->my) * DBL_EPSILON
                                  * sqrt((double)r->n * r->cyy)
                                  + DBL_EPSILON * r->cyy;
                    if (sse < floor) { sse = floor; fit->sigma_is_bound = 1; }
                }
                fit->rss = sse;
                /* Divided by the residual freedom, not by n: at df zero the
                 * line passes through every point. */
                if (fit->df > 0) fit->sigma = sqrt(sse / (double)fit->df);
            }
        } else {
            fit->r2 = REGRESS_R2_FLAT_Y;
        }
    }
    return 0;
}
