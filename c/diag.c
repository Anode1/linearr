/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* diag.c: see diag.h. Correlations from running sums, partialled, as a t. */
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

/* Term j's block at j*DIAG_PER_TERM, then the fitted value's, then the shared. */
#define B(d, j)    ((d)->s + (size_t)(j) * DIAG_PER_TERM)
#define FIT(d)     ((d)->s + (size_t)(d)->nvars * DIAG_PER_TERM)
#define SH(d)      ((d)->s + ((size_t)(d)->nvars + 1) * DIAG_PER_TERM)

/* Powers about the column's own centre. A constant shift leaves the partial
 * correlations below unchanged and removes the cancellation: a raw sum of v^6 at
 * v ~ 1e5 keeps no significant digits for a variance recovered by subtraction.
 * Layout: [0] shift, [1] n, then u, u^2..u^6, r*u, r*u^2, r*u^3, u = v-shift. */
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
    {   /* The fitted value shifted by the fit probe's constant, so the spread
         * correlation is about a centre too: the sums are not shift-invariant. */
        double fs = fitted - FIT(d)[0];
        double rr = resid * resid;
        sh[0] += resid;
        sh[1] += rr;
        sh[2] += a;
        sh[3] += a * a;
        sh[4] += a * fs;
        sh[5] += fs;
        sh[6] += fs * fs;
        /* For the spread check: r^2 against the prediction and its square. |r|
         * against the prediction is a linear correlation, which an error growing
         * symmetrically about the middle of the range does not have. r^2 on 1, u
         * and u^2 is R's bptest and car::ncvTest, and sees both cases. */
        sh[7]  += rr * rr;
        sh[8]  += rr * fs;
        sh[9]  += rr * fs * fs;
        sh[10] += fs * fs * fs;
        sh[11] += fs * fs * fs * fs;
    }
}

/* Correlation of the residual with the part of u^2 that 1 and u do not explain:
 * the residual is orthogonal to u, so raw u^2 measures mostly what u covers. */
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
 * offset-invariant even in exact arithmetic: u^3 about a shifted origin carries a
 * 3*c*u^2 term [1, u] cannot absorb, leaving an even component orthogonal to the
 * odd residual sought. The 2x2 normal equations for u^3 on [u, u^2] restore it. */
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

/* The spread check: how much of the squared residual's variation the prediction
 * and its square explain. That auxiliary R^2 is the Breusch-Pagan score; sqrt of
 * its F is reported, on the same scale as the other probes. u^2 is in it because
 * [1, u] alone is blind to a symmetric pattern. */
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
    /* F on 2 and n-3, as its square root, clamped where t_of clamps.
     * DIAG_T_CAP is a sentinel meaning "exact" and diag_result picks by
     * magnitude: unclamped, a strong spread runs above 9999 and outranks it. */
    {   double f = sqrt((r2 / 2.0) / ((1.0 - r2) / (n - 3.0)));
        return (f > DIAG_T_CAP) ? DIAG_T_CAP : f;
    }
}

/* A correlation carries no sense of how much data stands behind it; a t does.
 * df is n minus what the model spent, p + 1, minus one for the probe. */
static double t_of(double r, double n, int nvars) {
    double denom = 1.0 - r * r;
    double df    = n - (double)nvars - 2.0;
    if (df <= 0.0) return 0.0;
    /* Capped: an exact relation would print a t of 1e9, which reads as a number
     * rather than "exactly". 1e-12 and not 1e-15, which sits below the rounding
     * floor of the sums that produced r, where 1 - r^2 is noise. */
    if (denom <= 1e-12) return (r < 0.0 ? -DIAG_T_CAP : DIAG_T_CAP);
    {   double t = r * sqrt(df) / sqrt(denom);
        /* Same clamp, so the sentinel meaning "exact" is never outranked by a
         * value meaning "strong": diag_result picks by |t|. */
        if (t >  DIAG_T_CAP) t =  DIAG_T_CAP;
        if (t < -DIAG_T_CAP) t = -DIAG_T_CAP;
        return t;
    }
}

/* The bound |t| must pass, given m probes. DIAG_T alone bounds ONE test; each
 * group runs 2*nvars + 2 and the summary maximises over groups, so 580 groups of
 * 35 terms is 41,760 probes. The maximum of m independent normals grows like
 * sqrt(2 ln m), and a fixed bound warned every time on correctly specified data
 * with normal noise. sqrt(DIAG_T^2 + 2 ln m) is DIAG_T at m = 1 and rises with
 * the maximum. Not a multiple-comparison procedure with a stated level. */
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

    /* The same probe against the fitted value, which sees an omitted interaction
     * or an odd power that a per-term probe cannot. */
    t = t_of(partial_corr(FIT(d), sh[0], sh[1]), n, d->nvars);
    if (fabs(t) >= bound) out->fitted_t = t;

    t = spread_stat(sh, n);
    if (t >= bound) out->spread_t = t;
}
