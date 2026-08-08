/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* los.c -- see los.h.
 *
 * One of this program's three heap users is here: the coefficient table, one
 * struct los_model per group, held in the hash table for the life of the run.
 * It is bounded by the number of GROUPS in the table, never by the number of
 * cases scored, and it is freed on every path by los_free(). The schema is a
 * fixed array. Cases themselves are stack objects, processed one at a time and
 * forgotten -- scoring ten cases and scoring ten million cost the same memory.
 *
 * The other two are params.c's config table and, while `-t` fits every group,
 * one accumulator per group in process.c. This comment used to say "nothing
 * else allocates", which was false the day it was written: params.c was already
 * there. A count is a claim like any other. */
#include "los.h"
#include "csv.h"
#include "hash.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>
#include <float.h>

#define GROUP_BUCKETS 1024

static char reason[256] = "";

/* Why the last load failed. The caller used to print one sentence -- "X is not
 * a coefficient table" -- for about ten distinct causes, with the real one
 * visible only under -d. */
const char *los_error(void) { return reason; }

static int refuse(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof reason, fmt, ap);
    va_end(ap);
    debug("los: %s", reason);
    return -1;
}

static struct hash *models;
static char         var_name[LOS_MAX_VARS][LOS_NAME_MAX];
static int          nvars;
static long         ngroups;

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        /* unsigned char, so a byte >= 0x80 is not implementation-defined here
         * (MISRA 10.3); both operands are widened the same way either way, but
         * "works by symmetry" is not a thing to leave in a header comparison. */
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

int los_schema_set(char *const *names, int n) {
    int i;

    if (n < 1 || n > LOS_MAX_VARS) {
        debug("los: %d terms is outside 1..%d", n, LOS_MAX_VARS);
        return -1;
    }
    for (i = 0; i < n; i++) {
        int j;
        if (names[i][0] == '\0' || strlen(names[i]) >= LOS_NAME_MAX) {
            debug("los: column %d has an empty or over-long name", i + 1);
            return -1;
        }
        /* Two columns a user cannot tell apart are refused rather than ranked.
         * Lookup by name is case-insensitive, so "A" and "a" collide: with both
         * present, `linearr G a=1` silently set column "A" and there was no way
         * to address the other one at all. */
        for (j = 0; j < i; j++)
            if (ci_equal(names[i], names[j])) {
                debug("los: columns %d and %d are both '%s' (names are matched "
                      "without regard to case)", j + 1, i + 1, names[i]);
                return -1;
            }
    }
    /* Only once every name is known good, so a rejected header leaves the
     * previous schema intact rather than half-replaced. */
    for (i = 0; i < n; i++)
        strcpy(var_name[i], names[i]);          /* length checked above */
    nvars = n;
    debug("los: schema of %d terms", n);
    return 0;
}

int los_nvars(void) { return nvars; }

const char *los_var_name(int i) {
    return (i >= 0 && i < nvars) ? var_name[i] : NULL;
}

int los_var_index(const char *name) {
    int i;
    for (i = 0; i < nvars; i++)
        if (ci_equal(var_name[i], name)) return i;
    return -1;
}

long los_ngroups(void) { return ngroups; }

/* strtod that refuses what atof would have accepted silently: an empty field,
 * trailing text -- and the three the earlier version of this comment claimed to
 * catch and did not. strtod happily returns nan for "nan", inf for "inf" and
 * for 1e400 (with ERANGE), and reads "0x10" as 16. Each of those loaded into a
 * coefficient table without complaint, scored "prediction=nan", and exited 0.
 * A number that is not finite is not a number we can publish. */
static int parse_num(const char *s, double *out) {
    char *end;
    double v;

    if (s[0] == '\0') return -1;
    errno = 0;
    v = strtod(s, &end);
    if (errno == ERANGE) return -1;             /* 1e400, and denormal underflow */
    while (*end == ' ') end++;
    if (*end != '\0') return -1;
    if (!isfinite(v)) return -1;                /* nan, inf, -inf */
    *out = v;
    return 0;
}

static int copy_group(char *dst, size_t dstsz, const char *src) {
    if (src[0] == '\0' || strlen(src) >= dstsz) return -1;
    strcpy(dst, src);                           /* checked on the line above */
    return 0;
}

/* A line that begins with '#' but has exactly the shape of a data row is almost
 * certainly data whose group code starts with '#', not a comment. Saying so
 * beats dropping the row and reporting one group fewer than the file has. */
static int comment_is_data_shaped(char *line, int want) {
    char *field[CSV_MAX_FIELDS];
    return csv_split(line, field, CSV_MAX_FIELDS) == want;
}

static int load_coefficients(const char *path) {
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp = fopen(path, "r");
    int    rc = -1, n, i;
    long   rows = 0;

    if (!fp) return refuse("cannot open %s", path);

    while ((n = csv_next(fp, line, sizeof line)) == 2)
        ;                                   /* leading comments precede a header */
    if (n != 1) {
        refuse("%s has no header line", path);
        goto cleanup;
    }
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        refuse("%s needs a header of GROUP, Intercept and at least one term; "
               "this one has %d column%s", path, n, n == 1 ? "" : "s");
        goto cleanup;
    }
    /* A "header" whose every field is a number is not a header. It used to be
     * adopted as the schema, so the terms were named "6.4832" and "0", and the
     * error a user finally saw complained about a missing TERM. */
    {   int numeric = 1, k;
        double tmp;
        for (k = 1; k < n; k++)
            if (parse_num(field[k], &tmp) != 0) { numeric = 0; break; }
        if (numeric) {
            refuse("%s starts with a row of numbers where its header should be: "
                   "a coefficient file needs GROUP,Intercept,<term names>", path);
            goto cleanup;
        }
    }
    /* The header IS the model's column order: whatever it names, in that order,
     * is what x[] and b[] mean from here on. */
    if (los_schema_set(field + 2, n - 2) != 0) {
        refuse("%s does not name %d usable terms: they must be non-empty, under "
               "%d characters, and distinct ignoring case", path, n - 2, LOS_NAME_MAX);
        goto cleanup;
    }

    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        struct los_model *m;
        char   group[GROUP_MAX];
        double v;

        if (n == 2) {
            if (comment_is_data_shaped(line, nvars + 2)) {
                refuse("%s has a line beginning with '#' that has the shape of a "
                       "data row -- a group code cannot start with '#', because "
                       "the line reads as a comment", path);
                goto cleanup;
            }
            continue;
        }

        if (csv_split(line, field, CSV_MAX_FIELDS) != nvars + 2) {
            refuse("%s row %ld does not have %d columns", path, rows + 1, nvars + 2);
            goto cleanup;
        }
        if (copy_group(group, sizeof group, field[0]) != 0) {
            refuse("%s row %ld has an empty group, or one over %d characters",
                   path, rows + 1, GROUP_MAX - 1);
            goto cleanup;
        }
        /* Two rows for one group is a table its author did not mean to write.
         * Last-one-wins scored the second silently, so the file and the answer
         * disagreed and nothing said so. */
        if (hash_get(models, group) != NULL) {
            refuse("%s names group '%s' twice", path, group);
            goto cleanup;
        }

        m = xmalloc(sizeof *m);
        m->trim_addition = 0.0;
        if (parse_num(field[1], &m->intercept) != 0) {
            free(m);
            refuse("%s group %s: the intercept '%s' is not a finite number",
                   path, group, field[1]);
            goto cleanup;
        }
        for (i = 0; i < nvars; i++) {
            if (parse_num(field[i + 2], &v) != 0) {
                free(m);
                refuse("%s group %s: %s = '%s' is not a finite number",
                       path, group, var_name[i], field[i + 2]);
                goto cleanup;
            }
            m->b[i] = v;
        }
        ngroups++;
        free(hash_put(models, group, m));
        rows++;
    }
    if (n < 0) {
        refuse("%s has a line over %d bytes, or one holding a NUL byte",
               path, CSV_LINE_MAX - 2);
        goto cleanup;
    }

    debug("los: %ld groups from %s", rows, path);
    rc = 0;
cleanup:
    fclose(fp);
    return rc;
}

int los_load_trims(const char *path) {
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp;
    double first;
    int    rc = -1, n;

    if (!models) { debug("los: no coefficient table to attach trims to"); return -1; }

    fp = fopen(path, "r");
    if (!fp) { debug("los: cannot open %s", path); return -1; }

    /* Read the first line, and only DISCARD it if it is a header. It used to be
     * eaten unconditionally, so a headerless trim table silently lost its first
     * group's trim addition -- a wrong number, quietly, for one group only. */
    while ((n = csv_next(fp, line, sizeof line)) == 2)
        ;
    if (n != 1) {
        refuse("%s is empty", path);
        goto cleanup;
    }
    if (csv_split(line, field, CSV_MAX_FIELDS) == 2 && parse_num(field[1], &first) == 0) {
        struct los_model *m0 = hash_get(models, field[0]);
        if (m0) m0->trim_addition = first;      /* it was data, not a header */
        debug("los: %s has no header line; treating the first line as data", path);
    }

    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        struct los_model *m;
        double v;

        if (n == 2) continue;
        if (csv_split(line, field, CSV_MAX_FIELDS) != 2) {
            refuse("%s wants exactly GROUP,trim_addition on every line", path);
            goto cleanup;
        }
        if (parse_num(field[1], &v) != 0) {
            refuse("%s group %s: the trim addition '%s' is not a finite number",
                   path, field[0], field[1]);
            goto cleanup;
        }
        /* A trim for a group with no coefficients is not an error: the trim
         * table may be the wider of the two. It has nothing to attach to. */
        m = hash_get(models, field[0]);
        if (m) m->trim_addition = v;
    }
    if (n < 0) {
        refuse("%s has a line over %d bytes, or one holding a NUL byte",
               path, CSV_LINE_MAX - 2);
        goto cleanup;
    }
    rc = 0;
cleanup:
    fclose(fp);
    return rc;
}

int los_load(const char *coef_path) {
    los_free();
    models = hash_create(GROUP_BUCKETS);

    if (load_coefficients(coef_path) != 0) { los_free(); return -1; }
    return 0;
}

const struct los_model *los_model_get(const char *group) {
    return models ? (const struct los_model *)hash_get(models, group) : NULL;
}

void los_free(void) {
    nvars = 0;
    ngroups = 0;
    if (!models) return;
    hash_call(models, free);                    /* the struct los_model per group */
    hash_delete(models);                        /* keys + table                   */
    models = NULL;
}

/* Both parsers split a private copy: csv_split works in place, and the caller's
 * line is const. Bounded by CSV_LINE_MAX, on the stack, allocating nothing. */
static int parse_row(const char *line, struct los_case *c, double *los,
                     int xoff) {
    char  buf[CSV_LINE_MAX];
    char *field[CSV_MAX_FIELDS];
    int   i;

    if (nvars < 1) { debug("los: no schema yet"); return -1; }
    if (strlen(line) >= sizeof buf) return -1;
    strcpy(buf, line);                          /* checked on the line above */

    if (csv_split(buf, field, CSV_MAX_FIELDS) != nvars + xoff) return -1;
    if (copy_group(c->group, sizeof c->group, field[0]) != 0) return -1;
    if (los && parse_num(field[1], los) != 0) return -1;

    for (i = 0; i < nvars; i++)
        if (parse_num(field[i + xoff], &c->x[i]) != 0) return -1;
    return 0;
}

int los_parse_case(const char *line, struct los_case *c) {
    return parse_row(line, c, NULL, 1);
}

int los_parse_training(const char *line, struct los_case *c, double *los) {
    return parse_row(line, c, los, 2);
}

/* The two bounded appends the formatters are built from: each writes into the
 * space that is left, and a result that does not fit is an error rather than a
 * truncation nobody sees. */
static int append_str(char *out, size_t outsz, size_t *used, const char *s) {
    int w = snprintf(out + *used, outsz - *used, "%s", s);
    if (w < 0 || (size_t)w >= outsz - *used) return -1;
    *used += (size_t)w;
    return 0;
}

/* A coefficient is a model parameter, not a published figure. At the old %.4f
 * every coefficient below 5e-5 was written as 0.0000, so a fit that reported
 * R2=1.0000 wrote a CONSTANT model to disk, and the round trip the README
 * recommends -- fit, redirect, score -- silently produced a different model
 * from the one that was fitted. predict.scale still governs the PREDICTION,
 * where rounding is part of the answer; it has no business here.
 *
 * The fix is the SHORTEST representation that reads back as the same double,
 * not simply the longest available. %.17g always round-trips but prints 5 as
 * 4.9999999999999991, which makes a table of exact values look like noise and
 * invites someone to "clean it up". Trying 15, 16, then 17 gives "5" and "2.5"
 * where the value really is 5 and 2.5, and spends the extra digits only where
 * they carry information. */
static int append_num(char *out, size_t outsz, size_t *used, double v) {
    char   buf[64];
    int    prec, w = 0;

    for (prec = 15; prec <= 17; prec++) {
        w = snprintf(buf, sizeof buf, ",%.*g", prec, v);
        if (w < 0 || (size_t)w >= sizeof buf) return -1;
        if (strtod(buf + 1, NULL) == v) break;     /* reads back identical */
    }
    if ((size_t)w >= outsz - *used) return -1;
    memcpy(out + *used, buf, (size_t)w + 1);
    *used += (size_t)w;
    return 0;
}

int los_format_header(char *out, size_t outsz) {
    size_t used = 0;
    int    i;

    if (nvars < 1 || outsz == 0) return -1;
    if (append_str(out, outsz, &used, "GROUP,Intercept") != 0) return -1;
    for (i = 0; i < nvars; i++) {
        if (append_str(out, outsz, &used, ",") != 0) return -1;
        if (append_str(out, outsz, &used, var_name[i]) != 0) return -1;
    }
    return 0;
}

int los_format_model(const char *group, const struct los_model *m,
                     char *out, size_t outsz) {
    size_t used = 0;
    int    i;

    if (nvars < 1 || outsz == 0) return -1;
    if (append_str(out, outsz, &used, group) != 0) return -1;
    if (append_num(out, outsz, &used, m->intercept) != 0) return -1;
    for (i = 0; i < nvars; i++)
        if (append_num(out, outsz, &used, m->b[i]) != 0) return -1;
    return 0;
}

double los_predict(const struct los_model *m, const struct los_case *c) {
    double y = m->intercept;
    int    i;
    for (i = 0; i < nvars; i++) y += m->b[i] * c->x[i];
    return y;
}

double los_trim_point(const struct los_model *m, double prediction) {
    return prediction + m->trim_addition;
}

double los_round(double v, int scale) {
    double p = 1.0;
    int    i;

    if (!isfinite(v)) return v;
    for (i = 0; i < scale; i++) p *= 10.0;

    /* v*p overflowed to inf for a perfectly finite v -- 1.8e304 at scale 4 --
     * and the infinity was then printed as a prediction. Nothing useful is lost
     * by declining to round a number with no fractional part left to round. */
    if (fabs(v) > DBL_MAX / p) return v;

    /* round() is round-half-away-from-zero and correctly rounded. The old
     * floor(v*p + 0.5) form did the rounding twice: the addition itself rounds,
     * so 0.49999999999999994 -- the largest double below one half -- became
     * exactly 1.0 before floor() ever saw it, and rounded up. */
    return round(v * p) / p;
}
