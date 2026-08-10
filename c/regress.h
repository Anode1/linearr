/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* regress.h: ordinary least squares for y = b0 + b1*x1 + ... + bp*xp.
 *
 * Observations are ADDED and then forgotten: the fitter holds cross-products,
 * never rows, so ten observations and ten million are fitted in the same
 * memory. Peak footprint is a function of the number of terms alone.
 *
 * Three decisions here were paid for by review, and each replaced something
 * that looked fine and gave wrong answers:
 *
 * 1. CENTERED accumulation, not raw X'X. Sums of squares about zero are
 *    differences of large nearly-equal numbers, so a response with an offset
 *    (a price, an epoch timestamp, a population) destroyed R^2 while leaving
 *    the coefficients correct, and always in the flattering direction. With
 *    a 1e8 offset the fit reported R2=1.0000 for a model whose true R2 was 0.
 *    Means and co-moments are updated online (Welford), so the offset cancels
 *    before it can cancel anything else.
 *
 * 2. EQUILIBRATED rank test. The tolerance used to be absolute, taken from the
 *    largest diagonal of X'X, and those diagonals scale as the SQUARE of a
 *    column's units. A column measured in dollars sat 1e12 above an indicator,
 *    so the indicator was declared unidentifiable and silently deleted, however
 *    strong its effect. Whether a term existed depended on whether you wrote
 *    income in dollars or thousands. The matrix is now scaled to unit diagonal
 *    before elimination, which makes the test about rank, as its name says.
 *
 * 3. The intercept is NOT a column. It is recovered from the means afterwards,
 *    so it can never be pinned and a constant regressor is correctly absorbed
 *    into it rather than fighting it for the same degree of freedom.
 *
 * The module allocates nothing. The caller supplies storage, which is what lets
 * one fit live in static memory and a hundred simultaneous fits live in one
 * heap block sized to the actual term count. *
 * WHAT THE RESIDUAL SD COSTS. RSS is recovered here as Syy - beta'Sxy, which is
 * a subtraction of two numbers that agree to more and more places as the fit
 * gets better. The coefficients do not suffer; the residual SD does, in
 * proportion to 1/(1 - R^2). Measured, on one data set at four of six noise
 * levels (tests.c prints all six):
 *
 *      1 - R^2      relative error in the reported residual SD
 *      2.5e-01                 8e-16
 *      3.3e-05                 2e-11
 *      3.3e-09                 2e-07
 *      3.3e-11                 8e-06
 *
 * At R^2 = 0.99999999997 the residual SD is good to five digits, not sixteen.
 *
 * AND THAT TABLE IS THE MILD MECHANISM. It was measured on columns centred near
 * the origin, and it is only the 1/(1 - R^2) part. A column with an OFFSET
 * costs far more, because x[j] - mean[j] is itself a cancelling subtraction:
 * an offset of 1e6 against a spread of 1 loses six digits per row, before any
 * co-moment is formed. Two reviewers measured the same thing independently on
 * an exact quadratic fitted with a line, 200 rows, moving x away from zero:
 *
 *     x near     reported      truth     --qr
 *     1e4        0.3718        0.3745    0.3745
 *     1e5        0             0.3745    0.3746
 *     1e6        11.57         0.3745    0.3736
 *
 * A reported 0 is a claim of a perfect fit, cond= is 1.0 and therefore not
 * even printed, and the residual file written by the same run says 0.3745.
 * This is NOT FIXED; see the note in regress.c. On a response whose spread is
 * small beside its magnitude, use --qr. qr.c does not pay this cost: it
 * carries the residual through the rotation rather than subtracting for it.
 * tests.c holds the measurement as a test, so the property cannot drift
 * unnoticed in either direction.
 *
 */
#ifndef REGRESS_H
#define REGRESS_H

#include <stddef.h>

/* The term ceiling. Override at build time for a small target:
 *     make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32'
 * (Both, together: process.c refuses to compile if the model may be wider than
 * the fitter. Above 510 you must raise CSV_MAX_FIELDS too.) */
#ifndef REGRESS_MAX_VARS
#define REGRESS_MAX_VARS 256                /* slopes, not counting b0 */
#endif
#define REGRESS_MAX_TERMS (REGRESS_MAX_VARS + 1)

/* A fit in progress. The arrays point into storage the CALLER owns; nothing
 * here is allocated or freed by this module. */
struct regress {
    int     nvars;
    long long n;            /* observations added */
    double *mean;           /* nvars: running mean of each regressor */
    double *c;              /* nvars*nvars: centered cross-products, row-major */
    double *cxy;            /* nvars: centered cross-products with y */
    double  my;             /* running mean of the response */
    double  cyy;            /* centered sum of squares of the response */
};

/* How many doubles regress_init needs for nvars terms, and how many
 * regress_solve needs as scratch. Both are O(nvars^2) and neither depends on
 * how much data will be added. */
size_t regress_storage(int nvars);
size_t regress_solve_storage(int nvars);

/* Start a fit over nvars slopes, using storage[regress_storage(nvars)], which
 * must stay alive until the fit is solved. Returns 0, or -1 if nvars is out of
 * range or storage is NULL. */
int regress_init(struct regress *r, int nvars, double *storage);

/* Add one observation: x[nvars] regressors and the response y. Returns 0, or
 * -1 if any value is not finite: a NaN admitted here poisons every
 * coefficient, and used to do so silently, all the way to a published table of
 * "nan" that scored "prediction=nan" and exited 0. */
int regress_add(struct regress *r, const double *x, double y);

/* Why a term carries no coefficient. These are different verdicts with
 * different consequences, and the fitter is the only thing that knows which is
 * which, so it says, rather than leaving both as an indistinguishable 0. */
enum regress_term {
    REGRESS_FITTED = 0,   /* estimated from the data                        */
    REGRESS_CONSTANT,     /* the column never varies: no evidence at all     */
    REGRESS_COLLINEAR     /* its effect is inseparable from another column's;
                             which of the pair keeps the effect is decided by
                             column order, not by the data                   */
};

/* What the fit turned out to be. */
struct regress_fit {
    int    pinned;      /* terms the sample could not identify, set to 0     */
    long long df;       /* residual degrees of freedom: n - (identified + 1) */
    double r2;          /* -1 when it is not defined (a response that never
                           varies) or not computable to useful precision     */
    unsigned char term[REGRESS_MAX_VARS];  /* enum regress_term, per slope */
    double rss;         /* residual sum of squares                           */
    double sigma;       /* residual standard deviation, sqrt(rss/df): the
                           typical distance between a prediction and the
                           truth, in the response's own units. R^2 says how
                           much of the variance was explained, which is a
                           ratio and tells a user consuming a prediction
                           nothing about how wrong it will be. This does.
                           -1 when there is no residual freedom to divide by. */
    double condition;   /* ratio of largest to smallest accepted pivot on the
                           equilibrated matrix: a conditioning proxy. 1.0 is
                           perfect. Past ~1e8 the later digits of the
                           coefficients are noise, and R^2 will not tell you:
                           an ill-conditioned design fits its own sample
                           beautifully. This is the number that says so.     */
};

/* Solve for beta[nvars+1], where beta[0] is the intercept, using
 * scratch[regress_solve_storage(nvars)]. Fills fit if it is not NULL.
 *
 * A term the sample cannot identify (a regressor that never varies, or one
 * collinear with the others) has no least-squares answer, so it is pinned to
 * exactly 0 and the rest are fitted around it. Which of a collinear PAIR gets
 * pinned depends on column order; there is no answer to that question in the
 * data, and fit->pinned is how the caller learns not to read the zero as an
 * estimated effect.
 *
 * Returns 0, or -1 if nothing was added or the fit is not finite. Note what is
 * NOT refused: fewer rows than terms. That is ordinary here (25 terms and 15
 * rows, because a group never sees most interventions), and pinning answers
 * it. Judge that case by fit->df, which goes to zero when the line is passing
 * through every point by construction. */
int regress_solve(const struct regress *r, double *beta, double *scratch,
                  struct regress_fit *fit);

#endif /* REGRESS_H */
