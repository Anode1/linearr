/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* process.h: two directions of one model. Score a case against the fitted
 * coefficients, or fit them from a training file. Nothing here prints. */
#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdio.h>
#include "constants.h"

/* scoring: load the coefficient table, and the trim additions if configured.
 * Call once, before the first process()/process_named(). Returns 0, or -1 with
 * a sentence in err[errsz] naming what could not be opened and where it was
 * sought. A table that will not load is fatal here, not per row. */
int process_init(char *err, size_t errsz);

/* Override the configured table paths, for -c/--coef and --trim/--no-trim.
 * Call before process_init. process_use_trim(NULL) means deliberately none. */
void process_use_coef(const char *path);
void process_use_trim(const char *path);

/* Solve by QR (qr.h), not normal equations (regress.h): same answer on a
 * well-conditioned design, the right one otherwise, forming X'X squaring the
 * condition number. Slower per row. */
void process_use_qr(int on);

/* "normal equations" or "QR": which solver produced the last fit. They report
 * cond= on different scales, cond(X'X) against cond(X). */
const char *process_solver(void);

/* Score one case as a row: "group,x1,...,xp", one value per term in the
 * coefficient file's column order. Returns 0, or -1 (see process_error). */
int process(const char *input, char *out, size_t outsz);

/* Score one case by name: a group plus n assignments "term=value". A term not
 * mentioned is 0, so a wide model needs no run of zeroes. Returns 0, or -1. */
int process_named(const char *group, char *const *assign, int n,
                  char *out, size_t outsz);

/* Why the last call returned -1. Never NULL; a fragment fit to follow
 * "cannot score X: ". */
const char *process_error(void);

/* the loaded schema, for `--terms` */

int         process_nterms(void);          /* terms in the loaded table       */
long long   process_ngroups(void);         /* groups in the loaded table      */
const char *process_term_name(int i);      /* NULL if i is out of range       */
const char *process_coef_path(void);       /* the table actually opened       */

/* fitting */

/* What the fit found, for the report on stderr. */
struct fit_info {
    long long rows;     /* training rows used                              */
    int    pinned;      /* terms the sample could not identify, set to 0   */
    long long df;       /* residual degrees of freedom: rows minus terms
                           identified. At or below 0 the line passes through
                           every point and r2 is 1 whatever the data says   */
    double r2;          /* -1 when undefined or not computable usefully    */
    int    sigma_is_bound;  /* sigma is an upper bound, not a value        */
    double sigma;       /* residual SD, in the response's units. -1 when
                           there is no residual freedom.                   */
    double condition;   /* largest accepted pivot over smallest. Past ~1e8 the
                           coefficients' trailing digits are noise, and R2 will
                           not say so: an ill-conditioned design fits its own
                           sample well                                      */
};

/* The predicted column, by header name. NULL, the default, takes column 2. */
void process_use_response(const char *name);

/* Non-zero if --response named the column rather than it being column 2. */
int process_response_named(void);

/* Decimal places in the prediction and in the trim point. 0..9; outside that,
 * returns -1 and changes nothing. */
int process_set_scale(int decimals);
int process_set_trim_scale(int decimals);

/* Is a progress line due? Split out so it is testable without waiting a minute.
 * rows so far; elapsed and since_last in seconds. */
int process_progress_due(long long rows, long elapsed, long since_last);

/* Bytes held for ONE group while -t fits every group in one pass: fitter,
 * coefficients, residual-check block, record. Multiply by the group count. */
size_t process_group_bytes(int nvars);

/* Bytes held for one group of a LOADED table: the scoring side, a different
 * figure. Independent of the term count, the array being at the build ceiling. */
size_t process_model_bytes(void);

/* Fit one group's line from a training CSV of "group,value,<terms>" rows whose
 * header names the terms. group selects the rows; "*" pools every row under that
 * name. Writes a complete coefficient file into out -- header, newline, fitted
 * row -- so `linearr -t train.csv -g 001 > model.csv` gives a table the scorer
 * reads back. info may be NULL; a few rows still fit, so check info.df.
 * Returns 0, or -1 (see process_error). */
int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info);

/* Fit EVERY group in one pass, writing a complete coefficient file to out: one
 * line per group, one read of the file. The one place the footprint depends on
 * more than the model, holding one accumulator per group; process_group_bytes()
 * and `linearr --footprint TERMS [GROUPS]` both say what one costs, from the
 * same code. Nothing grows with the ROW count. */
struct fit_summary {
    long long groups;   /* groups fitted                                  */
    long long rows;     /* training rows used                             */
    long long min_df;   /* the least residual freedom any group had       */
    double min_r2;      /* the worst group's R2, or -1 if none was defined  */
    /* Groups with no R2 to report, and why. The worst-of aggregation skips
       them, so these keep them counted. */
    long long groups_flat_y;   /* the response never varied                  */
    long long groups_r2_lost;  /* the arithmetic could not report one        */
    double max_sigma;   /* the worst group's residual standard deviation   */
    int    sigma_is_bound;  /* max_sigma is an upper bound, not a value    */
    double max_condition;  /* the worst-conditioned group                 */
    /* From the residual pass. Each group is examined alone and the strongest
     * finding reported with its group. curved_term indexes the schema, or -1. */
    int    curved_term;
    double curved_t;
    int    curved_pow;   /* 2 or 3 */
    double fitted_t;
    double spread_t;
    char   worst_group[GROUP_MAX];
    int    pinned;      /* total terms pinned across all groups           */
};
int process_train_all(const char *csv_path, FILE *out, struct fit_summary *sum);

/* Fit, then write one residual per training row to `resid`: what the row said,
 * what the line predicts, the difference. That shows the shape of the error -- a
 * curve the line cannot follow, a spread growing with the prediction, the one
 * row unlike the others -- which R2 and the residual SD average away. Costs a
 * SECOND PASS, not a copy in memory: the fit forgets each row.
 * stats: with -t, one row per GROUP: rows, df, R2, residual SD, conditioning, of
 * which the summary reports only the worst. NULL for none.
 * only: fit just that group, or NULL for every group. */
int process_train_residuals(const char *csv_path, const char *only, FILE *out,
                            FILE *resid, FILE *stats, struct fit_summary *sum);

/* Release what scoring loaded. Idempotent. */
void process_free(void);

#endif /* PROCESS_H */
