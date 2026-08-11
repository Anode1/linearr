/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* diag.h: read the residuals and say whether a straight line was the wrong
 * choice. R2, residual SD and conditioning summarise the residuals; none sees
 * structure IN them. Three probes do, streaming, memory ~ term count.
 *
 * CURVATURE, per term: the residual against the part of x^2 that 1 and x do not
 * explain, and of x^3 on 1, x, x^2, about each column's centre. Raw x^2 would
 * mostly measure what x accounts for -- same quadratic, origin moved:
 *
 *       offset   corr(r, x^2)   corr(r, x^2 partialled)
 *          0        0.204              0.794
 *          1        0.069              0.794
 *         10        0.0099             0.794
 *       1000        0.0001             0.794
 *
 * so the raw form is invisible on offset variables: years, Kelvin, prices.
 *
 * CURVATURE IN THE FIT, once, against the fitted value: RESET's probe, which
 * sees a missing interaction or an odd power that a per-term probe cannot.
 *
 * SPREAD: residual size against the fitted value. Least squares assumes one
 * error size everywhere; when it grows with the prediction the residual SD is
 * not a typical error at either end.
 *
 * Correlations converted to a t, reported when |t| passes a bound; not tests.
 * Crude Ramsey RESET and Breusch-Pagan; R's car::residualPlots gives p-values.
 * The t assumes independent rows; otherwise it is too large. 100 correctly
 * specified fits of 300 rows each:
 *
 *      independent noise                          0 of 100 warned
 *      AR(1) rho=0.85, x independent of row order  0 of 100 warned
 *      AR(1) rho=0.85, x IS the row index         53 of 100 warned
 *
 * The failure needs the correlation to line up with a COLUMN, as a time series
 * does by construction; Durbin-Watson would name it but not repair the t. On
 * ordered data a curvature warning is a reason to open the residual file, not a
 * conclusion: plotted against row order it tells a real curve, where the advice
 * to add a square is right, from autocorrelation, where it is not. */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

/* The floor |t| must clear. A t, not a correlation: a correlation is a fixed
 * EFFECT SIZE, and a 0.4 floor stays silent at n=10,000 on curvature RESET
 * rejects at F=332. diag_bound() raises this with the probe count. */
#define DIAG_T 3.5

/* Every reported t is clamped here, so the value meaning "exact" is the largest
 * one. Past this the digits are the summation order, not the evidence. */
#define DIAG_T_CAP 9999.0

/* Rows below which nothing is reported. Ten, measured: of 200 correctly
 * specified models of ELEVEN rows each, three warned, the rate a t bound of 3.5
 * over a few probes should give. Eleven is Anscombe's quartet's set size. */
#define DIAG_MIN_ROWS 10

/* Nothing at all when the residuals are negligible against the response's own
 * spread: correlating rounding error measures the floating point unit.
 * example/routes.csv fits exactly, to 1e-7 of the answer. */
#define DIAG_MIN_SHARE 1e-6

/* One probe block, eleven doubles: shift, count, then sums of u, u^2, u^3, u^4,
 * u^5, u^6, r*u, r*u^2, r*u^3, u being the value less the shift. Shifted, since
 * raw power sums with variances recovered by subtraction are the naive-variance
 * formula regress.c refuses, and the offsets that motivate the probe (years,
 * timestamps, prices, Kelvin) live at 1e4 to 1e9. Up to u^6 because the cube,
 * partialled on 1, u and u^2, needs the sixth moment; the cube is there because
 * an even probe cannot see an odd departure. RESET uses both powers too. Keep
 * equal to the slots probe_add() writes: too small, each term's block writes its
 * last sum over the next term's shift and the probes go quiet. */
#define DIAG_PER_TERM 11
#define DIAG_SHARED   12

struct diag {
    int     nvars;
    int     shifted;    /* the caller supplied the centres */
    long long n;
    double *s;
    double  resid_sd, response_sd;
};

struct diag_result {
    int    curved_term;   /* term whose square explains the residual, or -1 */
    double curved_t;      /* its t statistic, 0 when there is none          */
    int    curved_pow;    /* 2 or 3: which power explained the residual     */
    double fitted_t;      /* the same probe against the fitted value        */
    double spread_t;      /* |residual| against the fitted value            */
};

size_t diag_storage(int nvars);
int    diag_init(struct diag *d, int nvars, double *storage);

/* The centre each term's powers are taken about, from the FIT rather than the
 * first row: from the first row an outlier arriving first becomes the centre,
 * and the same rows in another order give a different t. A running mean cannot
 * serve, moving the centre meaning six accumulated power sums re-normalised per
 * row through a binomial expansion. Costs nothing, the first pass having
 * computed these. Optional. shift holds nvars term centres; yhat is the fitted
 * value's centre, the mean of the response for a least-squares fit. */
void diag_center(struct diag *d, const double *shift, double yhat);

/* The response's own spread, so a residual can be told from rounding error. */
void diag_scale(struct diag *d, double resid_sd, double response_sd);

/* One row: its terms, the residual it left, and the value predicted. */
void diag_add(struct diag *d, const double *x, double resid, double fitted);

/* The bound |t| must pass, widened for the number of probes being read: each
 * group runs 2*nvars + 2 and the caller takes the maximum over all groups. */
double diag_bound(int nvars, long long groups);

void diag_result(const struct diag *d, double bound, struct diag_result *out);

#endif /* DIAG_H */
