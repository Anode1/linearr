/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* qr.c: see qr.h. Givens rotations, one row at a time. */
#include "qr.h"
#include "common.h"

#include <math.h>
#include <string.h>

/* Rank tolerance on |R_ii|/||col_i||, the fraction of column i surviving
 * projection onto the columns before it. Compared to the tolerance DIRECTLY,
 * not to the largest surviving diagonal times it.
 *
 * 1e-9, not R's 1e-7 on the same quantity (LINPACK dqrdc2, tol = 1e-7): R
 * pivots, testing a remainder with the strong columns already out, while this
 * factorises in arrival order. At 1e-7 here an identifiable column at offset
 * 1e8 was deleted; 1e-9 keeps it. Near 1e9 the test is uninformative BOTH ways:
 * the surviving fraction is about 1e-9 whether the column is dependent or not,
 * and cond=, over ACCEPTED pivots, cannot cover for it. Centre such columns, or
 * use the normal equations. */
#define QR_RANK_EPS 1e-9

size_t qr_scratch(int nvars) {
    if (nvars < 1) return 0;
    return ((size_t)nvars + 1) * ((size_t)nvars + 2);
}

size_t qr_storage(int nvars) {
    if (nvars < 1) return 0;      /* as regress_storage does */
    /* R, the column ranges, the column sums of squares, their scale. */
    return ((size_t)nvars + 1) * ((size_t)nvars + 2) + 4 * ((size_t)nvars + 1);
}

int qr_init(struct qr *q, int nvars, double *storage) {
    if (nvars < 1 || nvars > REGRESS_MAX_VARS || !storage) {
        debug("qr: %d terms is outside 1..%d, or no storage", nvars, REGRESS_MAX_VARS);
        return -1;
    }
    memset(storage, 0, qr_storage(nvars) * sizeof *storage);
    q->nvars = nvars;
    q->n     = 0;
    q->my    = 0.0;
    q->cyy   = 0.0;
    q->rss   = 0.0;
    q->r      = storage;
    q->colmin = storage + ((size_t)nvars + 1) * ((size_t)nvars + 2);
    q->colmax = q->colmin + nvars + 1;
    q->colss    = q->colmax + nvars + 1;
    q->colscale = q->colss + nvars + 1;
    return 0;
}

int qr_add(struct qr *q, const double *x, double y) {
    const int p = q->nvars;
    const int w = p + 2;                 /* columns: intercept, terms, Q'y */
    double row[REGRESS_MAX_TERMS + 1];
    double dy;
    int i, k, j;

    if (!isfinite(y)) { debug("qr: response is not finite"); return -1; }
    for (i = 0; i < p; i++)
        if (!isfinite(x[i])) { debug("qr: term %d is not finite", i + 1); return -1; }

    row[0] = 1.0;                        /* the intercept IS a column here */
    for (i = 0; i < p; i++) row[i + 1] = x[i];
    row[p + 1] = y;

    /* Each column's own scale, so the rank test judges it against itself. */
    for (i = 0; i <= p; i++) {
        if (q->n == 0) { q->colmin[i] = q->colmax[i] = row[i]; }
        else {
            if (row[i] < q->colmin[i]) q->colmin[i] = row[i];
            if (row[i] > q->colmax[i]) q->colmax[i] = row[i];
        }
        /* Scaled so squaring cannot overflow: `colss += row*row` reaches +inf
         * past 1.3e154, and |R_ii|/inf = 0 deletes an identified term as
         * COLLINEAR. */
        {   double a = fabs(row[i]);
            if (a > q->colscale[i]) {
                double t = (q->colscale[i] > 0.0) ? q->colscale[i] / a : 0.0;
                q->colss[i] = q->colss[i] * t * t + 1.0;
                q->colscale[i] = a;
            } else if (a > 0.0) {
                double t = a / q->colscale[i];
                q->colss[i] += t * t;
            }
        }
    }

    /* SST, separate and centered: R holds the fit, not the spread. */
    q->n++;
    dy = y - q->my;
    q->my  += dy / (double)q->n;
    q->cyy += dy * (y - q->my);

    /* Rotate the row into R: each step annihilates one leading element. */
    for (k = 0; k <= p; k++) {
        double a, b, c, s, h;
        if (row[k] == 0.0) continue;
        a = q->r[(size_t)k * (size_t)w + (size_t)k];
        b = row[k];
        h = hypot(a, b);                 /* no square to overflow */
        if (h == 0.0) continue;
        c = a / h;
        s = b / h;
        q->r[(size_t)k * (size_t)w + (size_t)k] = h;
        row[k] = 0.0;
        for (j = k + 1; j < w; j++) {
            double t1 = q->r[(size_t)k * (size_t)w + (size_t)j];
            double t2 = row[j];
            q->r[(size_t)k * (size_t)w + (size_t)j] =  c * t1 + s * t2;
            row[j]                                  = -s * t1 + c * t2;
        }
    }
    /* What is left in the response column is this row's residual. */
    q->rss += row[p + 1] * row[p + 1];
    return 0;
}

int qr_solve(const struct qr *q, double *beta, double *scratch,
             struct regress_fit *fit) {
    const int p = q->nvars;
    const int w = p + 2;
    double dmax = 0.0, dmin = 0.0;
    double rel[REGRESS_MAX_TERMS + 1];
    int    keep[REGRESS_MAX_TERMS + 1];
    int    i, j, rank = 0;
    double eps;
    double drop_rss = 0.0;

    if (!q->n || p < 1) return -1;

    for (i = 0; i <= p; i++) beta[i] = 0.0;
    if (fit) for (i = 0; i < p; i++) fit->term[i] = REGRESS_COLLINEAR;

    /* Judge each diagonal against ITS OWN column's 2-norm -- equilibrating to
     * unit columns -- so the test carries no units and cond= is comparable
     * between runs whose columns are measured differently. */
    for (i = 0; i <= p; i++) {
        double scale = (q->colss[i] > 0.0)
                       ? q->colscale[i] * sqrt(q->colss[i]) : 1.0;
        double d = fabs(q->r[(size_t)i * (size_t)w + (size_t)i]) / scale;
        rel[i] = d;
        if (d > dmax) dmax = d;
    }
    /* rel[i] is dimensionless: compared to the tolerance directly, not to
     * dmax * tolerance. See QR_RANK_EPS. */
    eps = QR_RANK_EPS;

    for (i = 0; i <= p; i++) {
        /* The intercept is never dropped: another model, not a rank finding. */
        keep[i] = (i == 0) || (rel[i] > eps);
        if (keep[i]) {
            rank++;
            if (dmin == 0.0 || rel[i] < dmin) dmin = rel[i];
        } else if (fit) {
            /* No spread of its own is CONSTANT; spread but no diagonal left is
             * COLLINEAR, as in regress.c. */
            fit->term[i - 1] = (q->colmax[i] == q->colmin[i]) ? REGRESS_CONSTANT
                                                              : REGRESS_COLLINEAR;
        }
    }

    /* Back substitution. At full rank R is triangular. With a column dropped it
     * is not, and the leftovers on the dropped rows are not orthogonal to the
     * kept columns, so the kept columns are re-triangularised here -- what
     * column pivoting achieves, at O(p^3) on the factor, not on the data. W
     * holds the kept columns of R then Q'y; rotating its subdiagonal away
     * leaves a triangular system, back-solved, the tail extra residual. */
    if (rank == p + 1) {
        for (i = p; i >= 0; i--) {
            double v = q->r[(size_t)i * (size_t)w + (size_t)(p + 1)];
            for (j = i + 1; j <= p; j++)
                v -= q->r[(size_t)i * (size_t)w + (size_t)j] * beta[j];
            beta[i] = v / q->r[(size_t)i * (size_t)w + (size_t)i];
            if (fit && i > 0) fit->term[i - 1] = REGRESS_FITTED;
        }
    } else {
        int    col[REGRESS_MAX_TERMS + 1];
        int    k = 0, cw;
        double *W = scratch;

        if (!W) { debug("qr: a rank-deficient solve needs scratch"); return -1; }
        for (i = 0; i <= p; i++) if (keep[i]) col[k++] = i;
        cw = k + 1;                                  /* kept columns, then Q'y */

        for (i = 0; i <= p; i++) {
            for (j = 0; j < k; j++)
                W[(size_t)i * (size_t)cw + (size_t)j] =
                    q->r[(size_t)i * (size_t)w + (size_t)col[j]];
            W[(size_t)i * (size_t)cw + (size_t)k] =
                q->r[(size_t)i * (size_t)w + (size_t)(p + 1)];
        }

        for (j = 0; j < k; j++) {
            for (i = p; i > j; i--) {
                double a = W[(size_t)j * (size_t)cw + (size_t)j];
                double b = W[(size_t)i * (size_t)cw + (size_t)j];
                double r, c, sn;
                int    m;
                if (b == 0.0) continue;
                r = hypot(a, b);
                c = a / r; sn = b / r;
                for (m = j; m < cw; m++) {
                    double u = W[(size_t)j * (size_t)cw + (size_t)m];
                    double v = W[(size_t)i * (size_t)cw + (size_t)m];
                    W[(size_t)j * (size_t)cw + (size_t)m] =  c * u + sn * v;
                    W[(size_t)i * (size_t)cw + (size_t)m] = -sn * u + c * v;
                }
            }
        }

        for (i = k - 1; i >= 0; i--) {
            double v = W[(size_t)i * (size_t)cw + (size_t)k];
            for (j = i + 1; j < k; j++)
                v -= W[(size_t)i * (size_t)cw + (size_t)j] * beta[col[j]];
            beta[col[i]] = v / W[(size_t)i * (size_t)cw + (size_t)i];
            if (fit && col[i] > 0) fit->term[col[i] - 1] = REGRESS_FITTED;
        }
        for (i = k; i <= p; i++) {
            double v = W[(size_t)i * (size_t)cw + (size_t)k];
            drop_rss += v * v;
        }
    }

    for (i = 0; i <= p; i++)
        if (!isfinite(beta[i])) { debug("qr: coefficient %d is not finite", i); return -1; }

    /* colscale spares the COLUMNS, but the response is squared twice -- cyy for
     * R2, the rotated-out residual for rss -- so a y near 1e160 sends both to
     * inf while every row passed isfinite. */
    if (!isfinite(q->cyy) || !isfinite(q->rss + drop_rss)) {
        debug("qr: the response overflowed its sums of squares (its values "
              "are near 1e160 or beyond); rescale that column");
        return -3;
    }

    if (fit) {
        fit->pinned = (p + 1) - rank;          /* slopes only: the intercept is kept */
        fit->df     = q->n - rank;
        fit->condition = (dmin > 0.0) ? dmax / dmin : 1.0;

        /* q->rss is the ROTATION's residual, over every column including any
         * discarded; at full rank that is the model's. With a column dropped,
         * each dropped row is left missing by v = z_i - sum over j > i of
         * R_ij beta_j, orthogonal to what the rotation discarded, so exactly
         *
         *     RSS = (residual of the rotation) + sum of v^2 over dropped rows
         *
         * tests.c checks that against the same data with the redundant column
         * removed by hand. Still wrong when a pivot survives the rank test by a
         * hair: q->rss has lost digits through a rotation whose pivot was at
         * rounding level, about 15% of its value below, so the figure falls
         * BELOW what any beta can achieve and bounds nothing either way. Thirty
         * rows, x1 exactly 3 + (x0-1e9)/2, x0 near 1e9:
         *
         *     reported by this            rss 57.53   resid SD 1.46
         *     the residual file, same run rss 61.28   resid SD 1.506
         *     the true minimum, exact     rss 58.40   resid SD 1.470
         *     the normal equations        pinned 1    resid SD 1.444
         *
         * The normal equations pin instead: C is symmetric, so a pivot below
         * tolerance has a near-zero leftover row too. Signals: cond=, 1.16e+08
         * here and past the ill-conditioning warning, and --residuals. A fix
         * needs the rank test to cut where the solve turns unreliable. */
        fit->rss   = q->rss + drop_rss;
        fit->sigma = (fit->df > 0) ? sqrt(fit->rss / (double)fit->df) : -1.0;
        /* Always a value, never a bound: the residual comes out of the
         * rotation, not from cancelling two large sums. Assigned explicitly
         * because the caller leaves `struct regress_fit f;` uninitialised. */
        fit->sigma_is_bound = 0;
        /* The clamp belongs INSIDE the cyy > 0 branch: outside, it would eat
         * the -1 for "the response never varies" and print R2=0.0000 where R2
         * is undefined. */
        if (q->cyy > 0.0) {
            fit->r2 = 1.0 - fit->rss / q->cyy;
            if (fit->r2 < 0.0) fit->r2 = 0.0;
        } else {
            fit->r2 = REGRESS_R2_FLAT_Y;
        }
    }
    return 0;
}
