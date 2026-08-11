/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* los.h, the worked example: length of stay as a linear function of the case.
 *
 *   LOS = b0 + b1*term1 + ... + bp*termp
 *
 * One fitted line per group, plus that group's trim addition: the day count
 * past which a stay stops being typical. The terms are not compiled in: the
 * column names and their count come from the coefficient or training file's
 * header, so adding a term is adding a CSV column. Ceiling LOS_MAX_VARS. */
#ifndef LOS_H
#define LOS_H

#include "constants.h"

#include <stddef.h>

/* Must not exceed the fitter's REGRESS_MAX_VARS, which process.c asserts at
 * compile time. Override both together. */
#ifndef LOS_MAX_VARS
#define LOS_MAX_VARS 256        /* terms, excluding the intercept   */
#endif
#define LOS_NAME_MAX  64        /* longest column name we will hold */

/* One case: its group, and one value per term of the current schema. Fixed
 * size: cases are processed one at a time, never collected. */
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

/* the schema: which terms the polynomial has, and in what order */

/* Adopt n column names as the schema, replacing any previous one. Returns 0, or
 * -1 if n is out of range, or a name is empty, too long, or a duplicate of
 * another ignoring case. */
int         los_schema_set(const char *const *names, int n);

/* The same from a whole training header, the response named rather than assumed
 * to be column 2. header[0] is the group, the named column the response, every
 * other column a term in the order it appears. Returns 0; -1 as los_schema_set
 * does; -2 if the name is not in the header; -3 if it names the group. */
int         los_schema_set_response(const char *const *header, int n,
                                    const char *want);

int         los_nvars(void);            /* 0 until a schema is set */
const char *los_var_name(int i);        /* NULL if i is out of range */

/* The index of a term by name, or -1 if the schema has no such column. Lets a
 * case be written "icu_indicator=1" rather than by field number.
 * Case-insensitive: a column name is a label, not an identifier. */
int         los_var_index(const char *name);

/* How many groups the loaded table holds. */
long long   los_ngroups(void);

/* the tables */

/* Load the coefficient table. Its header is
 *   group,intercept,<one column per term>
 * and its term columns become the schema. Returns 0, or -1 if the file will not
 * open or a row is malformed. Calling it again replaces table and schema. */
int los_load(const char *coef_path);

/* Load group,trim_addition rows onto the table already loaded. Not calling this
 * leaves every trim addition 0. Returns 0, or -1 if there is no table yet, the
 * file will not open, or a row is malformed. A group here that the coefficient
 * table does not have is skipped, not an error: the trim table may be wider. */
int los_load_trims(const char *trim_path);

/* Whether any trim table was loaded. Without one the trim point is only the
 * prediction again. */
int los_has_trims(void);

/* The response's name, from the training header, or "" if none was read. */
const char *los_response_name(void);

/* Why the last los_parse_training() or los_parse_case() refused a line: one
 * sentence naming the column and what was in it. Valid until the next call. */
const char *los_parse_error(void);

/* Remember the response column's name, for the `# response:` line a fitted
 * table carries. Set from the training header; los_response_name() reads it. */
void los_set_response_name(const char *name);

/* Why the last los_load/los_load_trims returned -1. Never NULL. */
const char *los_error(void);

/* The program's one number parser: a CSV field and a TERM=VALUE assignment both
 * pass it. There is exactly one because the file form and the named form of a
 * case must not disagree about what is a number. Refuses an empty field,
 * trailing text, hexadecimal, and any non-finite value. Returns 0, or -1 with
 * *why (unless NULL) set to a clause naming the fault: "'%s' %s". */
int los_parse_number(const char *s, double *out, const char **why);

/* The model for a group, or NULL if the table does not have it. */
const struct los_model *los_model_get(const char *group);

/* Release the tables and the schema. Idempotent. */
void los_free(void);

/* rows */

/* Parse one case, "group,x1,...,xp" against the current schema. Returns 0, or
 * -1 on a bad group, a wrong field count, or a field that is not a number. */
int los_parse_case(const char *line, struct los_case *c);

/* Parse one training row, "group,value,x1,...,xp": the case with the observed
 * value in front of the terms. Returns 0 or -1, as above. */
int los_parse_training(const char *line, struct los_case *c, double *los);

/* Write the coefficient file's header line for the current schema. */
int los_format_header(char *out, size_t outsz);

/* Write a model as a coefficient row, "group,intercept,b1,...,bp". Returns 0,
 * or -1 if out is too small. */
int los_format_model(const char *group, const struct los_model *m,
                     char *out, size_t outsz);

/* the arithmetic */

/* The prediction: the intercept plus every coefficient times its term. */
double los_predict(const struct los_model *m, const struct los_case *c);

/* The trim point: the prediction plus the group's trim addition. */
double los_trim_point(const struct los_model *m, double prediction);

/* Round half away from zero to `scale` digits. Not printf's %.*f, which rounds
 * half to even, so a published figure could differ in the last digit. */
double los_round(double v, int scale);

#endif /* LOS_H */
