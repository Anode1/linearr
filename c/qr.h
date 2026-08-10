/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* qr.h: the same fit as regress.h, solved without squaring the data first.
 *
 * regress.c accumulates X'X. That is what bounds its memory, and it costs
 * conditioning: forming the cross-products squares the condition number of the
 * design, so a near-collinear or badly scaled problem loses roughly twice the
 * digits it needs to. On example/nearly-the-same.csv it returns 2.00002 and
 * 2.99998 where the answer is 2 and 3.
 *
 * This module reaches the same answer by rotating each row into an upper
 * triangular factor R with Givens rotations, one row at a time. The row is used
 * and dropped, exactly as in regress.c, so the memory is still a function of
 * the model alone: p^2 + 7p + 6 doubles, against the normal equations' p^2 +
 * 2p. The ACCUMULATOR is 5p + 6 doubles more, not less. What a caller
 * allocates is the other way round, because regress_solve needs a p^2 + p
 * elimination workspace and this does not: 750 doubles against 1224 at p=24. This header said less, and the
 * README said more, and neither counted the three per-column vectors this
 * module keeps; the figures now come from qr_storage() and regress_storage(),
 * which are what the callers allocate. What it does not do is square anything, so the digits lost are the
 * design's own condition number rather than its square.
 *
 * What it is NOT: a strictly better solver. It does not centre the data, and a
 * reviewer found that without column scaling its rank test deleted a
 * well-identified indicator for being measured in a small unit, which is the
 * same defect regress.c documents as fixed. Each column is now judged against
 * its own 2-norm, and when a column is dropped the residual of the model that
 * was actually returned is computed rather than the rotation's, so R2 and the
 * residual SD mean what they say in that case too.
 *
 * What remains is that this module does not centre, and two things follow from
 * it. Its cond= is not comparable with regress.c's (see qr_solve below), and
 * its rank test needs a looser tolerance and still cannot resolve a dependent
 * column once the data sits near 1e9 (see QR_RANK_EPS in qr.c, where the
 * measurement is). The summary is that the two solvers are different
 * trade-offs and not an upgrade path.
 *
 * The algorithm is row-wise updating QR by Givens rotations, the family
 * Gentleman described in 1974 and the same shape R's biglm has used for two
 * decades. Not AS 75 or AS 274 themselves: those are the SQUARE-ROOT-FREE
 * form, carrying a separate weight vector and a unit-diagonal R. This is plain
 * Givens with hypot(), which is simpler to read and does the same job. Nothing here is new; the
 * packaging is.
 *
 * The cost is arithmetic: a rotation per column per row, against one
 * multiply-add per pair of columns per row. For a well-conditioned design the
 * two agree and regress.c is faster; when cond= is large this one is right.
 * Choose with --qr, or leave it and the fit summary will tell you when it
 * matters. */
#ifndef QR_H
#define QR_H

#include "regress.h"    /* struct regress_fit, REGRESS_MAX_VARS */

#include <stddef.h>

/* A fit in progress. The array points into storage the CALLER owns; nothing
 * here allocates. R is (p+1) rows of (p+2), the last column holding Q'y. */
struct qr {
    int     nvars;
    long long n;
    double *r;
    double  my, cyy;    /* running mean and centered sum of squares of y */
    double  rss;        /* residual of the ROTATION; only the model's when
                           nothing was dropped                            */
    /* Each column's own range, so the rank test can be relative to it and a
     * column with no spread can be told from one that is merely collinear.
     * These point into the caller's storage rather than sitting inline: inline
     * they made struct qr 4 KB, and since one of these lives in every group's
     * accumulator, a two-term model paid four kilobytes a group for two
     * columns' worth of information. */
    double *colmin;
    double *colmax;
    /* Each column's sum of squares, so the rank test can divide a diagonal by
     * its own column's 2-norm. The range is kept as well, but only to tell a
     * CONSTANT column from a COLLINEAR one, which are different verdicts. */
    double *colss;
    /* The magnitude colss is relative to, so squaring cannot overflow: a
     * column past about 1.3e154 used to send the sum to +inf and delete a
     * well-identified term. */
    double *colscale;
};

size_t qr_storage(int nvars);

/* Doubles qr_solve needs to re-triangularise the kept columns of a
 * rank-deficient design. Zero work and zero space at full rank. */
size_t qr_scratch(int nvars);

/* Start a fit over nvars slopes. Returns 0, or -1 if nvars is out of range or
 * storage is NULL. */
int qr_init(struct qr *q, int nvars, double *storage);

/* Rotate one observation into R and drop it. Returns 0, or -1 if any value is
 * not finite. */
int qr_add(struct qr *q, const double *x, double y);

/* Back-substitute for beta[nvars+1]; beta[0] is the intercept. Fills fit if it
 * is not NULL.
 *
 * scratch must hold qr_scratch(nvars) doubles when the design turns out to be
 * rank deficient, because the kept columns are re-triangularised there before
 * the solve; at full rank it is untouched and may be NULL. Passing NULL to a
 * deficient design returns -1 rather than the wrong answer it used to give.
 *
 * fit->condition here is an estimate of cond(X), not of cond(X'X): the largest
 * accepted diagonal of R over the smallest.
 *
 * Do NOT read it as the square root of the figure regress.c reports. This
 * header said so for a while and the certified sets say otherwise: on Longley
 * regress.c reports 934 and this reports 1.17e4, the larger of the two. There
 * is no contradiction, because they are conditioning figures for different
 * matrices. regress.c centres its co-moments, so its number describes the
 * columns about their own means, with the intercept already accounted for.
 * This module does not centre, so its number still carries the intercept's
 * near-collinearity with any column that lives far from zero, which on Longley
 * is a year near 1950 and a population near 1.2e8. The accuracy of the two
 * fits does not follow the ordering of the two numbers; see the Wampler1
 * figures in tests.c, where this solver is right and the other is not.
 *
 * So: compare a cond= against other runs of the SAME solver, not across the
 * two. The summary line says which produced it for that reason.
 *
 * Returns 0, or -1 if nothing was added or the result is not finite. */
int qr_solve(const struct qr *q, double *beta, double *scratch,
             struct regress_fit *fit);

#endif /* QR_H */
