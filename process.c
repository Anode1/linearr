/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* process.c -- see process.h. The two halves of the model: fit the line from a
 * training file, then use the line to score a case.
 *
 * The prediction is the intercept plus one coefficient-times-term product per
 * column, rounded half away from zero to four digits; the trim point is that
 * plus the group's trim addition, rounded to one. The rounding is part of the
 * answer, not presentation: the published figure is the rounded number. Both
 * scales are configurable, and so is the number of terms -- it comes from the
 * file's header, not from this file. */
#include "process.h"
#include "los.h"
#include "csv.h"
#include "regress.h"
#include "params.h"
#include "resolve.h"
#include "hash.h"
#include "common.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>

/* The model may not be wider than the fitter. Overriding one ceiling and not
 * the other would otherwise fail far away from here, as a schema that loads and
 * then a fit that refuses it, so it fails at compile time instead. */
typedef char los_fits_in_regress[(LOS_MAX_VARS <= REGRESS_MAX_VARS) ? 1 : -1];

/* A case line has to fit in a CSV line, and its fields in the field array. */
typedef char case_fits_in_csv[(LOS_MAX_VARS + 2 <= CSV_MAX_FIELDS) ? 1 : -1];

#define DEFAULT_COEF_FILE "conf/coefficients.csv"
#define DEFAULT_TRIM_FILE "conf/trim_additions.csv"
#define DEFAULT_PREDICT_SCALE 4
#define DEFAULT_TRIM_SCALE 1

/* The fitter's two matrices, in static storage rather than on the stack.
 * As automatics they were 606 KB and 531 KB, live at the same time, so the
 * default build needed 1.18 MB of contiguous stack and died with SIGSEGV and no
 * diagnostic under `ulimit -s 1024` -- on a program whose README sells embedded
 * and locked-down clinical targets, where a 128 KB thread stack is normal. The
 * same bytes in BSS are allocated once, cost the same, and cannot blow a stack.
 * The consequence is that a fit is not reentrant, which is true of this
 * single-threaded CLI anyway and is now written down instead of implied. */
static double fit_store[REGRESS_MAX_VARS * REGRESS_MAX_VARS + 2 * REGRESS_MAX_VARS];
static double fit_scratch[REGRESS_MAX_VARS * (REGRESS_MAX_VARS + 1)];
static double fit_beta[REGRESS_MAX_TERMS];

static const char *coef_override;
static const char *trim_override;
static int  trim_override_set;

void process_use_coef(const char *path) { coef_override = path; }
void process_use_trim(const char *path) { trim_override = path; trim_override_set = 1; }

static int  tables_loaded;
static char coef_path[RESOLVE_PATH_MAX];
static char err_buf[512] = "no error";

/* Set the reason and fail in one statement, so no path can return -1 while
 * leaving the previous run's explanation behind. */
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, sizeof err_buf, fmt, ap);
    va_end(ap);
    debug("%s", err_buf);
    return -1;
}

const char *process_error(void) { return err_buf; }

static const char *param_or(const char *key, const char *fallback) {
    const char *v = params_get(key);
    return (v && v[0]) ? v : fallback;
}

static int param_int(const char *key, int fallback) {
    const char *v = params_get(key);
    char *end;
    long n;
    if (!v || v[0] == '\0') return fallback;
    n = strtol(v, &end, 10);
    if (*end != '\0' || n < 0 || n > 9) return fallback;
    return (int)n;
}

static int ensure_tables(void) {
    const char *trim, *want;
    char path[RESOLVE_PATH_MAX];

    /* Both halves of this condition are load-bearing. The flag alone used to be
     * trusted, and it lies: los.c holds the schema and the table as module
     * state that something else can clear (los_free) or repurpose
     * (process_train, whose training header replaces the schema). Scoring then
     * proceeded against a schema of zero terms, and process_term_name handed
     * back NULL to a caller that had every reason to expect a string. Asking
     * los whether it still has a schema is self-healing: if it does not, we
     * load again. */
    if (tables_loaded && los_nvars() > 0) return 0;
    tables_loaded = 0;

    want = coef_override ? coef_override : param_or("coef.file", DEFAULT_COEF_FILE);
    if (resolve_file(want, path, sizeof path) != 0)
        return fail("cannot find the coefficient table %s -- point coef.file "
                    "in system.properties at yours, or fit one with -t", path);
    if (los_load(path) != 0)
        return fail("%s is not a coefficient table: it needs a "
                    "GROUP,Intercept,<terms> header and matching rows "
                    "(run with -d for the line)", path);
    snprintf(coef_path, sizeof coef_path, "%s", path);

    /* The trim table is optional in three distinguishable ways, and they are
     * not the same thing: named in the config and unreadable is a failure the
     * user asked for and must hear about; set to empty means deliberately none;
     * absent from the config means the built-in path, which is only a default
     * and may simply not be there. Treating all three as fatal made a
     * coefficient table produced by -t impossible to score against. */
    trim = trim_override_set ? trim_override : params_get("trim.file");
    if (trim_override_set && !trim) {
        debug("process: --no-trim; the trim point is the prediction");
    } else if (trim && trim[0] == '\0') {
        debug("process: trim.file is empty; the trim point is the prediction");
    } else {
        const char *tw = trim ? trim : DEFAULT_TRIM_FILE;
        if (resolve_file(tw, path, sizeof path) != 0 || los_load_trims(path) != 0) {
            if (trim) {                       /* asked for by name: their call */
                los_free();
                return fail("cannot use the trim table %s -- set trim.file empty "
                            "if there is none", path);
            }
            debug("process: no %s; the trim point is the prediction", DEFAULT_TRIM_FILE);
        }
    }

    tables_loaded = 1;
    return 0;
}

int process_init(char *err, size_t errsz) {
    if (ensure_tables() != 0) {
        snprintf(err, errsz, "%s", err_buf);
        return -1;
    }
    return 0;
}

int process_nterms(void)  { return los_nvars(); }
long process_ngroups(void) { return los_ngroups(); }
const char *process_term_name(int i) { return los_var_name(i); }
const char *process_coef_path(void) { return coef_path; }

/* The half both entry points share: a filled-in case becomes two rounded
 * numbers and a line of text. */
static int score_case(const struct los_case *c, char *out, size_t outsz) {
    const struct los_model *m;
    double prediction, trim;
    int pscale, tscale, w;

    m = los_model_get(c->group);
    if (!m)
        return fail("no group '%s' in %s (%ld groups there)",
                    c->group, coef_path, los_ngroups());

    pscale = param_int("predict.scale", DEFAULT_PREDICT_SCALE);
    tscale = param_int("trim.scale", DEFAULT_TRIM_SCALE);

    /* The trim point is built on the ROUNDED prediction, not the raw one: the
     * published figure is what the next step is entitled to use. */
    prediction = los_round(los_predict(m, c), pscale);
    trim       = los_round(los_trim_point(m, prediction), tscale);

    w = snprintf(out, outsz, "%s prediction=%.*f trim=%.*f",
                 c->group, pscale, prediction, tscale, trim);
    if (w < 0 || (size_t)w >= outsz)
        return fail("the result does not fit in %lu bytes", (unsigned long)outsz);
    return 0;
}

int process_named(const char *group, char *const *assign, int n,
                  char *out, size_t outsz) {
    struct los_case c;
    int i, j;

    if (ensure_tables() != 0) return -1;

    if (strlen(group) >= sizeof c.group)
        return fail("group '%s' is longer than %d characters", group,
                    (int)sizeof c.group - 1);
    strcpy(c.group, group);                     /* checked on the line above */
    for (i = 0; i < los_nvars(); i++) c.x[i] = 0.0;

    for (i = 0; i < n; i++) {
        const char *eq = strchr(assign[i], '=');
        char name[LOS_NAME_MAX];
        char *end;
        size_t len;
        double v;

        if (!eq)
            return fail("'%s' is not term=value", assign[i]);
        len = (size_t)(eq - assign[i]);
        if (len == 0 || len >= sizeof name)
            return fail("'%s' has no usable term name", assign[i]);
        memcpy(name, assign[i], len);
        name[len] = '\0';

        j = los_var_index(name);
        if (j < 0)
            return fail("no term '%s' in %s -- run --terms to list them",
                        name, coef_path);

        errno = 0;
        v = strtod(eq + 1, &end);
        if (end == eq + 1 || *end != '\0' || errno == ERANGE || !isfinite(v))
            return fail("'%s' is not a finite number", eq + 1);
        c.x[j] = v;
    }
    return score_case(&c, out, outsz);
}

int process(const char *input, char *out, size_t outsz) {
    struct los_case c;

    if (ensure_tables() != 0) return -1;

    if (los_parse_case(input, &c) != 0)
        return fail("expected a group and %d comma-separated values, "
                    "one per term", los_nvars());

    return score_case(&c, out, outsz);
}

int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info) {
    struct regress     r;
    struct regress_fit f;
    struct los_model   fitted;
    struct los_case    c;
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp;
    size_t used;
    int    rc = -1, n, i, pinned, nvars;
    int    pool = (!group || strcmp(group, "*") == 0);
    long   rows = 0, seen = 0;

    {   /* the same two places as the tables: your file first, then the ones
         * that shipped beside the program, so the examples work from anywhere */
        char path[RESOLVE_PATH_MAX];
        if (resolve_file(csv_path, path, sizeof path) != 0)
            return fail("cannot open the training file %s", path);
        fp = fopen(path, "r");
        if (!fp) return fail("cannot open the training file '%s'", path);
    }

    /* The training file's header defines the polynomial: GROUP, the observed
     * value, then one column per term. Add a column to the file and the fit has
     * that term -- nothing here counts the terms for itself. */
    if (csv_next(fp, line, sizeof line) != 1) {
        fail("%s has no header line", csv_path);
        goto cleanup;
    }
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        fail("%s needs a header of GROUP, the observed value, and at least "
             "one term", csv_path);
        goto cleanup;
    }
    if (los_schema_set(field + 2, n - 2) != 0) {
        fail("%s does not name %d usable terms: they must be 1..%d, non-empty, "
             "under %d characters, and distinct ignoring case (run with -d for "
             "which one)", csv_path, n - 2, LOS_MAX_VARS, LOS_NAME_MAX);
        goto cleanup;
    }
    /* The schema now belongs to this training file, not to the coefficient
     * table, so anything scoring afterwards must load the real one again. This
     * is set HERE and not on the success path: every `goto cleanup` below used
     * to leave the borrowed schema installed while tables_loaded still said the
     * tables were good, and a caller could then score a 24-term model with a
     * 2-term case. That is the same "a flag another module can invalidate is
     * not a fact" defect this file already fixed once, in the other direction. */
    tables_loaded = 0;

    nvars = los_nvars();
    if (regress_init(&r, nvars, fit_store) != 0) {
        fail("%s has %d terms, more than the fitter's %d", csv_path, nvars,
             REGRESS_MAX_VARS);
        goto cleanup;
    }
    /* One row at a time: read it, add it to the cross-products, forget it. The
     * file may be any size; the fitter's footprint is the same either way. */
    while ((n = csv_next(fp, line, sizeof line)) == 1) {
        double los;
        seen++;
        if (los_parse_training(line, &c, &los) != 0) {
            fail("%s row %ld: expected a group, a value, and %d terms",
                 csv_path, seen, nvars);
            goto cleanup;
        }
        if (!pool && strcmp(c.group, group) != 0) continue;
        if (regress_add(&r, c.x, los) != 0) {
            fail("%s row %ld holds a value that is not finite", csv_path, seen);
            goto cleanup;
        }
        rows++;
    }
    if (n < 0) { fail("%s has a line longer than %d bytes", csv_path, CSV_LINE_MAX); goto cleanup; }

    if (rows == 0) {
        fail("no rows for group '%s' in %s (%ld rows read)",
             pool ? "*" : group, csv_path, seen);
        goto cleanup;
    }

    if (regress_solve(&r, fit_beta, fit_scratch, &f) != 0) {
        fail("cannot fit group '%s': the result is not a finite line",
             pool ? "*" : group);
        goto cleanup;
    }
    pinned = f.pinned;

    fitted.intercept = fit_beta[0];
    for (i = 0; i < nvars; i++) fitted.b[i] = fit_beta[i + 1];
    fitted.trim_addition = 0.0;      /* the trim table is a separate input */

    /* Header, newline, row: a coefficient file, not a fragment of one. */
    if (los_format_header(out, outsz) != 0 ||
        (used = strlen(out)) + 2 > outsz) {
        fail("the fitted table does not fit in %lu bytes", (unsigned long)outsz);
        goto cleanup;
    }
    out[used++] = '\n';
    out[used] = '\0';
    if (los_format_model(pool ? "*" : group, &fitted, out + used, outsz - used) != 0) {
        fail("the fitted table does not fit in %lu bytes", (unsigned long)outsz);
        goto cleanup;
    }

    if (info) {
        info->rows      = rows;
        info->pinned    = pinned;
        info->df        = f.df;
        info->r2        = f.r2;
        info->condition = f.condition;
    }
    rc = 0;
cleanup:
    fclose(fp);
    return rc;
}

/* One accumulator per group, in first-seen order so the emitted table is
 * reproducible. The hash gives O(1) lookup per row; the list gives an order. */
struct group_fit {
    struct group_fit *next;
    char   group[GROUP_MAX];
    struct regress r;
    double storage[1];              /* really regress_storage(nvars) doubles */
};

static void group_fits_free(struct group_fit *head) {
    while (head) {
        struct group_fit *next = head->next;
        free(head);
        head = next;
    }
}

int process_train_all(const char *csv_path, FILE *out, struct fit_summary *sum) {
    struct group_fit *head = NULL, *tail = NULL, *g;
    struct hash      *index = NULL;
    struct los_case   c;
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    char   row[MAX_OUTPUT];
    FILE  *fp = NULL;
    size_t need;
    int    rc = -1, n, i, nvars = 0;
    long   rows = 0, seen = 0, groups = 0;

    if (sum) {
        sum->groups = 0; sum->rows = 0; sum->min_df = 0;
        sum->max_condition = 1.0; sum->pinned = 0;
    }

    {   char path[RESOLVE_PATH_MAX];
        if (resolve_file(csv_path, path, sizeof path) != 0)
            return fail("cannot open the training file %s", path);
        fp = fopen(path, "r");
        if (!fp) return fail("cannot open the training file '%s'", path);
    }

    if (csv_next(fp, line, sizeof line) != 1) {
        fail("%s has no header line", csv_path);
        goto cleanup;
    }
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        fail("%s needs a header of GROUP, the observed value, and at least one term",
             csv_path);
        goto cleanup;
    }
    if (los_schema_set(field + 2, n - 2) != 0) {
        fail("%s does not name %d usable terms (run with -d for which one)",
             csv_path, n - 2);
        goto cleanup;
    }
    tables_loaded = 0;                  /* the schema now belongs to this file */
    nvars = los_nvars();
    need  = regress_storage(nvars);
    index = hash_create(1024);

    /* One pass. A row is added to its group's accumulator and forgotten, so the
     * file may be any size; only the number of GROUPS costs memory. */
    while ((n = csv_next(fp, line, sizeof line)) == 1) {
        double los;
        seen++;
        if (los_parse_training(line, &c, &los) != 0) {
            fail("%s row %ld: expected a group, a value, and %d terms",
                 csv_path, seen, nvars);
            goto cleanup;
        }
        g = hash_get(index, c.group);
        if (!g) {
            g = xmalloc(sizeof *g + (need > 0 ? need - 1 : 0) * sizeof(double));
            g->next = NULL;
            memcpy(g->group, c.group, sizeof g->group);
            if (regress_init(&g->r, nvars, g->storage) != 0) {
                free(g);
                fail("%s has %d terms, more than the fitter's %d",
                     csv_path, nvars, REGRESS_MAX_VARS);
                goto cleanup;
            }
            free(hash_put(index, c.group, g));
            if (tail) tail->next = g; else head = g;
            tail = g;
            groups++;
        }
        if (regress_add(&g->r, c.x, los) != 0) {
            fail("%s row %ld holds a value that is not finite", csv_path, seen);
            goto cleanup;
        }
        rows++;
    }
    if (n < 0) {
        fail("%s has a line that is over-long or holds a NUL byte", csv_path);
        goto cleanup;
    }
    if (groups == 0) { fail("%s has no data rows", csv_path); goto cleanup; }

    if (los_format_header(row, sizeof row) != 0) {
        fail("the header does not fit in %lu bytes", (unsigned long)sizeof row);
        goto cleanup;
    }
    fprintf(out, "%s\n", row);

    for (g = head; g; g = g->next) {
        struct regress_fit f;
        struct los_model   fitted;

        if (regress_solve(&g->r, fit_beta, fit_scratch, &f) != 0) {
            fail("cannot fit group '%s': the result is not a finite line", g->group);
            goto cleanup;
        }
        fitted.intercept = fit_beta[0];
        for (i = 0; i < nvars; i++) fitted.b[i] = fit_beta[i + 1];
        fitted.trim_addition = 0.0;

        if (los_format_model(g->group, &fitted, row, sizeof row) != 0) {
            fail("group '%s' does not fit in %lu bytes", g->group,
                 (unsigned long)sizeof row);
            goto cleanup;
        }
        fprintf(out, "%s\n", row);

        if (sum) {
            sum->pinned += f.pinned;
            if (sum->groups == 0 || f.df < sum->min_df) sum->min_df = f.df;
            if (f.condition > sum->max_condition) sum->max_condition = f.condition;
            sum->groups++;
        }
    }
    if (sum) sum->rows = rows;
    rc = 0;
cleanup:
    if (fp) fclose(fp);
    if (index) hash_delete(index);      /* the models are freed by the list */
    group_fits_free(head);
    return rc;
}

void process_free(void) {
    los_free();
    tables_loaded = 0;
}
