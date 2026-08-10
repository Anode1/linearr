/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* canon.h: published datasets whose answers somebody else certified.
 *
 * Every other test in this project checks the code against arithmetic this
 * project also wrote. That catches a change in behaviour and nothing else: if
 * the solver and the expected value are derived from the same understanding,
 * they are wrong together and the suite stays green. A reviewer put it as the
 * recurring fault here, that the gate is aimed one file to the left of the
 * defect.
 *
 * These datasets close that. They come from outside, their answers were
 * computed by somebody else to more digits than a double carries, and they are
 * the sets a statistician already knows:
 *
 *   NORRIS    36 points, one term, from a NIST calibration study. The easy
 *             case, and the one that says the plain path is right.
 *   LONGLEY   16 points, six terms, US macroeconomic series 1947-1962. Longley
 *             published it in 1967 precisely because the regression packages of
 *             the day returned as few as two correct digits on it; it has been
 *             the standard hard case ever since.
 *   WAMPLER1  21 points on an exact quintic, y = 1 + x + x^2 + ... + x^5. Every
 *             coefficient is 1 and the residual is 0, so any departure is the
 *             solver's own error with nothing else mixed in. This is the set
 *             that separates the two solvers here: see the numbers in tests.c.
 *
 * The certified values are from the NIST Statistical Reference Datasets
 * (Linear Regression), which are a US Government work and not under copyright.
 * They were checked before being written down: solving each set again in exact
 * rational arithmetic reproduces the published values to 1e-15, so the data and
 * the certified answers confirm each other rather than both resting on a
 * transcription.
 *
 * Anscombe's quartet is canonical too and is not here: it is a test of the
 * residual checks rather than of arithmetic, so it lives in example/ where it
 * can be run, and tests/cli.sh fits it. */
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
