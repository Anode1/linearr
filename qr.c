/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* qr.c: see qr.h. Givens rotations, one row at a time. */
#include "qr.h"
#include "common.h"

#include <math.h>
#include <string.h>

/* Rank tolerance, relative to the largest diagonal of R. R's diagonals are in
 * the data's own units rather than their squares, so this is a tolerance on
 * cond(X) of about 1e12, which is the same statement regress.c makes with
 * 1e-12 on the squared matrix. */
#define QR_RANK_EPS 1e-12

size_t qr_storage(int nvars) {
    if (nvars < 0) return 0;
    return ((size_t)nvars + 1) * ((size_t)nvars + 2);
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
    q->r     = storage;
    {   int i;
        for (i = 0; i <= nvars; i++) { q->colmin[i] = 0.0; q->colmax[i] = 0.0; }
    }
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

    (void)scratch;                       /* the factor is already triangular */
    if (!q->n || p < 1) return -1;

    for (i = 0; i <= p; i++) beta[i] = 0.0;
    if (fit) for (i = 0; i < p; i++) fit->term[i] = REGRESS_COLLINEAR;

    /* Judge each diagonal against ITS OWN column's scale, so the test is about
     * rank and carries no units. dmax/dmin are then collected on the same
     * scaled footing, which also makes cond= comparable between runs whose
     * columns are measured differently. */
    for (i = 0; i <= p; i++) {
        double mag = fabs(q->colmax[i]) > fabs(q->colmin[i])
                     ? fabs(q->colmax[i]) : fabs(q->colmin[i]);
        double scale = (mag > 0.0) ? mag * sqrt((double)q->n) : 1.0;
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
     * which is the same policy regress.c applies. */
    for (i = p; i >= 0; i--) {
        double v;
        if (!keep[i]) continue;
        v = q->r[(size_t)i * (size_t)w + (size_t)(p + 1)];
        for (j = i + 1; j <= p; j++)
            v -= q->r[(size_t)i * (size_t)w + (size_t)j] * beta[j];
        beta[i] = v / q->r[(size_t)i * (size_t)w + (size_t)i];
        if (fit && i > 0) fit->term[i - 1] = REGRESS_FITTED;
    }

    for (i = 0; i <= p; i++)
        if (!isfinite(beta[i])) { debug("qr: coefficient %d is not finite", i); return -1; }

    if (fit) {
        fit->pinned = (p + 1) - rank;          /* slopes only: the intercept is kept */
        fit->df     = q->n - rank;
        fit->condition = (dmin > 0.0) ? dmax / dmin : 1.0;

        /* q->rss is the residual of the rotation, which used every column
         * INCLUDING the ones just discarded. At full rank that is the model's
         * residual; once a column is dropped it is the residual of a model that
         * was never returned, and reporting it understated the error by fifteen
         * orders of magnitude in a case a reviewer built. Say nothing rather
         * than say that. */
        if (fit->pinned == 0) {
            fit->rss   = q->rss;
            fit->sigma = (fit->df > 0) ? sqrt(q->rss / (double)fit->df) : -1.0;
            fit->r2    = (q->cyy > 0.0) ? 1.0 - q->rss / q->cyy : -1.0;
            if (fit->r2 < 0.0) fit->r2 = 0.0;
        } else {
            fit->rss   = -1.0;
            fit->sigma = -1.0;
            fit->r2    = -1.0;
        }
    }
    return 0;
}
