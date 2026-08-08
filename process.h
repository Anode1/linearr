/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* process.h -- THE slot. Two directions of the same model: score a case against
 * the fitted coefficients, or fit the coefficients from a training file.
 * main.c calls one or the other and prints the result; nothing here prints. */
#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>

/* --- scoring --------------------------------------------------------------
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

/* Score one case written as a row: "GROUP,x1,...,xp", one value per term in the
 * coefficient file's column order. Returns 0, or -1 -- process_error() then
 * says why. */
int process(const char *input, char *out, size_t outsz);

/* Score one case written by name: a group, plus n assignments of the form
 * "term=value". Any term not mentioned is 0, which is what makes this usable
 * with a wide model -- naming the two things that are true beats typing 254
 * zeroes. Returns 0, or -1 (see process_error). */
int process_named(const char *group, char *const *assign, int n,
                  char *out, size_t outsz);

/* Why the last process()/process_named()/process_init() returned -1. Never
 * NULL; a sentence fragment fit to follow "cannot score X: ". */
const char *process_error(void);

/* --- the loaded schema, for `--terms` -------------------------------------- */

int         process_nterms(void);          /* terms in the loaded table       */
long        process_ngroups(void);         /* groups in the loaded table      */
const char *process_term_name(int i);      /* NULL if i is out of range       */
const char *process_coef_path(void);       /* the table actually opened       */

/* --- fitting --------------------------------------------------------------- */

/* What the fit found, for the report on stderr. */
struct fit_info {
    long   rows;        /* training rows used                              */
    int    pinned;      /* terms the sample could not identify, set to 0   */
    long   df;          /* residual degrees of freedom: rows minus the
                           terms actually identified. At or below 0 the
                           line passes through every point by construction
                           and r2 is 1 no matter what the data says.       */
    double r2;          /* coefficient of determination, -1 if undefined   */
};

/* Fit one group's line from a training CSV of "GROUP,VALUE,<terms>" rows, whose
 * header names the terms. group selects the rows; "*" pools every row in the
 * file under that name. Writes a complete two-line coefficient file into out --
 * the header, a newline, then the fitted row -- so
 *   linearr -t train.csv -g 001 > conf/coefficients.csv
 * produces a table the scorer can read straight back. info may be NULL.
 * Returns 0, or -1 (see process_error). A group with few rows still fits: see
 * info.df before believing the result. */
int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info);

/* Release what scoring loaded. Idempotent. */
void process_free(void);

#endif /* PROCESS_H */
