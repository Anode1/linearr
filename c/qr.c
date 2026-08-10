/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* qr.c: see qr.h. Givens rotations, one row at a time. */
#include "qr.h"
#include "common.h"

#include <math.h>
#include <string.h>

/* Rank tolerance, relative to the largest diagonal of R after each column is
 * divided by its own 2-norm.
 *
 * It is 1e-9 and NOT the 1e-12 regress.c uses, and the difference is not an
 * oversight in either file. regress.c centres its co-moments, so by the time it
 * tests rank the intercept is out of the way and the columns are about their
 * own means. This module does not centre -- that is what makes it a one-pass
 * QR -- so a column sitting far from zero is numerically close to the intercept
 * column, and the factorisation loses digits in proportion to how far out it
 * sits. Below 1e-9 those lost digits look like rank.
 *
 * Measured, on a design of two columns where the second is exactly a linear
 * function of the first, so the right answer is always "drop one":
 *
 *     offset of the columns     1e-12    1e-10    1e-9    1e-8    1e-7
 *              0                 drop     drop     drop    drop    drop
 *              1e3               drop     drop     drop    drop    drop
 *              1e6               MISSED   drop     drop    drop    drop
 *              1e9               MISSED   MISSED   MISSED  MISSED  MISSED
 *
 * and at 1e-8 and looser a design whose columns are genuinely independent
 * starts being pinned instead, so there is no threshold that rescues 1e9.
 *
 * THE LIMITATION, stated: with columns near 1e9 this module cannot tell an
 * exactly dependent column from an independent one. cond= still reports the
 * ill-conditioning, so the fit is not silent about it, but the rank test does
 * not cut. Centre such columns before fitting, or use the normal equations,
 * which centre for you. */
#define QR_RANK_EPS 1e-9

size_t qr_storage(int nvars) {
    if (nvars < 0) return 0;
    /* R, then the column-range vectors that used to sit inside struct qr, then
     * each column's sum of squares for the rank test, and the scale that sum
     * is taken relative to. */
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

    /* Each column's own scale, accumulated as we go. Without it the rank test
     * below compares a column against the LARGEST column's magnitude, so a term
     * in small units is deleted for being small: the identical defect regress.c
     * documents as fixed, reintroduced here. A reviewer produced a case where
     * an indicator worth 5 was deleted beside a column of size 1e15, and the
     * fit then reported R2=1.0000 for a model whose residuals were 4.0. */
    for (i = 0; i <= p; i++) {
        if (q->n == 0) { q->colmin[i] = q->colmax[i] = row[i]; }
        else {
            if (row[i] < q->colmin[i]) q->colmin[i] = row[i];
            if (row[i] > q->colmax[i]) q->colmax[i] = row[i];
        }
        /* The column's 2-norm, accumulated so that squaring cannot overflow.
         * `colss += row*row` reaches +inf for any column past about 1.3e154,
         * the scaled diagonal is then |R_ii|/inf = 0, and a perfectly
         * identified term is deleted and reported COLLINEAR. The rotation two
         * screens below already uses hypot() to avoid exactly this, and this
         * loop squared directly.
         *
         * colmax[] holds the running largest magnitude and colss[] the sum of
         * squares relative to it, rescaled when a larger value arrives. */
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

    /* SST, kept separately and centered, because R holds the fit and not the
     * spread of the response. */
    q->n++;
    dy = y - q->my;
    q->my  += dy / (double)q->n;
    q->cyy += dy * (y - q->my);

    /* Rotate the row into R. Each step annihilates one leading element of the
     * row against the corresponding row of R, and the row is finished with. */
    for (k = 0; k <= p; k++) {
        double a, b, c, s, h;
        if (row[k] == 0.0) continue;
        a = q->r[(size_t)k * (size_t)w + (size_t)k];
        b = row[k];
        h = hypot(a, b);                 /* hypot, so a large a or b cannot
                                            overflow the intermediate square */
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
    /* Whatever is left in the response column could not be explained by any
     * combination of the columns: it is this row's contribution to the residual,
     * available without a second pass over the data. */
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

    (void)scratch;                       /* the factor is already triangular */
    if (!q->n || p < 1) return -1;

    for (i = 0; i <= p; i++) beta[i] = 0.0;
    if (fit) for (i = 0; i < p; i++) fit->term[i] = REGRESS_COLLINEAR;

    /* Judge each diagonal against ITS OWN column's scale, so the test is about
     * rank and carries no units. dmax/dmin are then collected on the same
     * scaled footing, which also makes cond= comparable between runs whose
     * columns are measured differently. */
    for (i = 0; i <= p; i++) {
        /* The column's own 2-norm. This used to be the largest absolute value
         * in the column times sqrt(n), which is not the column's size: for a
         * column sitting at 1e6 and varying by 1, it returns 1e6, and the
         * diagonal it divides is about 1. The term was then deleted for being
         * collinear when it was merely offset, and the offsets that do this
         * (a year, a price, a temperature in Kelvin) are ordinary.
         *
         * The 2-norm is also the right scale for THIS factorisation and not
         * merely a better one: the module does not centre, so what it factors
         * is the raw column, and dividing each diagonal by its column's norm is
         * exactly equilibrating the design to unit columns before asking about
         * rank. */
        double scale = (q->colss[i] > 0.0)
                       ? q->colscale[i] * sqrt(q->colss[i]) : 1.0;
        double d = fabs(q->r[(size_t)i * (size_t)w + (size_t)i]) / scale;
        rel[i] = d;
        if (d > dmax) dmax = d;
    }
    eps = (dmax > 0.0 ? dmax : 1.0) * QR_RANK_EPS;

    for (i = 0; i <= p; i++) {
        /* The intercept is never dropped. Dropping it silently set beta[0] to 0
         * with nothing naming it, and a model without an intercept is a
         * different model, not a rank finding. */
        keep[i] = (i == 0) || (rel[i] > eps);
        if (keep[i]) {
            rank++;
            if (dmin == 0.0 || rel[i] < dmin) dmin = rel[i];
        } else if (fit) {
            /* A column with no spread of its own is CONSTANT; one that had
             * spread and still lost its diagonal is COLLINEAR with another.
             * These are different verdicts and regress.c already says so. */
            fit->term[i - 1] = (q->colmax[i] == q->colmin[i]) ? REGRESS_CONSTANT
                                                              : REGRESS_COLLINEAR;
        }
    }

    /* Back substitution over the kept columns. A dropped column keeps its 0,
     * which is the same policy regress.c applies.
     *
     * A dropped row's equation is then NOT satisfied, and the amount by which
     * it is missed is collected here. Descending order is what makes this
     * possible in one pass: by the time row i is reached, every beta[j] for
     * j > i is final.
     *
     * KNOWN WRONG WHEN A DROPPED COLUMN IS NOT THE LAST ONE. Back-substitution
     * forces a zero residual on every kept row and leaves v_i on each dropped
     * row. For the result to be the least-squares solution of the reduced
     * model, those leftovers must be orthogonal to the kept columns, that is
     * sum over dropped i of v_i * R_ij = 0 for every kept j. They are not:
     * when R_ii is a rounding-level residue, the rotation that produced it had
     * c near 0 and s near 1 and moved the whole row, z_i and every R_ij for
     * j > i, into row i.
     *
     * Twenty rows, a constant column listed BEFORE an ordinary one:
     *
     *     least squares   intercept 2.91      slope 2.004736842
     *     this            intercept 3.00755   slope 1.99447
     *
     * The intercept is 3.4% out and it does not shrink with more rows: it is a
     * bias, not rounding. The RSS reconstructed below is the true residual of
     * that beta, so nothing here notices.
     *
     * The fix is column pivoting, so a dropped column is always last, which is
     * what LINPACK's dqrdc2 and R's lm() do. Until then the normal equations
     * are the ones to trust on a rank-deficient design: C is symmetric, so a
     * column whose pivot falls below the tolerance also has a near-zero
     * leftover row, and its pinned solution really is the reduced one.
     *
     * tests.c did not catch this because its rank-deficient case puts the
     * redundant column last, where no j > i exists and the answer is exact. */
    for (i = p; i >= 0; i--) {
        double v = q->r[(size_t)i * (size_t)w + (size_t)(p + 1)];
        for (j = i + 1; j <= p; j++)
            v -= q->r[(size_t)i * (size_t)w + (size_t)j] * beta[j];
        if (!keep[i]) { drop_rss += v * v; continue; }
        beta[i] = v / q->r[(size_t)i * (size_t)w + (size_t)i];
        if (fit && i > 0) fit->term[i - 1] = REGRESS_FITTED;
    }

    for (i = 0; i <= p; i++)
        if (!isfinite(beta[i])) { debug("qr: coefficient %d is not finite", i); return -1; }

    if (fit) {
        fit->pinned = (p + 1) - rank;          /* slopes only: the intercept is kept */
        fit->df     = q->n - rank;
        fit->condition = (dmin > 0.0) ? dmax / dmin : 1.0;

        /* q->rss is the residual of the ROTATION, which used every column
         * including the ones just discarded. At full rank that is the model's
         * residual. Once a column is dropped it is the residual of a model that
         * was never returned, and reporting it understated the error by fifteen
         * orders of magnitude in a case a reviewer built.
         *
         * This used to be answered by withholding the three figures, which is
         * not the smaller of the two evils: a fit that reports no residual SD
         * and no R2 is a fit nobody can judge, and every caller then had to
         * carry a special case. The residual of the model actually returned is
         * available for the cost of the loop above. Writing R beta = z, back
         * substitution satisfies every KEPT row exactly and leaves each dropped
         * row missing by v = z_i - sum over j > i of R_ij beta_j, since the
         * dropped beta_i is 0. Those misses are orthogonal to what the rotation
         * already discarded, so they add:
         *
         *     RSS = (residual of the rotation) + sum of v^2 over dropped rows
         *
         * which is exact, not an estimate. tests.c checks it against a fit of
         * the same data with the redundant column removed by hand. */
        fit->rss   = q->rss + drop_rss;
        fit->sigma = (fit->df > 0) ? sqrt(fit->rss / (double)fit->df) : -1.0;
        fit->r2    = (q->cyy > 0.0) ? 1.0 - fit->rss / q->cyy : -1.0;
        if (fit->r2 < 0.0) fit->r2 = 0.0;
    }
    return 0;
}
