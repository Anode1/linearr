/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* process.h: THE slot. Two directions of the same model: score a case against
 * the fitted coefficients, or fit the coefficients from a training file.
 * main.c calls one or the other and prints the result; nothing here prints. */
#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdio.h>
#include "constants.h"

/* scoring
 *
 * Load the coefficient table (and the trim additions, if configured) so the
 * program can score. Call it once, before the first process()/process_named().
 * Returns 0, or -1 with a complete sentence in err[errsz] saying what could not
 * be opened and where it was looked for.
 *
 * This exists because failing lazily lied: a missing table made every case fail
 * with "cannot score: <the case>", which sends the user to check their data
 * when the data was never the problem. A table that will not load is one fatal
 * condition, reported once, not a per-row complaint. */
int process_init(char *err, size_t errsz);

/* Override the configured table paths, for -c/--coef and --trim/--no-trim.
 * Call before process_init. process_use_trim(NULL) means deliberately none.
 * These exist because the only way to score against a model you had just
 * fitted was to create a directory and redirect into a hardcoded
 * relative path: a dead end a first-time user hit within five minutes. */
void process_use_coef(const char *path);
void process_use_trim(const char *path);

/* Solve by QR (qr.h) instead of by normal equations (regress.h). Same answer on
 * a well-conditioned design, and the right answer on one that is not: forming
 * X'X squares the condition number, and QR does not. Slower per row. */
void process_use_qr(int on);

/* "normal equations" or "QR": which solver produced the last fit. The two
 * report cond= on different scales (cond(X'X) against cond(X)), so a summary
 * that prints one without the other invites a comparison that is not valid. */
const char *process_solver(void);

/* Score one case written as a row: "group,x1,...,xp", one value per term in the
 * coefficient file's column order. Returns 0, or -1; process_error() then
 * says why. */
int process(const char *input, char *out, size_t outsz);

/* Score one case written by name: a group, plus n assignments of the form
 * "term=value". Any term not mentioned is 0, which is what makes this usable
 * with a wide model: naming the two things that are true beats typing 254
 * zeroes. Returns 0, or -1 (see process_error). */
int process_named(const char *group, char *const *assign, int n,
                  char *out, size_t outsz);

/* Why the last process()/process_named()/process_init() returned -1. Never
 * NULL; a sentence fragment fit to follow "cannot score X: ". */
const char *process_error(void);

/* the loaded schema, for `--terms` */

int         process_nterms(void);          /* terms in the loaded table       */
long        process_ngroups(void);         /* groups in the loaded table      */
const char *process_term_name(int i);      /* NULL if i is out of range       */
const char *process_coef_path(void);       /* the table actually opened       */

/* fitting */

/* What the fit found, for the report on stderr. */
struct fit_info {
    long   rows;        /* training rows used                              */
    int    pinned;      /* terms the sample could not identify, set to 0   */
    long   df;          /* residual degrees of freedom: rows minus the
                           terms actually identified. At or below 0 the
                           line passes through every point by construction
                           and r2 is 1 no matter what the data says.       */
    double r2;          /* coefficient of determination, -1 when undefined
                           or not computable to useful precision           */
    double sigma;       /* residual standard deviation: how far a prediction
                           typically lands from the truth, in the response's
                           units. -1 when there is no residual freedom.     */
    double condition;   /* conditioning proxy: the ratio of the largest to
                           the smallest pivot the fit accepted. Past ~1e8
                           the trailing digits of the coefficients are
                           noise, and R2 will not tell you, because an
                           ill-conditioned design fits its own sample
                           beautifully.                                    */
};

/* Fit one group's line from a training CSV of "group,value,<terms>" rows, whose
 * header names the terms. group selects the rows; "*" pools every row in the
 * file under that name. Writes a complete two-line coefficient file into out:
 * the header, a newline, then the fitted row, so
 *   linearr -t train.csv -g 001 > model.csv
 * produces a table the scorer can read straight back. info may be NULL.
 * Returns 0, or -1 (see process_error). A group with few rows still fits: see
 * info.df before believing the result. */
/* Bytes held for ONE group while -t fits every group in one pass. The fitter,
 * the coefficients and the residual-check block, plus the record around them.
 * Multiply by the number of groups; nothing here depends on the row count. */
/* Decimal places in the printed prediction, and in the trim point. 0..9;
 * returns -1 and changes nothing outside that. These were keys in a properties
 * file, which is a second way of saying what an option already says. */
int process_set_scale(int decimals);
int process_set_trim_scale(int decimals);

/* Is a progress line due? Split out so it can be tested without waiting a
 * minute for one. rows is the count so far, elapsed the seconds since the run
 * began, since_last the seconds since the previous line. */
int process_progress_due(long rows, long elapsed, long since_last);

size_t process_group_bytes(int nvars);

/* Bytes held for one group of a LOADED coefficient table, which is the scoring
 * side and a different figure. Independent of the term count: the coefficient
 * array is dimensioned at the build ceiling. */
size_t process_model_bytes(void);

int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info);

/* Fit EVERY group in the training file, in one pass, writing a complete
 * coefficient file to out. This is what the model is actually for (one line
 * per group), and doing it with repeated -g invocations cost one full re-read
 * of the training file per group.
 *
 * Memory here is the one place this program's footprint depends on something
 * other than the model: fitting every group in one pass holds one accumulator
 * per group. process_group_bytes() below says exactly what one costs, and
 * `linearr --footprint TERMS [GROUPS]` prints it, so the figure in a document
 * and the figure the program allocates come from the same line of code. They
 * used not to: three places in this project quoted three different numbers,
 * all of them counting the fitter alone.
 *
 * Nothing grows with the number of ROWS, which is the property the whole
 * design exists for.
 */
struct fit_summary {
    long   groups;      /* groups fitted                                  */
    long   rows;        /* training rows used                             */
    long   min_df;      /* the least residual freedom any group had       */
    double max_sigma;   /* the worst group's residual standard deviation  */
    double max_condition;  /* the worst-conditioned group                 */
    /* From the residual pass, when one was made. Each group is examined on its
     * own and the strongest finding is reported with the group it came from:
     * pooling them meant three groups that were each individually correct, with
     * different scales, produced a warning about a file where nothing was
     * wrong. curved_term is an index into the schema, or -1. */
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
 * what the line predicts, and the difference. The coefficients say what the
 * model believes; the residuals are where it is wrong, and that is where the
 * shape of the error shows: a curve the line cannot follow, a group whose
 * spread grows with its prediction, the one row that is not like the others.
 * No summary statistic shows those; R2 and the residual SD both average them
 * away.
 *
 * It costs a SECOND PASS over the training file, not a copy of it in memory:
 * the fit forgets each row as it goes, so the rows have to be read again to be
 * subtracted from. Memory stays a function of the model. */
/* only: fit just that group, or NULL for every group in the file. */
int process_train_residuals(const char *csv_path, const char *only, FILE *out,
                            FILE *resid, struct fit_summary *sum);

/* Release what scoring loaded. Idempotent. */
void process_free(void);

#endif /* PROCESS_H */
