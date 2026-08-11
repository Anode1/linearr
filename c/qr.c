/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* qr.c: see qr.h. Givens rotations, one row at a time. */
#include "qr.h"
#include "common.h"

#include <math.h>
#include <string.h>

/* Rank tolerance: the fraction of a column that must survive projection onto
 * the columns before it. |R_ii| is what is left of column i, and colss[i] is
 * what it started with, so |R_ii|/||col_i|| is that fraction directly.
 *
 * Compared against the tolerance DIRECTLY. It used to be scaled by the largest
 * surviving diagonal, which made the threshold depend on whichever column
 * happened to survive best.
 *
 * The value is 1e-9 and not R's 1e-7, though R tests the same quantity
 * (LINPACK dqrdc2, tol = 1e-7). R pivots during the factorisation, so its
 * comparison is against a remainder that has already had the strong columns
 * taken out; this factorises in the order the columns arrive, because the rows
 * arrive one at a time and there is no second look. Measured with 1e-7 on that
 * un-pivoted factor, an identifiable column at an offset of 1e8 was deleted
 * where 1e-9 keeps it. Of the two ways to be wrong, keeping a column that
 * should have gone leaves an unstable fit that cond= reports, and deleting one
 * that should have stayed silently zeroes a real coefficient. This errs
 * toward keeping.
 *
 * THE LIMITATION, stated: around 1e9 the test is uninformative in BOTH
 * directions, because the surviving fraction is about 1e-9 whether the column
 * is dependent or not. Do not expect cond= to cover for it: cond is computed
 * over the pivots that were ACCEPTED, so deleting a column removes the
 * evidence. Centre such columns before fitting, or use the normal equations,
 * which centre for you. */
#define QR_RANK_EPS 1e-9

size_t qr_scratch(int nvars) {
    if (nvars < 1) return 0;
    return ((size_t)nvars + 1) * ((size_t)nvars + 2);
}

size_t qr_storage(int nvars) {
    if (nvars < 1) return 0;      /* as regress_storage does; 0 was returning 6 */
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
    /* Compared against the tolerance directly, not against dmax * tolerance.
     * rel[i] is already dimensionless: |R_ii| is what is left of column i once
     * the columns before it are projected out, and the divisor is that
     * column's own norm, so rel is the fraction of the column that survived.
     * That is the quantity LINPACK's dqrdc2 tests, which is what R's lm()
     * uses, and it compares it to tol directly. Scaling by dmax made the
     * threshold depend on whichever column happened to survive best. */
    eps = QR_RANK_EPS;

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

    /* Back substitution.
     *
     * At full rank R is already triangular and this is a back solve.
     *
     * When a column is dropped it is NOT, and the old code solved anyway. It
     * forced a zero residual on every kept row and left v_i on each dropped
     * one, which is the least-squares answer only if those leftovers are
     * orthogonal to the kept columns: sum over dropped i of v_i * R_ij = 0 for
     * every kept j. They are not. When R_ii is a rounding-level residue, the
     * rotation that made it had c near 0 and s near 1 and moved the whole row
     * into row i, z_i and every R_ij to its right with it. On twenty rows with
     * a constant column listed BEFORE an ordinary one that returned an
     * intercept of 3.00755 where least squares gives 2.91, and the error did
     * not shrink with more rows.
     *
     * So the kept columns are re-triangularised here, which is what column
     * pivoting achieves and is cheap because it is O(p^3) on the factor, not
     * on the data. W holds the kept columns of R followed by Q'y; rotating its
     * subdiagonal away leaves a genuine triangular system whose back solve IS
     * the least-squares solution of the reduced model, and whose leftover tail
     * is the extra residual. */
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

    /* The response overflowed. The colscale machinery keeps the COLUMNS from
     * ever being squared, but the response is squared twice over -- cyy for
     * R2 and the rotated-out residual for rss -- so a y near 1e160 sends both
     * to inf while every added row passed isfinite. Unchecked, the fit came
     * back rc=0 reporting resid SD=inf and R2=NaN, and the advice regress.c
     * gives for its own overflow ("use --qr") walked the caller straight into
     * this one. Same refusal, same words, from either solver. */
    if (!isfinite(q->cyy) || !isfinite(q->rss + drop_rss)) {
        debug("qr: the response overflowed its sums of squares (its values "
              "are near 1e160 or beyond); rescale that column");
        return -3;
    }

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
         * the same data with the redundant column removed by hand.
         *
         * WHERE THAT IS STILL WRONG, stated. Both halves describe the LEAST
         * SQUARES solution of the kept columns. When a pivot survives the rank
         * test by a hair, back substitution divides by that near-zero diagonal
         * and the beta returned is not that solution, while this figure still
         * reports the residual the solution would have had. It is then BELOW
         * what any beta can achieve, so it cannot be read as a bound either
         * way. Thirty rows, x1 exactly 3 + (x0-1e9)/2 with x0 near 1e9:
         *
         *     reported by this            rss 57.53   resid SD 1.46
         *     the residual file, same run rss 61.28   resid SD 1.506
         *     the true minimum, exact     rss 58.40   resid SD 1.470
         *     the normal equations        pinned 1    resid SD 1.444
         *
         * The normal equations get it right by pinning: C is symmetric, so a
         * pivot below tolerance also has a near-zero leftover row. The signal
         * here is cond=, which reads 1.16e+08 on that design and trips the
         * ill-conditioning warning, and --residuals writes the residuals the
         * returned line actually leaves. Fixing it needs the rank test to cut
         * where the solve becomes unreliable rather than where the diagonal
         * does, which is a threshold nobody has measured; until then this is
         * one more reason the normal equations are the ones to trust on a
         * rank-deficient design. */
        fit->rss   = q->rss + drop_rss;
        fit->sigma = (fit->df > 0) ? sqrt(fit->rss / (double)fit->df) : -1.0;
        /* Never a bound here, always a value: this residual comes out of the
         * rotation rather than from cancelling two large sums, so there is
         * nothing to floor and nothing to hedge. Said explicitly because the
         * caller declares `struct regress_fit f;` uninitialised and prints '<'
         * or '=' from this field: leaving it unassigned was a read of an
         * indeterminate value that happened to be 0 at -O2, and every QR fit
         * printed 'resid SD<' under -ftrivial-auto-var-init=pattern. */
        fit->sigma_is_bound = 0;
        /* The clamp belongs INSIDE the cyy > 0 branch. Outside it, it ate the
         * -1 that had just been set to mean "the response never varies", and
         * printed R2=0.0000, which reads as "the model explains nothing" where
         * the truth is that R2 is undefined. regress.c has it right. */
        if (q->cyy > 0.0) {
            fit->r2 = 1.0 - fit->rss / q->cyy;
            if (fit->r2 < 0.0) fit->r2 = 0.0;
        } else {
            fit->r2 = REGRESS_R2_FLAT_Y;
        }
    }
    return 0;
}
