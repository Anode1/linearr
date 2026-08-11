/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* qr.h: the same fit as regress.h, solved without squaring the data first.
 *
 * Forming X'X squares the condition number, so regress.c loses twice the digits
 * it needs to on a near-collinear or badly scaled design: on
 * example/nearly-the-same.csv it returns 2.00002 and 2.99998 for 2 and 3. Here
 * each row is rotated into an upper triangular R by Givens rotations and
 * dropped, so memory stays a function of the model alone. Costs MORE than the
 * normal equations on both counts:
 *
 *                    accumulator      solve workspace     total, p=24
 *     QR             p^2 + 7p + 6     p^2 + 3p + 2        1400 doubles
 *     normal eq.     p^2 + 2p         p^2 + p             1224 doubles
 *
 * Figures from the four storage functions; the extra 14% buys columns that are
 * never squared. The solve workspace re-triangularises the kept columns; not
 * column pivoting, which this does not do -- columns are rotated in arrival
 * order.
 *
 * It does not centre. Each column is judged against its own 2-norm; a dropped
 * column leaves the residual of the model returned, not the rotation's. Not
 * centring costs two things: cond= is not comparable with regress.c's, and the
 * rank test cannot resolve a dependent column past about 1e9 (see qr_solve and
 * QR_RANK_EPS in qr.c). The response is still squared, so a y near 1e160
 * overflows here as in regress.c; see qr_solve.
 *
 * Row-wise updating QR by Givens rotations, the family Gentleman described in
 * 1974, as in R's biglm. Not AS 75 or AS 274: those are the SQUARE-ROOT-FREE
 * form, with a separate weight vector and a unit-diagonal R. This is plain
 * Givens with hypot(). A rotation per column per row against one multiply-add
 * per pair of columns per row: well conditioned the two agree and regress.c is
 * faster, at large cond= this one is right. Choose with --qr. */
#ifndef QR_H
#define QR_H

#include "regress.h"    /* struct regress_fit, REGRESS_MAX_VARS */

#include <stddef.h>

/* A fit in progress. Storage is the CALLER's; R is (p+1) rows of (p+2), the
 * last column holding Q'y. */
struct qr {
    int     nvars;
    long long n;
    double *r;
    double  my, cyy;    /* running mean and centered sum of squares of y */
    double  rss;        /* residual of the ROTATION; only the model's when
                           nothing was dropped                            */
    /* Each column's range, telling a CONSTANT column from a COLLINEAR one. Not
     * inline: that costs REGRESS_MAX_VARS doubles apiece per accumulator. */
    double *colmin;
    double *colmax;
    double *colss;      /* per column: sum of squares relative to colscale,
                           the rank test's divisor                          */
    double *colscale;   /* per column: running largest magnitude, so squaring
                           cannot overflow. A direct sum of squares reaches
                           +inf past about 1.3e154.                         */
};

size_t qr_storage(int nvars);

/* Doubles qr_solve needs to re-triangularise the kept columns of a
 * rank-deficient design. Zero at full rank. */
size_t qr_scratch(int nvars);

/* Start a fit over nvars slopes. Returns 0, or -1 if nvars is out of range or
 * storage is NULL. */
int qr_init(struct qr *q, int nvars, double *storage);

/* Rotate one observation into R and drop it. Returns 0, or -1 if any value is
 * not finite. */
int qr_add(struct qr *q, const double *x, double y);

/* Back-substitute for beta[nvars+1]; beta[0] is the intercept. Fills fit if it
 * is not NULL. scratch must hold qr_scratch(nvars) doubles when the design is
 * rank deficient; at full rank it is untouched and may be NULL. NULL on a
 * deficient design returns -1.
 *
 * fit->condition estimates cond(X), not cond(X'X): largest accepted diagonal of
 * R over smallest. NOT the square root of regress.c's figure -- on Longley
 * regress.c reports 934 and this 1.17e4 -- the two being conditioning figures
 * for different matrices. regress.c centres; this does not, so its number also
 * carries the intercept's near-collinearity with any column far from zero (on
 * Longley a year near 1950, a population near 1.2e8). Accuracy does not follow
 * the ordering: see the Wampler1 figures in tests.c. Compare cond= only across
 * runs of the SAME solver; the summary line says which produced it.
 *
 * Returns 0; -1 if nothing was added or the result is not finite; -3 if the
 * RESPONSE's sums of squares overflowed while being accumulated (a y near
 * 1e160 or beyond). Columns cannot overflow here, which is what colscale buys;
 * both solvers square the response, so rescaling it is the only cure. */
int qr_solve(const struct qr *q, double *beta, double *scratch,
             struct regress_fit *fit);

#endif /* QR_H */
