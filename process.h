/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* process.h -- THE slot. Two directions of the same model: process() scores one
 * case against the fitted coefficients, process_train() fits the coefficients
 * from a training file. main.c calls one or the other and prints the result. */
#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>

/* Score one case: "GROUP,x1,...,xp" in, "GROUP prediction=... trim=..." out.
 * The coefficient table is loaded on first use, from the paths in
 * system.properties (coef.file / trim.file), and its header decides how many
 * terms a case has. Returns 0 on success, -1 on a malformed case, an unknown
 * group, an unreadable table, or out too small. */
int process(const char *input, char *out, size_t outsz);

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

/* Fit one group's line from a training CSV of "GROUP,LOS,<terms>" rows, whose
 * header names the terms. group selects the rows; "*" pools every row in the
 * file under that name. Writes a complete two-line coefficient file into out --
 * the header, a newline, then the fitted row -- so
 *   linearr -t train.csv -g 001 > conf/coefficients.csv
 * produces a table the scorer can read straight back. info may be NULL.
 * Returns 0, or -1 if the file will not open, a row is malformed, the group has
 * no rows at all, or out is too small. A group with few rows still fits: see
 * info.df before believing the result. */
int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info);

/* Release what process() loaded lazily. Idempotent. */
void process_free(void);

#endif /* PROCESS_H */
