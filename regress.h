/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* regress.h -- ordinary least squares for y = b0 + b1*x1 + ... + bp*xp, by
 * accumulated normal equations.
 *
 * The point of this shape: an observation is ADDED and then forgotten. The
 * fitter holds the (p+1)x(p+1) cross-product matrix and nothing else, so a
 * training file of ten rows and one of ten million rows are fitted in the same
 * memory -- peak footprint is a function of the number of variables, never of
 * the number of rows (STYLE.md's bounded-memory invariant, on the arithmetic
 * rather than on the store). Nothing here allocates. */
#ifndef REGRESS_H
#define REGRESS_H

#define REGRESS_MAX_VARS 32                 /* slopes, not counting b0        */
#define REGRESS_MAX_TERMS (REGRESS_MAX_VARS + 1)

struct regress {
    int    nvars;                           /* p: slopes, excluding intercept */
    long   n;                               /* observations added             */
    double xtx[REGRESS_MAX_TERMS][REGRESS_MAX_TERMS];  /* X'X, term 0 is the 1 */
    double xty[REGRESS_MAX_TERMS];                     /* X'y                  */
    double sy, syy;                         /* for the R^2 report             */
};

/* Start a fit over nvars slopes. Returns 0, or -1 if nvars is out of range. */
int regress_init(struct regress *r, int nvars);

/* Add one observation: x[nvars] regressors and the response y. Returns 0. */
int regress_add(struct regress *r, const double *x, double y);

/* Solve for beta[nvars+1] (beta[0] is the intercept), by Gauss-Jordan with
 * partial pivoting on the normal equations.
 *
 * A term the sample cannot identify -- a regressor that never varies, or one
 * collinear with the others -- has no least-squares answer at all, and the
 * matrix is singular there. Rather than fail the whole fit or return a number
 * the data does not support, such a term is pinned to exactly 0 and the rest
 * are fitted around it. That is the honest reading (this indicator contributes
 * nothing we can see) and it matches the shipped tables, where an intervention
 * a group never receives carries a zero coefficient.
 *
 * Returns the number of terms pinned that way (0 when the fit is full rank),
 * or -1 if nothing was added.
 *
 * Note what this does NOT refuse: a sample with fewer rows than terms. That is
 * the ordinary case here -- 25 terms and 15 rows, because a group never sees
 * most of the 24 interventions -- and pinning is exactly the right answer to
 * it. The fit is only meaningless when the rows do not outnumber the terms the
 * sample DID identify, which the caller can see: rows minus (nvars + 1 - the
 * return value) is the residual degrees of freedom, and at zero the line passes
 * through every point by construction and R^2 is 1 whatever the data says. */
int regress_solve(const struct regress *r, double *beta);

/* Coefficient of determination for a solved beta, in [0,1] for a fit with an
 * intercept. Returns -1 if the response never varies (R^2 is undefined). */
double regress_r2(const struct regress *r, const double *beta);

#endif /* REGRESS_H */
