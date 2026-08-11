/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* regress.h: ordinary least squares for y = b0 + b1*x1 + ... + bp*xp.
 *
 * Observations are added and then forgotten: cross-products are held, never
 * rows, so peak footprint is a function of the term count alone.
 *
 * Three decisions:
 * 1. CENTERED accumulation, not raw X'X: raw sums about zero cancel, reporting
 *    R2=1.0000 at an offset of 1e8 where the true R^2 is 0, coefficients
 *    unharmed. Means and co-moments updated online (Welford).
 * 2. EQUILIBRATED rank test: X'X diagonals scale as the SQUARE of a column's
 *    units, dollars sitting 1e12 above an indicator, so the matrix is scaled
 *    to unit diagonal before elimination.
 * 3. The intercept is NOT a column: recovered from the means, so it cannot be
 *    pinned and a constant regressor is absorbed into it.
 *
 * rss = Syy - beta'Sxy cancels as R^2 -> 1: relative error in the residual SD
 * ~ 1/(1 - R^2). Coefficients unaffected. Measured (tests.c has all six):
 *
 *      1 - R^2      relative error in the reported residual SD
 *      2.5e-01                 8e-16
 *      3.3e-05                 2e-11
 *      3.3e-09                 2e-07
 *      3.3e-11                 8e-06
 *
 * The mild case: columns centred near the origin. An OFFSET column costs more,
 * x[j] - mean[j] cancelling too -- six digits per row at offset 1e6 against
 * spread 1 -- and far out sse reports 0, so an upper bound is printed instead,
 * `resid SD<1.799`. Measurements and floor in regress.c. --qr does not pay this
 * and suits a response whose spread is small beside its magnitude. */
#ifndef REGRESS_H
#define REGRESS_H

#include <stddef.h>

/* The term ceiling. Override at build time for a small target:
 *     make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32'
 * Both together: process.c refuses to compile if the model may be wider than
 * the fitter. Above 510, raise CSV_MAX_FIELDS too. */
#ifndef REGRESS_MAX_VARS
#define REGRESS_MAX_VARS 256                /* slopes, not counting b0 */
#endif
#define REGRESS_MAX_TERMS (REGRESS_MAX_VARS + 1)

/* A fit in progress. The arrays point into storage the CALLER owns; this module
 * allocates nothing. */
struct regress {
    int     nvars;
    long long n;            /* observations added */
    double *mean;           /* nvars: running mean of each regressor */
    double *c;              /* nvars*nvars: centered cross-products, row-major */
    double *cxy;            /* nvars: centered cross-products with y */
    double  my;             /* running mean of the response */
    double  cyy;            /* centered sum of squares of the response */
};

/* Doubles regress_init needs for nvars terms, and doubles regress_solve needs
 * as scratch. Both O(nvars^2), neither depending on how much data is added. */
size_t regress_storage(int nvars);
size_t regress_solve_storage(int nvars);

/* Start a fit over nvars slopes on storage[regress_storage(nvars)], which must
 * stay alive until the fit is solved. Returns 0, or -1 if nvars is out of
 * range or storage is NULL. */
int regress_init(struct regress *r, int nvars, double *storage);

/* Add one observation: x[nvars] regressors and the response y. Returns 0, or
 * -1 if any value is not finite. */
int regress_add(struct regress *r, const double *x, double y);

/* Why a term carries no coefficient: distinct verdicts, not one shared 0. */
enum regress_term {
    REGRESS_FITTED = 0,   /* estimated from the data                        */
    REGRESS_CONSTANT,     /* the column never varies: no evidence at all     */
    REGRESS_COLLINEAR     /* inseparable from another column; which of the pair
                             keeps the effect follows column order, not data  */
};

struct regress_fit {
    int    pinned;      /* terms the sample could not identify, set to 0     */
    int    sigma_is_bound;  /* sse fell below what the subtraction can resolve:
                               rss and sigma are an UPPER BOUND, printed `<` */
    long long df;       /* residual degrees of freedom: n - (identified + 1) */
    /* R2, or a sentinel below: a property of the data, or lost digits. */
    double r2;
#define REGRESS_R2_FLAT_Y (-1.0) /* the response never varies: R2 is 0/0, and
                                    undefined rather than zero               */
#define REGRESS_R2_LOST   (-2.0) /* cyy - b'cxy came out meaningfully below
                                    zero, no digits left: rescale, not a bad
                                    fit                                      */
    unsigned char term[REGRESS_MAX_VARS];  /* enum regress_term, per slope */
    double rss;         /* residual sum of squares                           */
    double sigma;       /* residual SD, sqrt(rss/df): typical distance from
                           prediction to truth in the response's own units,
                           which R^2, a ratio, does not give. -1 with no
                           residual df                                       */
    double condition;   /* largest over smallest accepted pivot on the
                           equilibrated matrix: a conditioning proxy, 1.0
                           perfect. Past ~1e8 the later coefficient digits are
                           noise, and R^2 will not show it                    */
};

/* Solve for beta[nvars+1], where beta[0] is the intercept, using
 * scratch[regress_solve_storage(nvars)]. Fills fit if it is not NULL.
 *
 * A term the sample cannot identify (never varies, or collinear with the
 * others) has no least-squares answer: it is pinned to exactly 0, the rest
 * fitted around it. fit->pinned and fit->term tell that 0 from an estimate. NOT
 * refused: fewer rows than terms, which is ordinary here and answered by
 * pinning. Judge it by fit->df, zero when the line passes through every point
 * by construction.
 *
 * Returns 0; -1 if nothing was added or the fit is not finite; -2 if a term's
 * cross-products overflowed while being accumulated (a column near 1e160 or
 * beyond: rescale it, or use the QR, which does not square the columns); -3 if
 * the RESPONSE's sums overflowed the same way, where the QR is no remedy and
 * rescaling is the only cure. */
int regress_solve(const struct regress *r, double *beta, double *scratch,
                  struct regress_fit *fit);

#endif /* REGRESS_H */
