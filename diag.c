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

/* Per-probe block: x, x^2, x^3, x^4, r*x, r*x^2. Term j at j*6; the fitted
 * value's block sits after the terms; the shared sums after that. */
#define B(d, j)    ((d)->s + (size_t)(j) * DIAG_PER_TERM)
#define FIT(d)     ((d)->s + (size_t)(d)->nvars * DIAG_PER_TERM)
#define SH(d)      ((d)->s + ((size_t)(d)->nvars + 1) * DIAG_PER_TERM)

static void probe_add(double *b, double v, double r) {
    double v2 = v * v, v3 = v2 * v;
    b[0] += v;
    b[1] += v2;
    b[2] += v3;
    b[3] += v2 * v2;
    b[4] += r * v;
    b[5] += r * v2;
    b[6] += v3 * v3;          /* v^6, for the variance of v^3 */
    b[7] += r * v3;
}

void diag_add(struct diag *d, const double *x, double resid, double fitted) {
    double *sh;
    double a = fabs(resid);
    int j;

    if (!isfinite(resid) || !isfinite(fitted)) return;
    d->n++;
    for (j = 0; j < d->nvars; j++) probe_add(B(d, j), x[j], resid);
    probe_add(FIT(d), fitted, resid);

    sh = SH(d);
    sh[0] += resid;
    sh[1] += resid * resid;
    sh[2] += a;
    sh[3] += a * a;
    sh[4] += a * fitted;
}

/* The correlation between the residual and the part of v^2 that 1 and v do not
 * explain. Partialling is the whole point: the residual is orthogonal to v by
 * construction, so a raw correlation with v^2 mostly measures what v already
 * accounts for, and vanishes as v moves away from zero. */
static double partial_corr(const double *b, double n, double sr, double srr) {
    double sxx   = b[1] - b[0] * b[0] / n;              /* var of v      */
    double sxx2  = b[2] - b[0] * b[1] / n;              /* cov(v, v^2)   */
    double sx2x2 = b[3] - b[1] * b[1] / n;              /* var of v^2    */
    double srx   = b[4] - sr * b[0] / n;                /* cov(r, v)     */
    double srx2  = b[5] - sr * b[1] / n;                /* cov(r, v^2)   */
    double srr_c = srr - sr * sr / n;                   /* var of r      */
    double bslope, varz, covrz;

    if (sxx <= 0.0 || srr_c <= 0.0) return 0.0;         /* v or r never varies */
    bslope = sxx2 / sxx;
    varz   = sx2x2 - bslope * sxx2;                     /* var of the residual
                                                           of v^2 on [1, v]   */
    if (varz <= 0.0) return 0.0;                        /* v^2 is a line in v  */
    covrz = srx2 - bslope * srx;
    return covrz / sqrt(varz * srr_c);
}

/* The same idea one power up. A cubic departure is odd in v, and the square
 * probe above is even, so it cannot see one however large it is. */
static double partial_corr3(const double *b, double n, double sr, double srr) {
    double sxx   = b[1] - b[0] * b[0] / n;              /* var of v        */
    double sxx3  = b[3] - b[0] * b[2] / n;              /* cov(v, v^3)     */
    double sx3x3 = b[6] - b[2] * b[2] / n;              /* var of v^3      */
    double srx   = b[4] - sr * b[0] / n;                /* cov(r, v)       */
    double srx3  = b[7] - sr * b[2] / n;                /* cov(r, v^3)     */
    double srr_c = srr - sr * sr / n;
    double bslope, varz, covrz;

    if (sxx <= 0.0 || srr_c <= 0.0) return 0.0;
    bslope = sxx3 / sxx;
    varz   = sx3x3 - bslope * sxx3;
    if (varz <= 0.0) return 0.0;
    covrz = srx3 - bslope * srx;
    return covrz / sqrt(varz * srr_c);
}

static double plain_corr(double n, double sx, double sxx, double sy, double syy,
                         double sxy) {
    double vx = n * sxx - sx * sx;
    double vy = n * syy - sy * sy;
    if (vx <= 0.0 || vy <= 0.0) return 0.0;
    return (n * sxy - sx * sy) / sqrt(vx * vy);
}

/* A correlation carries no sense of how much data stands behind it. The t
 * statistic does, which is why the threshold is on t and not on r: a fixed
 * correlation bound is a fixed effect size, and its sensitivity never improves
 * however much data arrives. */
static double t_of(double r, double n) {
    double denom = 1.0 - r * r;
    if (n <= 3.0) return 0.0;
    /* Capped: an exact relation would otherwise print a t of 1e9, which reads
     * as a number rather than as "exactly". */
    if (denom <= 1e-15) return (r < 0.0 ? -9999.0 : 9999.0);
    return r * sqrt(n - 3.0) / sqrt(denom);
}

void diag_result(const struct diag *d, struct diag_result *out) {
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
        t = t_of(partial_corr(B(d, j), n, sh[0], sh[1]), n);
        if (fabs(t) > fabs(best)) { best = t; out->curved_term = j; out->curved_pow = 2; }
        t = t_of(partial_corr3(B(d, j), n, sh[0], sh[1]), n);
        if (fabs(t) > fabs(best)) { best = t; out->curved_term = j; out->curved_pow = 3; }
    }
    if (fabs(best) < DIAG_T) { out->curved_term = -1; out->curved_pow = 0; }
    else out->curved_t = best;

    /* The same probe against the fitted value. A per-term probe cannot see an
     * omitted interaction or an odd power; this can. */
    t = t_of(partial_corr(FIT(d), n, sh[0], sh[1]), n);
    if (fabs(t) >= DIAG_T) out->fitted_t = t;

    t = t_of(plain_corr(n, FIT(d)[0], FIT(d)[1], sh[2], sh[3], sh[4]), n);
    if (fabs(t) >= DIAG_T) out->spread_t = t;
}
