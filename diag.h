/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* diag.h: read the residuals and say whether a straight line was the wrong
 * choice.
 *
 * A fit reports R2, a residual SD and a conditioning figure. All three are
 * summaries of the residuals, so none can see structure IN them, and that is
 * where "the model has the wrong shape" is written.
 *
 * Three probes, all computable in one streaming pass, memory proportional to
 * the number of terms:
 *
 *   CURVATURE, per term. The correlation between the residual and the part of
 *   x^2 that is NOT explained by 1 and x. The partialling matters and was
 *   missing in the first version: the residual is already orthogonal to x by
 *   construction, so correlating it against RAW x^2 measures mostly the part of
 *   x^2 that x already accounts for. A reviewer showed the consequence. On the
 *   same quadratic, moving the origin of x alone:
 *
 *       offset   corr(r, x^2)   corr(r, x^2 partialled)
 *          0        0.204              0.794
 *          1        0.069              0.794
 *         10        0.0099             0.794
 *       1000        0.0001             0.794
 *
 *   The raw version was invisible on any variable with an offset, which is most
 *   of them: years, ages, temperatures in Kelvin, prices, counts with a floor.
 *   example/curve.csv is symmetric about zero, the one case where it worked,
 *   which is why it looked convincing.
 *
 *   CURVATURE IN THE FIT, once. The same probe against the fitted value rather
 *   than a term. A per-term probe cannot see a missing interaction or an odd
 *   power; a probe on y-hat can, and this is what Ramsey's RESET uses.
 *
 *   SPREAD. The correlation between the size of the residual and the fitted
 *   value. Least squares assumes the error is the same size everywhere; when it
 *   grows with the prediction, the residual SD is not a typical error at either
 *   end of the range.
 *
 * None of these is a hypothesis test in the strict sense; they are correlations
 * converted to a t statistic, which is the same arithmetic a test would use,
 * reported when |t| passes a fixed bound. What they are approximating is
 * standard: the first two are a crude Ramsey RESET, the third a crude
 * Breusch-Pagan, and R's car::residualPlots has reported the per-predictor
 * version with a p-value for years. Nothing here is new; it runs without R. */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

/* Report when |t| exceeds this. A t bound rather than a bare correlation
 * because a fixed correlation is a fixed EFFECT SIZE: the previous 0.4 floor
 * meant the check's sensitivity never improved with more data, and at n=10,000
 * it stayed silent on curvature that RESET rejected at F=332. A t bound scales
 * the way the evidence does. 3.5 leaves room for several terms being probed at
 * once without a correction of its own. */
#define DIAG_T 3.5

/* Rows below which nothing is reported: a handful of points can look like
 * anything, and the t statistic is not to be trusted there either. */
#define DIAG_MIN_ROWS 12

/* And nothing at all when the residuals are negligible against the response's
 * own spread: correlating rounding error with anything measures the floating
 * point unit. example/routes.csv fits exactly, to 1e-7 of the answer. */
#define DIAG_MIN_SHARE 1e-6

/* Per term: sums of x, x^2, x^3, x^4, r*x, r*x^2. Then the same six for the
 * fitted value, then n, sum r, sum r^2, and three for |r| against y-hat. */
/* x, x^2, x^3, x^4, r*x, r*x^2, then x^6 and r*x^3 for the cube probe. An
 * even probe cannot see an odd departure: a symmetric cubic leaves residuals
 * that are odd in x, and x^2 is orthogonal to them. RESET uses both the square
 * and the cube for this reason. */
#define DIAG_PER_TERM 8
#define DIAG_SHARED   9

struct diag {
    int     nvars;
    long    n;
    double *s;
    double  resid_sd, response_sd;
};

struct diag_result {
    long   rows;
    int    curved_term;   /* term whose square explains the residual, or -1 */
    double curved_t;      /* its t statistic, 0 when there is none          */
    int    curved_pow;    /* 2 or 3: which power explained the residual     */
    double fitted_t;      /* the same probe against the fitted value        */
    double spread_t;      /* |residual| against the fitted value            */
};

size_t diag_storage(int nvars);
int    diag_init(struct diag *d, int nvars, double *storage);

/* The response's own spread, so a residual can be told from rounding error. */
void diag_scale(struct diag *d, double resid_sd, double response_sd);

/* One row: its terms, the residual it left, and the value predicted. */
void diag_add(struct diag *d, const double *x, double resid, double fitted);

void diag_result(const struct diag *d, struct diag_result *out);

#endif /* DIAG_H */
