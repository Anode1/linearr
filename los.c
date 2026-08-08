/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* los.c -- see los.h.
 *
 * The one heap this program sanctions is here: the coefficient table, one
 * struct los_model per group, held in the hash table for the life of the run.
 * It is bounded by the number of GROUPS in the table, never by the number of
 * cases scored, and it is freed on every path by los_free(). The schema is a
 * fixed array. Cases themselves are stack objects, processed one at a time and
 * forgotten -- scoring ten cases and scoring ten million cost the same memory.
 * Nothing else allocates. */
#include "los.h"
#include "csv.h"
#include "hash.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GROUP_BUCKETS 1024

static struct hash *models;
static char         var_name[LOS_MAX_VARS][LOS_NAME_MAX];
static int          nvars;
static long         ngroups;

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
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
        if (names[i][0] == '\0' || strlen(names[i]) >= LOS_NAME_MAX) {
            debug("los: column %d has an empty or over-long name", i + 1);
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
 * or trailing text after the number. A typo in a table becomes an error rather
 * than a zero coefficient nobody notices. */
static int parse_num(const char *s, double *out) {
    char *end;
    double v;
    if (s[0] == '\0') return -1;
    v = strtod(s, &end);
    while (*end == ' ') end++;
    if (*end != '\0') return -1;
    *out = v;
    return 0;
}

static int copy_group(char *dst, size_t dstsz, const char *src) {
    if (src[0] == '\0' || strlen(src) >= dstsz) return -1;
    strcpy(dst, src);                           /* checked on the line above */
    return 0;
}

static int load_coefficients(const char *path) {
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp = fopen(path, "r");
    int    rc = -1, n, i;
    long   rows = 0;

    if (!fp) { debug("los: cannot open %s", path); return -1; }

    if (csv_next(fp, line, sizeof line) != 1) {
        debug("los: %s has no header", path);
        goto cleanup;
    }
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        debug("los: %s wants GROUP,Intercept and at least one term", path);
        goto cleanup;
    }
    /* The header IS the model's column order: whatever it names, in that order,
     * is what x[] and b[] mean from here on. */
    if (los_schema_set(field + 2, n - 2) != 0) goto cleanup;

    while ((n = csv_next(fp, line, sizeof line)) == 1) {
        struct los_model *m;
        char   group[GROUP_MAX];
        double v;

        if (csv_split(line, field, CSV_MAX_FIELDS) != nvars + 2) {
            debug("los: %s row %ld does not have %d columns", path, rows + 1, nvars + 2);
            goto cleanup;
        }
        if (copy_group(group, sizeof group, field[0]) != 0) {
            debug("los: %s row %ld has a bad group '%s'", path, rows + 1, field[0]);
            goto cleanup;
        }

        m = xmalloc(sizeof *m);
        m->trim_addition = 0.0;
        if (parse_num(field[1], &m->intercept) != 0) {
            free(m);
            debug("los: %s group %s has a bad intercept", path, group);
            goto cleanup;
        }
        for (i = 0; i < nvars; i++) {
            if (parse_num(field[i + 2], &v) != 0) {
                free(m);
                debug("los: %s group %s has a bad %s", path, group, var_name[i]);
                goto cleanup;
            }
            m->b[i] = v;
        }
        if (hash_get(models, group) == NULL) ngroups++;
        free(hash_put(models, group, m));       /* free a duplicate group's row */
        rows++;
    }
    if (n < 0) { debug("los: %s has an over-long line", path); goto cleanup; }

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
    int    rc = -1, n;

    if (!models) { debug("los: no coefficient table to attach trims to"); return -1; }

    fp = fopen(path, "r");
    if (!fp) { debug("los: cannot open %s", path); return -1; }

    if (csv_next(fp, line, sizeof line) != 1) {
        debug("los: %s has no header", path);
        goto cleanup;
    }
    while ((n = csv_next(fp, line, sizeof line)) == 1) {
        struct los_model *m;
        double v;

        if (csv_split(line, field, CSV_MAX_FIELDS) != 2) {
            debug("los: %s wants exactly GROUP,trim_addition", path);
            goto cleanup;
        }
        if (parse_num(field[1], &v) != 0) {
            debug("los: %s group %s has a bad trim addition", path, field[0]);
            goto cleanup;
        }
        /* A trim for a group with no coefficients is not an error: the trim
         * table may be the wider of the two. It has nothing to attach to. */
        m = hash_get(models, field[0]);
        if (m) m->trim_addition = v;
    }
    if (n < 0) { debug("los: %s has an over-long line", path); goto cleanup; }
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

static int append_num(char *out, size_t outsz, size_t *used, double v) {
    int w = snprintf(out + *used, outsz - *used, ",%.4f", v);
    if (w < 0 || (size_t)w >= outsz - *used) return -1;
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
    for (i = 0; i < scale; i++) p *= 10.0;
    return (v >= 0.0 ? floor(v * p + 0.5) : ceil(v * p - 0.5)) / p;
}
