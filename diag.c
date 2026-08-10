/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* diag.c: see diag.h. Pearson correlations from running sums. */
#include "diag.h"
#include "common.h"

#include <math.h>
#include <string.h>

/* Per term: sum of x^2, of (x^2)^2, and of residual*x^2. The residual's own
 * sums are shared, so they are kept once at the end. */
#define PER_TERM 3
#define SHARED   4      /* sum r, r^2, |r|, and the pair for |r| against fitted */

size_t diag_storage(int nvars) {
    if (nvars < 1) return 0;
    return (size_t)nvars * PER_TERM + SHARED + 3;
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

/* Layout: [0 .. 3n)  per term, then the shared sums. */
#define SX(d, j)   ((d)->s[(size_t)(j) * PER_TERM + 0])   /* sum x^2        */
#define SXX(d, j)  ((d)->s[(size_t)(j) * PER_TERM + 1])   /* sum (x^2)^2    */
#define SRX(d, j)  ((d)->s[(size_t)(j) * PER_TERM + 2])   /* sum r * x^2    */
#define SR(d)      ((d)->s[(size_t)(d)->nvars * PER_TERM + 0])
#define SRR(d)     ((d)->s[(size_t)(d)->nvars * PER_TERM + 1])
#define SA(d)      ((d)->s[(size_t)(d)->nvars * PER_TERM + 2])   /* sum |r|  */
#define SAA(d)     ((d)->s[(size_t)(d)->nvars * PER_TERM + 3])
#define SF(d)      ((d)->s[(size_t)(d)->nvars * PER_TERM + 4])   /* sum yhat */
#define SFF(d)     ((d)->s[(size_t)(d)->nvars * PER_TERM + 5])
#define SAF(d)     ((d)->s[(size_t)(d)->nvars * PER_TERM + 6])

void diag_scale(struct diag *d, double resid_sd, double response_sd) {
    d->resid_sd    = resid_sd;
    d->response_sd = response_sd;
}

void diag_add(struct diag *d, const double *x, double resid, double fitted) {
    double a = fabs(resid);
    int j;

    if (!isfinite(resid) || !isfinite(fitted)) return;
    d->n++;
    for (j = 0; j < d->nvars; j++) {
        double x2 = x[j] * x[j];
        SX(d, j)  += x2;
        SXX(d, j) += x2 * x2;
        SRX(d, j) += resid * x2;
    }
    SR(d)  += resid;
    SRR(d) += resid * resid;
    SA(d)  += a;
    SAA(d) += a * a;
    SF(d)  += fitted;
    SFF(d) += fitted * fitted;
    SAF(d) += a * fitted;
}

static double pearson(double n, double sx, double sxx, double sy, double syy,
                      double sxy) {
    double cov = n * sxy - sx * sy;
    double vx  = n * sxx - sx * sx;
    double vy  = n * syy - sy * sy;
    if (vx <= 0.0 || vy <= 0.0) return 0.0;    /* one of them never varies */
    return cov / sqrt(vx * vy);
}

void diag_result(const struct diag *d, struct diag_result *out) {
    double n = (double)d->n;
    int j;

    out->rows        = d->n;
    out->curved_term = -1;
    out->curved_r    = 0.0;
    out->spread_r    = 0.0;
    if (d->n < DIAG_MIN_ROWS) return;

    /* Nothing left to explain: the residuals are rounding error. */
    if (d->response_sd > 0.0 && d->resid_sd >= 0.0 &&
        d->resid_sd < DIAG_MIN_SHARE * d->response_sd) return;

    {   /* A correlation is not evidence until it exceeds what noise gives at
         * this sample size. */
        double floor_r = 3.0 / sqrt(n);
        if (floor_r < DIAG_REPORT) floor_r = DIAG_REPORT;

        for (j = 0; j < d->nvars; j++) {
            double r = pearson(n, SX(d, j), SXX(d, j), SR(d), SRR(d), SRX(d, j));
            if (fabs(r) > fabs(out->curved_r)) { out->curved_r = r; out->curved_term = j; }
        }
        if (fabs(out->curved_r) < floor_r) { out->curved_r = 0.0; out->curved_term = -1; }

        out->spread_r = pearson(n, SF(d), SFF(d), SA(d), SAA(d), SAF(d));
        if (fabs(out->spread_r) < floor_r) out->spread_r = 0.0;
    }
}
