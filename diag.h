/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* diag.h: read the residuals and say whether the linear model was the wrong
 * choice.
 *
 * A fit reports R2, a residual SD and a conditioning figure. All three are
 * averages over the residuals, so none of them can see structure IN the
 * residuals, and that structure is where "a straight line was the wrong shape"
 * is written. example/curve.csv fits a parabola with a line: R2 and the
 * residual SD look ordinary and every residual is predictable from x.
 *
 * Two checks, both computable in one streaming pass with memory proportional to
 * the number of terms:
 *
 *   CURVATURE     the correlation between the residual and the square of each
 *                 term. For a term entering linearly this is 0. A large value
 *                 names the term whose relationship is bent, which is more
 *                 useful than knowing only that something is wrong.
 *
 *   SPREAD        the correlation between the size of the residual and the
 *                 fitted value. Least squares assumes the error is the same
 *                 size everywhere; when it grows with the prediction, the fit
 *                 is dominated by the largest cases and the residual SD is not
 *                 a typical error for any of them.
 *
 * Neither is a hypothesis test and neither is reported as one. They are
 * correlations with a threshold, meant to prompt a look at the residual file. */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

/* Two guards, both learned from a false positive on example/routes.csv, which
 * is exactly linear:
 *
 * The threshold is the LARGER of this floor and 3/sqrt(n), which is roughly a
 * three-sigma bound on the correlation of unstructured data. At 18 rows that is
 * 0.71, and the -0.44 that routes.csv produced by chance stays quiet; at 200
 * rows the floor governs. A correlation is not evidence until it is larger than
 * what noise produces at that sample size. */
#define DIAG_REPORT 0.4

/* Rows below which nothing is reported: three points can look like anything. */
#define DIAG_MIN_ROWS 12

/* And nothing is reported at all when the residuals are negligible against the
 * response's own spread. routes.csv fits to 1e-7 of an exact answer, so its
 * residuals are rounding error; correlating rounding error with anything is
 * measuring the floating-point unit, not the model. */
#define DIAG_MIN_SHARE 1e-6

struct diag {
    int     nvars;
    long    n;
    double *s;          /* accumulator, diag_storage(nvars) doubles */
    double  resid_sd;   /* from the fit; -1 if not supplied         */
    double  response_sd;
};

struct diag_result {
    long   rows;
    int    curved_term;   /* term whose square best explains the residual, or -1 */
    double curved_r;      /* that correlation, 0 when there is none            */
    double spread_r;      /* |residual| against the fitted value               */
};

size_t diag_storage(int nvars);
int    diag_init(struct diag *d, int nvars, double *storage);

/* One row: its terms, the residual it left, and the value that was predicted. */
void diag_add(struct diag *d, const double *x, double resid, double fitted);

/* The response's own spread, so the checks can tell a residual from rounding.
 * Pass the fit's residual SD and the training response's SD. */
void diag_scale(struct diag *d, double resid_sd, double response_sd);

void diag_result(const struct diag *d, struct diag_result *out);

#endif /* DIAG_H */
