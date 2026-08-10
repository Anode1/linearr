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
 * the model alone: (p+1)(p+2) doubles, slightly less than the normal equations
 * need. What it does not do is square anything, so the digits lost are the
 * design's own condition number rather than its square.
 *
 * What it is NOT: a strictly better solver. It does not centre the data, and a
 * reviewer found that without column scaling its rank test deleted a
 * well-identified indicator for being measured in a small unit, which is the
 * same defect regress.c documents as fixed. Columns are scaled here now, and
 * the figures that depend on a dropped column are withheld rather than
 * reported, but the honest summary is that the two solvers are different
 * trade-offs and not an upgrade path.
 *
 * The algorithm is Gentleman's 1974 row-wise updating QR (AS 75 / AS 274), the
 * same method R's biglm has used for two decades. Nothing here is new; the
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
    long    n;
    double *r;
    double  my, cyy;    /* running mean and centered sum of squares of y */
    double  rss;        /* residual of the ROTATION; only the model's when
                           nothing was dropped                            */
    double  colmin[REGRESS_MAX_TERMS + 1];  /* each column's own range, so   */
    double  colmax[REGRESS_MAX_TERMS + 1];  /* the rank test can be relative
                                               to it and a column with no
                                               spread can be told from one
                                               that is merely collinear      */
};

size_t qr_storage(int nvars);

/* Start a fit over nvars slopes. Returns 0, or -1 if nvars is out of range or
 * storage is NULL. */
int qr_init(struct qr *q, int nvars, double *storage);

/* Rotate one observation into R and drop it. Returns 0, or -1 if any value is
 * not finite. */
int qr_add(struct qr *q, const double *x, double y);

/* Back-substitute for beta[nvars+1]; beta[0] is the intercept. Fills fit if it
 * is not NULL. scratch is unused and may be NULL; the parameter is there so the
 * two solvers can be called through the same shape.
 *
 * fit->condition here is an estimate of cond(X), not of cond(X'X): the largest
 * accepted diagonal of R over the smallest. Expect it to be about the square
 * root of the figure regress.c reports for the same data. That is the whole
 * point of the module, so the two numbers are not comparable and the summary
 * says which solver produced it.
 *
 * Returns 0, or -1 if nothing was added or the result is not finite. */
int qr_solve(const struct qr *q, double *beta, double *scratch,
             struct regress_fit *fit);

#endif /* QR_H */
