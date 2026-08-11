/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* canon.h: published datasets whose answers somebody else certified. Every other
 * test checks the code against arithmetic this project also wrote, which can be
 * wrong with the solver and stay green. These are from outside, computed to more
 * digits than a double carries:
 *
 *   NORRIS    36 points, one term, from a NIST calibration study. The easy
 *             case, and the one that says the plain path is right.
 *   LONGLEY   16 points, six terms, US macroeconomic series 1947-1962. Longley
 *             published it in 1967 because the packages of the day returned as
 *             few as two correct digits on it.
 *   WAMPLER1  21 points on an exact quintic, y = 1 + x + x^2 + ... + x^5. Every
 *             coefficient is 1 and the residual 0, so any departure is the
 *             solver's own error. This set separates the two solvers; the
 *             numbers are in tests.c.
 *
 * Certified values from the NIST Statistical Reference Datasets (Linear
 * Regression), a US Government work not under copyright. Solving each set again
 * in exact rational arithmetic reproduces them to 1e-15, so data and answers
 * confirm each other rather than resting on a transcription. Anscombe's quartet
 * is canonical too but tests the residual checks, not arithmetic, so it lives in
 * example/ and tests/cli.sh fits it. */
#ifndef CANON_H
#define CANON_H

struct canon_set {
    const char   *name;
    int           nvars;      /* terms, not counting the intercept */
    int           n;          /* rows                              */
    const double *data;       /* n rows of (1 + nvars): y, then the terms */
    const double *beta;       /* certified, nvars+1, intercept first      */
    double        sigma;      /* certified residual standard deviation    */
    double        r2;         /* certified R^2                            */
};

extern const struct canon_set canon_norris;
extern const struct canon_set canon_longley;
extern const struct canon_set canon_wampler1;

#endif /* CANON_H */
