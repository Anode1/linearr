/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* los.h -- the worked example: length of stay as a linear function of what was
 * done to the patient and who they are.
 *
 *   LOS = b0 + b1*term1 + ... + bp*termp
 *
 * One fitted line per group, plus that group's trim addition -- the day count
 * past which a stay stops being typical.
 *
 * THE TERMS ARE NOT COMPILED IN. The column names, and how many there are, come
 * from the header line of the coefficient or training file, so adding a term to
 * the polynomial is adding a column to a CSV: no edit here, no rebuild. That is
 * what makes this a length-of-stay program only by its example data -- point it
 * at a different table with different columns and it fits and scores that
 * instead. The ceiling is LOS_MAX_VARS. */
#ifndef LOS_H
#define LOS_H

#include "constants.h"

#include <stddef.h>

#define LOS_MAX_VARS  32        /* terms, excluding the intercept */
#define LOS_NAME_MAX  64        /* longest column name we will hold */

/* One case: its group, and one value per term of the current schema. Fixed
 * size -- cases are processed one at a time, never collected. */
struct los_case {
    char   group[GROUP_MAX];
    double x[LOS_MAX_VARS];
};

/* One group's fitted line. */
struct los_model {
    double intercept;
    double b[LOS_MAX_VARS];
    double trim_addition;       /* 0 when no trim table was loaded */
};

/* --- the schema: which terms the polynomial has, and in what order --------- */

/* Adopt n column names as the schema, replacing any previous one. Returns 0, or
 * -1 if n is out of range or a name is empty or too long. */
int         los_schema_set(char *const *names, int n);

int         los_nvars(void);            /* 0 until a schema is set */
const char *los_var_name(int i);        /* NULL if i is out of range */

/* --- the tables ----------------------------------------------------------- */

/* Load the coefficient table. Its header is
 *   GROUP,Intercept,<one column per term>
 * and its term columns BECOME the schema. Returns 0, or -1 if the file will not
 * open or a row is malformed. Calling it again replaces the table and schema. */
int los_load(const char *coef_path);

/* Load GROUP,trim_addition rows onto the table already loaded, so a caller that
 * has no trim table, or does not want one, simply does not call this and every
 * trim addition stays 0. Returns 0, or -1 if there is no table yet, the file
 * will not open, or a row is malformed. A group in this file that the
 * coefficient table does not have is skipped, not an error: the trim table is
 * allowed to be the wider of the two. */
int los_load_trims(const char *trim_path);

/* The model for a group, or NULL if the table does not have it. */
const struct los_model *los_model_get(const char *group);

/* Release the tables and the schema. Idempotent. */
void los_free(void);

/* --- rows ----------------------------------------------------------------- */

/* Parse one case, "GROUP,x1,...,xp" against the current schema. Returns 0, or
 * -1 on a bad group, a wrong field count, or a field that is not a number. */
int los_parse_case(const char *line, struct los_case *c);

/* Parse one training row, "GROUP,LOS,x1,...,xp" -- the case with the observed
 * length of stay in front of the terms. Returns 0 or -1, as above. */
int los_parse_training(const char *line, struct los_case *c, double *los);

/* Write the coefficient file's header line for the current schema. */
int los_format_header(char *out, size_t outsz);

/* Write a model as a coefficient row, "GROUP,intercept,b1,...,bp". Returns 0,
 * or -1 if out is too small. */
int los_format_model(const char *group, const struct los_model *m,
                     char *out, size_t outsz);

/* --- the arithmetic ------------------------------------------------------- */

/* The prediction: the intercept plus every coefficient times its term. */
double los_predict(const struct los_model *m, const struct los_case *c);

/* The trim point: the prediction plus the group's trim addition. */
double los_trim_point(const struct los_model *m, double prediction);

/* Round half away from zero to `scale` digits -- NOT what printf's %.*f does,
 * which rounds half to even, so a published figure could differ in the last
 * digit. The rounded number is the answer, not its presentation. */
double los_round(double v, int scale);

#endif /* LOS_H */
