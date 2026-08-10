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
 *   x^2 that is NOT explained by 1 and x, and separately of x^3 on 1, x and
 *   x^2. Both are computed about each column's own centre. The partialling matters and was
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
 * version with a p-value for years. Nothing here is new; it runs without R.
 *
 * WHERE IT IS WRONG. The t statistic assumes the rows are independent. When
 * they are not, it is too large, and the checks will describe correlated noise
 * as a shape. Measured over 100 correctly specified fits of 300 rows each:
 *
 *      independent noise              0 of 100 produced a warning
 *      AR(1), rho = 0.85             20 of 100 produced a warning
 *
 * Twenty per cent is the cost of reading a time series row by row and calling
 * the rows independent. There is no fix inside a one-pass residual check; a
 * Durbin-Watson statistic would name the cause but not repair the t. So: on
 * data with an order to it -- a series in time, a sequence down a well, repeat
 * measurements on the same subject -- treat a curvature warning as a reason to
 * look at the residual file, not as a conclusion. On unordered rows it means
 * what it says. The suggestion the warning prints, to add a square, is the
 * right move for a real curve and the wrong one for autocorrelation, and this
 * check cannot tell you which you have. Plotting the residual file against row
 * order takes a minute and does. */
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
 * anything. Ten rather than twelve, measured rather than guessed: on 200
 * correctly specified models of ELEVEN rows each, three produced a warning,
 * which is the rate a t bound of 3.5 over a few probes should give. Eleven is
 * also the size of each set in Anscombe's quartet, and a screen that cannot
 * speak about the canonical example of summary statistics hiding structure is
 * not much of a screen. */
#define DIAG_MIN_ROWS 10

/* And nothing at all when the residuals are negligible against the response's
 * own spread: correlating rounding error with anything measures the floating
 * point unit. example/routes.csv fits exactly, to 1e-7 of the answer. */
#define DIAG_MIN_SHARE 1e-6

/* One probe block, eleven doubles: the shift, the count, then sums of u, u^2,
 * u^3, u^4, u^5, u^6, r*u, r*u^2, r*u^3, where u is the value less the shift.
 *
 * Powers of the SHIFTED value because the first version accumulated raw power
 * sums and recovered variances by subtraction, which is the naive-variance
 * formula regress.c refuses to use. A reviewer measured the square probe going
 * silent at a column offset of 1e5 and, worse, inflating at 1e4 into a
 * departure that was not there. The offsets the header names as the motivation
 * for the probe (years, timestamps, prices, Kelvin) all live at 1e4 to 1e9.
 *
 * Up to u^6 because the cube is partialled on 1, u and u^2, which needs the
 * sixth moment. The cube is there because an even probe cannot see an odd
 * departure: a symmetric cubic leaves residuals that are odd in u, and u^2 is
 * orthogonal to them. RESET uses both powers for the same reason.
 *
 * Keep this equal to the number of slots probe_add() writes. It was 10 against
 * eleven slots for one build, and every term's block then wrote its last sum
 * over the next term's shift; the probes went quiet rather than wrong, which is
 * the failure this file exists to complain about. */
#define DIAG_PER_TERM 11
#define DIAG_SHARED   7

struct diag {
    int     nvars;
    long long n;
    double *s;
    double  resid_sd, response_sd;
};

struct diag_result {
    long long rows;
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

/* The bound |t| must pass, widened for the number of probes being read: each
 * group runs 2*nvars + 2 and the caller takes the maximum over all groups. */
double diag_bound(int nvars, long long groups);

void diag_result(const struct diag *d, double bound, struct diag_result *out);

#endif /* DIAG_H */
