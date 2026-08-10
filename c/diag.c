/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* diag.c: see diag.h. Correlations from running sums, partialled where it
 * matters, converted to a t statistic. */
#include "diag.h"
#include "common.h"

#include <math.h>
#include <string.h>

size_t diag_storage(int nvars) {
    if (nvars < 1) return 0;
    return (size_t)nvars * DIAG_PER_TERM + DIAG_SHARED + DIAG_PER_TERM;
}

int diag_init(struct diag *d, int nvars, double *storage) {
    if (nvars < 1 || !storage) return -1;
    memset(storage, 0, diag_storage(nvars) * sizeof *storage);
    d->nvars       = nvars;
    d->shifted     = 0;
    d->n           = 0;
    d->s           = storage;
    d->resid_sd    = -1.0;
    d->response_sd = -1.0;
    return 0;
}

void diag_scale(struct diag *d, double resid_sd, double response_sd) {
    d->resid_sd    = resid_sd;
    d->response_sd = response_sd;
}

/* Term j's block at j*DIAG_PER_TERM; the fitted value's block after the terms;
 * the shared sums after that. */
#define B(d, j)    ((d)->s + (size_t)(j) * DIAG_PER_TERM)
#define FIT(d)     ((d)->s + (size_t)(d)->nvars * DIAG_PER_TERM)
#define SH(d)      ((d)->s + ((size_t)(d)->nvars + 1) * DIAG_PER_TERM)

/* Powers about the column's own centre, taken from its first value. Shifting by
 * a constant changes none of the partial correlations below, and it removes the
 * cancellation that destroyed them: a raw sum of v^6 at v ~ 1e5 has no
 * significant digits left for a variance recovered by subtraction. A reviewer
 * measured the square probe going silent at offset 1e5 and, worse, inflating at
 * 1e4 into a departure that was not there.
 *
 * Layout: [0] shift, [1] n, then sums of u, u^2, u^3, u^4, u^5, u^6, r*u,
 * r*u^2, r*u^3 where u = v - shift. */
void diag_center(struct diag *d, const double *shift, double yhat) {
    int j;
    if (!d || !shift || d->n != 0) return;   /* only before any row is added */
    for (j = 0; j < d->nvars; j++) B(d, j)[0] = shift[j];
    FIT(d)[0] = yhat;
    d->shifted = 1;
}

static void probe_add(double *b, double v, double r, int preset) {
    double u, u2, u3;

    if (b[1] == 0.0 && !preset) b[0] = v;   /* else the caller set the centre */
    b[1] += 1.0;
    u  = v - b[0];
    u2 = u * u;
    u3 = u2 * u;
    b[2]  += u;
    b[3]  += u2;
    b[4]  += u3;
    b[5]  += u2 * u2;
    b[6]  += u2 * u3;
    b[7]  += u3 * u3;
    b[8]  += r * u;
    b[9]  += r * u2;
    b[10] += r * u3;
}

void diag_add(struct diag *d, const double *x, double resid, double fitted) {
    double *sh;
    double a = fabs(resid);
    int j;

    if (!isfinite(resid) || !isfinite(fitted)) return;
    d->n++;
    for (j = 0; j < d->nvars; j++) probe_add(B(d, j), x[j], resid, d->shifted);
    probe_add(FIT(d), fitted, resid, d->shifted);

    sh = SH(d);
    {   /* The fitted value shifted by the same constant the fit probe uses, so
         * the spread correlation is computed about a centre too. A correlation
         * is shift-invariant, and the sums are not. */
        double fs = fitted - FIT(d)[0];
        double rr = resid * resid;
        sh[0] += resid;
        sh[1] += rr;
        sh[2] += a;
        sh[3] += a * a;
        sh[4] += a * fs;
        sh[5] += fs;
        sh[6] += fs * fs;
        /* For the spread check: the SQUARED residual against the prediction
         * and its square. Correlating |r| with the prediction, which is what
         * this did, is a LINEAR correlation, and an error that grows
         * symmetrically about the middle of the range has none: measured, that
         * probe fired on 2.5% of samples where the spread genuinely varied
         * with |x|. Regressing r^2 on 1, u and u^2 is the shape R's bptest and
         * car::ncvTest use, and it sees both the monotone and the symmetric
         * case. */
        sh[7]  += rr * rr;
        sh[8]  += rr * fs;
        sh[9]  += rr * fs * fs;
        sh[10] += fs * fs * fs;
        sh[11] += fs * fs * fs * fs;
    }
}

/* The correlation between the residual and the part of u^2 that 1 and u do not
 * explain. Partialling is the point: the residual is orthogonal to u by
 * construction, so a raw correlation with u^2 measures mostly what u already
 * accounts for. */
static double partial_corr(const double *b, double sr, double srr) {
    double n    = b[1];
    double su   = b[2], su2 = b[3], su3 = b[4], su4 = b[5];
    double sru  = b[8], sru2 = b[9];
    double suu  = su2 - su * su / n;                 /* var u        */
    double su_2 = su3 - su * su2 / n;                /* cov(u, u^2)  */
    double s22  = su4 - su2 * su2 / n;               /* var u^2      */
    double cru  = sru - sr * su / n;
    double cru2 = sru2 - sr * su2 / n;
    double srr_c = srr - sr * sr / n;
    double bs, varz, covrz;

    if (suu <= 0.0 || srr_c <= 0.0) return 0.0;
    bs    = su_2 / suu;
    varz  = s22 - bs * su_2;
    if (varz <= 0.0) return 0.0;
    covrz = cru2 - bs * cru;
    return covrz / sqrt(varz * srr_c);
}

/* The cube, partialled on 1, u AND u^2. On [1, u] alone it is not
 * offset-invariant even in exact arithmetic: u^3 about a shifted origin carries
 * a 3*c*u^2 term that [1, u] cannot absorb, so the probe was dominated by an
 * even component orthogonal to the odd residual it exists to find. A reviewer
 * showed a cubic with amplitude 25 against noise 1 going undetected at an
 * offset of 10. Solving the 2x2 normal equations for u^3 on [u, u^2] (both
 * already centred) restores it. */
static double partial_corr3(const double *b, double sr, double srr) {
    double n   = b[1];
    double su  = b[2], su2 = b[3], su3 = b[4], su4 = b[5], su5 = b[6], su6 = b[7];
    double sru = b[8], sru2 = b[9], sru3 = b[10];
    /* centred moments */
    double m11 = su2 - su * su / n;                  /* <u,u>     */
    double m12 = su3 - su * su2 / n;                 /* <u,u^2>   */
    double m22 = su4 - su2 * su2 / n;                /* <u^2,u^2> */
    double c1  = su4 - su * su3 / n;                 /* <u,u^3>   */
    double c2  = su5 - su2 * su3 / n;                /* <u^2,u^3> */
    double m33 = su6 - su3 * su3 / n;                /* <u^3,u^3> */
    double cru = sru - sr * su / n, cru2 = sru2 - sr * su2 / n;
    double cru3 = sru3 - sr * su3 / n;
    double srr_c = srr - sr * sr / n;
    double det, a1, a2, varz, covrz;

    if (srr_c <= 0.0) return 0.0;
    det = m11 * m22 - m12 * m12;
    if (det <= 0.0) return 0.0;
    a1 = ( m22 * c1 - m12 * c2) / det;               /* u^3 on [u, u^2] */
    a2 = (-m12 * c1 + m11 * c2) / det;
    varz  = m33 - a1 * c1 - a2 * c2;
    if (varz <= 0.0) return 0.0;
    covrz = cru3 - a1 * cru - a2 * cru2;
    return covrz / sqrt(varz * srr_c);
}

/* The spread check: how much of the squared residual's own variation is
 * explained by the prediction and its square. R^2 of that auxiliary regression
 * is the Breusch-Pagan score, and sqrt of its F is reported so the number sits
 * on the same scale as the other probes.
 *
 * On [1, u] alone this would still be blind to a symmetric pattern, which is
 * why u^2 is in it. */
static double spread_stat(const double *sh, double n) {
    double sw = sh[1], sww = sh[7], swu = sh[8], swu2 = sh[9];
    double su = sh[5], su2 = sh[6], su3 = sh[10], su4 = sh[11];
    /* Centred cross-products of the auxiliary design [u, u^2] and of w. */
    double m11 = su2 - su * su / n;
    double m12 = su3 - su * su2 / n;
    double m22 = su4 - su2 * su2 / n;
    double c1  = swu - sw * su / n;
    double c2  = swu2 - sw * su2 / n;
    double sww_c = sww - sw * sw / n;
    double det, a1, a2, explained, r2;

    if (n <= 4.0 || sww_c <= 0.0) return 0.0;
    det = m11 * m22 - m12 * m12;
    if (det <= 0.0) return 0.0;
    a1 = ( m22 * c1 - m12 * c2) / det;
    a2 = (-m12 * c1 + m11 * c2) / det;
    explained = a1 * c1 + a2 * c2;
    if (explained <= 0.0) return 0.0;
    r2 = explained / sww_c;
    if (r2 >= 1.0) return DIAG_T_CAP;
    /* F on 2 and n-3, reported as its square root. */
    return sqrt((r2 / 2.0) / ((1.0 - r2) / (n - 3.0)));
}

/* A correlation carries no sense of how much data stands behind it. The t
 * statistic does, which is why the threshold is on t and not on r: a fixed
 * correlation bound is a fixed effect size, and its sensitivity never improves
 * however much data arrives. */
/* df is n minus what the MODEL already spent, p + 1, minus one more for the
 * probe itself. It was n - 3, which is right only when p = 1, and every
 * example that exercised this file has one term. At n = 15 with 8 terms that
 * inflated the statistic by 1.55x. */
static double t_of(double r, double n, int nvars) {
    double denom = 1.0 - r * r;
    double df    = n - (double)nvars - 2.0;
    if (df <= 0.0) return 0.0;
    /* Capped: an exact relation would otherwise print a t of 1e9, which reads
     * as a number rather than as "exactly". The bound is 1e-12 and not the
     * 1e-15 it was, because 1e-15 sits BELOW the rounding floor of the sums
     * that produced r: on an exactly quadratic residual the same probe returned
     * 9.0e7, 8.6e7, 7.3e7 and 9999 at four offsets of the same column, which is
     * rounding noise in 1 - r^2 and nothing else. A test that compared those
     * four failed, correctly, and the fault was here. */
    if (denom <= 1e-12) return (r < 0.0 ? -DIAG_T_CAP : DIAG_T_CAP);
    {   double t = r * sqrt(df) / sqrt(denom);
        /* Clamped at the same place. The cap used to be reachable only through
         * the branch above, while ordinary data produced 40387 and 1237430, so
         * the sentinel meaning "exact" sorted BELOW values meaning "strong",
         * and diag_result picks by |t|. */
        if (t >  DIAG_T_CAP) t =  DIAG_T_CAP;
        if (t < -DIAG_T_CAP) t = -DIAG_T_CAP;
        return t;
    }
}

/* The bound |t| must pass, given how many probes are being read.
 *
 * DIAG_T alone is a bound for ONE test. Each group runs 2*nvars + 2 of them
 * and the summary takes the maximum over every group, so a file of 580 groups
 * of 35 terms reads 41,760 probes and reports the largest. The maximum of m
 * independent normals grows like sqrt(2 ln m), so a fixed bound is not a fixed
 * error rate: on correctly specified data with normal noise, that shape
 * produced a warning every single time.
 *
 * sqrt(DIAG_T^2 + 2 ln m) is DIAG_T at m = 1 and rises the way the maximum
 * does. It is not a multiple-comparison procedure with a stated level; it is a
 * bound that stops the report being certain on data that is fine. */
double diag_bound(int nvars, long long groups) {
    double m = (double)(2 * nvars + 2) * (double)(groups > 0 ? groups : 1);
    if (m <= 1.0) return DIAG_T;
    return sqrt(DIAG_T * DIAG_T + 2.0 * log(m));
}

void diag_result(const struct diag *d, double bound, struct diag_result *out) {
    const double *sh = SH(d);
    double n = (double)d->n;
    double best = 0.0, t;
    int j;

    out->rows        = d->n;
    out->curved_term = -1;
    out->curved_t    = 0.0;
    out->curved_pow  = 0;
    out->fitted_t    = 0.0;
    out->spread_t    = 0.0;
    if (d->n < DIAG_MIN_ROWS) return;

    /* Nothing left to explain: these residuals are rounding error. */
    if (d->response_sd > 0.0 && d->resid_sd >= 0.0 &&
        d->resid_sd < DIAG_MIN_SHARE * d->response_sd) return;

    for (j = 0; j < d->nvars; j++) {
        t = t_of(partial_corr(B(d, j), sh[0], sh[1]), n, d->nvars);
        if (fabs(t) > fabs(best)) { best = t; out->curved_term = j; out->curved_pow = 2; }
        t = t_of(partial_corr3(B(d, j), sh[0], sh[1]), n, d->nvars);
        if (fabs(t) > fabs(best)) { best = t; out->curved_term = j; out->curved_pow = 3; }
    }
    if (fabs(best) < bound) { out->curved_term = -1; out->curved_pow = 0; }
    else out->curved_t = best;

    /* The same probe against the fitted value. A per-term probe cannot see an
     * omitted interaction or an odd power; this can. */
    t = t_of(partial_corr(FIT(d), sh[0], sh[1]), n, d->nvars);
    if (fabs(t) >= bound) out->fitted_t = t;

    t = spread_stat(sh, n);
    if (t >= bound) out->spread_t = t;
}
