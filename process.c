/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* process.c: see process.h. The two halves of the model: fit the line from a
 * training file, then use the line to score a case.
 *
 * The prediction is the intercept plus one coefficient-times-term product per
 * column, rounded half away from zero to four digits; the trim point is that
 * plus the group's trim addition, rounded to one. The rounding is part of the
 * answer, not presentation: the published figure is the rounded number. Both
 * scales are configurable, and so is the number of terms; it comes from the
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

/* The line buffers are derived from LOS_MAX_VARS and CSV_FIELD_MAX, which is
 * only correct while a field really is at most CSV_FIELD_MAX bytes. This is
 * the file that sees both headers, so it is where raising LOS_NAME_MAX stops
 * being free: a header of the widest names must still fit in a line. */
typedef char name_fits_in_field[(LOS_NAME_MAX + 1 <= CSV_FIELD_MAX) ? 1 : -1];
typedef char header_fits_in_line[
    ((LOS_MAX_VARS + 2) * (LOS_NAME_MAX + 1) + GROUP_MAX < CSV_LINE_MAX) ? 1 : -1];

#define DEFAULT_COEF_FILE "conf/coefficients.csv"
#define DEFAULT_TRIM_FILE "conf/trim_additions.csv"
#define DEFAULT_PREDICT_SCALE 4
#define DEFAULT_TRIM_SCALE 1

/* The fitter's two matrices, in static storage rather than on the stack.
 * As automatics they were 606 KB and 531 KB, live at the same time, so the
 * default build needed 1.18 MB of contiguous stack and died with SIGSEGV and no
 * diagnostic under `ulimit -s 1024`, on a program whose README sells embedded
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

/* A pinned term is written as 0 in the coefficient row, and 0 is also what an
 * estimated no-effect looks like. Redirecting a fit into a table therefore used
 * to LOSE the distinction between "we could not identify this" and "this does
 * nothing", and scoring a case that turns such a term on then produced a
 * confident extrapolation off the training data's column space. R writes NA for
 * an aliased term; statsmodels drops it. Here the count went to stderr and the
 * file said nothing.
 *
 * So the file carries a note. It is a '#' line, which every reader of these
 * files already skips, so the table still round-trips into the scorer
 * unchanged, but the fact survives the redirect and a human reading the table
 * can see which zeroes are claims and which are silences. Returns 0 if there was
 * nothing to say. */
static int format_pinned(const char *group, const struct regress_fit *f,
                         int nvars, char *out, size_t outsz) {
    size_t used = 0;
    int    i, w, any = 0, kind;

    for (i = 0; i < nvars; i++) if (f->term[i] != REGRESS_FITTED) any = 1;
    if (!any) return 0;

    w = snprintf(out, outsz, "# pinned %s:", group);
    if (w < 0 || (size_t)w >= outsz) return 0;
    used = (size_t)w;

    for (kind = REGRESS_CONSTANT; kind <= REGRESS_COLLINEAR; kind++) {
        int first = 1;
        for (i = 0; i < nvars; i++) {
            if (f->term[i] != kind) continue;
            w = snprintf(out + used, outsz - used, "%s%s",
                         first ? (kind == REGRESS_CONSTANT ? " constant " : " collinear ")
                               : ",",
                         los_var_name(i));
            if (w < 0 || (size_t)w >= outsz - used) return 1;   /* truncated, not dropped */
            used += (size_t)w;
            first = 0;
        }
    }
    return 1;
}

static char coef_path[RESOLVE_PATH_MAX];
static char err_buf[512] = "no error";

/* Set the reason and fail in one statement, so no path can return -1 while
 * leaving the previous run's explanation behind. */
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err_buf, sizeof err_buf, fmt, ap);
    va_end(ap);
    debug("%s", err_buf);
    return -1;
}

const char *process_error(void) { return err_buf; }

static const char *param_or(const char *key, const char *fallback) {
    const char *v = params_get(key);
    return (v && v[0]) ? v : fallback;
}

/* A scale outside 0..9 used to fall back to the default without a word, so
 * `predict.scale = 99` quietly produced four decimals and the config lied about
 * what the program was doing. A setting that cannot be honoured is an error. */
static int param_int(const char *key, int fallback, int *bad) {
    const char *v = params_get(key);
    char *end;
    long n;
    if (!v || v[0] == '\0') return fallback;
    n = strtol(v, &end, 10);
    if (*end != '\0' || n < 0 || n > 9) { if (bad) *bad = 1; return fallback; }
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
        return fail("cannot find the coefficient table %s; point coef.file "
                    "in system.properties at yours, or fit one with -t", path);
    if (los_load(path) != 0) return fail("%s", los_error());
    (void)snprintf(coef_path, sizeof coef_path, "%s", path);

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
                return fail("%s (pass --no-trim, or set trim.file empty, if "
                            "there is none)", los_error());
            }
            debug("process: no %s; the trim point is the prediction", DEFAULT_TRIM_FILE);
        }
    }

    {   int bad = 0;
        (void)param_int("predict.scale", DEFAULT_PREDICT_SCALE, &bad);
        (void)param_int("trim.scale", DEFAULT_TRIM_SCALE, &bad);
        if (bad) {
            los_free();
            return fail("predict.scale and trim.scale must be whole numbers "
                        "from 0 to 9");
        }
    }

    tables_loaded = 1;
    return 0;
}

int process_init(char *err, size_t errsz) {
    if (ensure_tables() != 0) {
        (void)snprintf(err, errsz, "%s", err_buf);
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

    pscale = param_int("predict.scale", DEFAULT_PREDICT_SCALE, NULL);
    tscale = param_int("trim.scale", DEFAULT_TRIM_SCALE, NULL);

    /* The trim point is built on the ROUNDED prediction, not the raw one: the
     * published figure is what the next step is entitled to use. */
    prediction = los_round(los_predict(m, c), pscale);
    trim       = los_round(los_trim_point(m, prediction), tscale);

    w = snprintf(out, outsz, "%s prediction=%.*f trim=%.*f",
                 c->group, pscale, prediction, tscale, trim);
    if (w < 0 || (size_t)w >= outsz)
        return fail("the result does not fit in %zu bytes", outsz);
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
            return fail("no term '%s' in %s; run --terms to list them",
                        name, coef_path);

        /* Trim, because csv_split does and the two forms of a case must not
         * disagree: "a=1 " was refused while "G,1 " was accepted. */
        errno = 0;
        v = strtod(eq + 1, &end);
        while (*end == ' ' || *end == '\t') end++;
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

/* Opening a training file is the same job for both fitters: resolve it, read
 * past any leading comments, and take the schema from the header. The header
 * defines the polynomial (GROUP, the observed value, then one column per
 * term), so adding a column to the file adds a term to the fit, and nothing here
 * counts the terms for itself.
 *
 * On return the file is positioned at the first data row and *nvars holds the
 * term count. *fpp is set as soon as the file is open, including on the failing
 * paths, so the caller's cleanup closes it either way. */
static int open_training(const char *csv_path, FILE **fpp, int *nvars) {
    char   path[RESOLVE_PATH_MAX];
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    int    n;

    *fpp   = NULL;
    *nvars = 0;

    /* the same two places as the tables: your file first, then the ones that
     * shipped beside the program, so the examples work from anywhere */
    if (resolve_file(csv_path, path, sizeof path) != 0)
        return fail("cannot open the training file %s", path);
    *fpp = fopen(path, "r");
    if (!*fpp) return fail("cannot open the training file '%s'", path);

    while ((n = csv_next(*fpp, line, sizeof line)) == 2)
        ;                                   /* leading comments precede a header */
    if (n != 1)
        return fail("%s has no header line", csv_path);
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3)
        return fail("%s needs a header of GROUP, the observed value, and at "
                    "least one term", csv_path);
    if (los_schema_set((const char *const *)(field + 2), n - 2) != 0)
        return fail("%s does not name %d usable terms: they must be 1..%d, "
                    "non-empty, under %d characters, and distinct ignoring case "
                    "(run with -d for which one)", csv_path, n - 2, LOS_MAX_VARS,
                    LOS_NAME_MAX);

    /* The schema now belongs to this training file, not to the coefficient
     * table, so anything scoring afterwards must load the real one again. This
     * is set HERE and not on the caller's success path: every `goto cleanup`
     * used to leave the borrowed schema installed while tables_loaded still
     * said the tables were good, and a caller could then score a 24-term model
     * with a 2-term case. That is the same "a flag another module can
     * invalidate is not a fact" defect this file already fixed once, in the
     * other direction. */
    tables_loaded = 0;
    *nvars = los_nvars();
    return 0;
}

/* Header, newline, row: a coefficient file, not a fragment of one. */
static int format_table(const char *group, const struct los_model *m,
                        char *out, size_t outsz) {
    size_t used;

    if (los_format_header(out, outsz) != 0) return -1;
    used = strlen(out);
    if (used + 2 > outsz) return -1;
    out[used++] = '\n';
    out[used]   = '\0';
    return los_format_model(group, m, out + used, outsz - used);
}

/* The solved coefficients as a model row. beta[0] is the intercept and the rest
 * follow the schema's column order; the trim table is a separate input, so a
 * freshly fitted model carries no trim addition. */
static void model_from_beta(struct los_model *m, int nvars) {
    int i;
    m->intercept = fit_beta[0];
    for (i = 0; i < nvars; i++) m->b[i] = fit_beta[i + 1];
    m->trim_addition = 0.0;
}

int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info) {
    struct regress     r;
    struct regress_fit f;
    struct los_model   fitted;
    struct los_case    c;
    char   line[CSV_LINE_MAX];
    FILE  *fp;
    int    rc = -1, n, pinned, nvars;
    int    pool = (!group || strcmp(group, "*") == 0);
    /* What the fitted row is labelled with, and what an error calls it: a
     * pooled fit is one line named '*', a named one keeps its own name. */
    const char *label = pool ? "*" : group;
    long   rows = 0, seen = 0;

    if (open_training(csv_path, &fp, &nvars) != 0) goto cleanup;

    if (regress_init(&r, nvars, fit_store) != 0) {
        fail("%s has %d terms, more than the fitter's %d", csv_path, nvars,
             REGRESS_MAX_VARS);
        goto cleanup;
    }
    /* One row at a time: read it, add it to the cross-products, forget it. The
     * file may be any size; the fitter's footprint is the same either way. */
    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        double los;
        if (n == 2) continue;
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
        fail("no rows for group '%s' in %s (%ld rows read)", label, csv_path, seen);
        goto cleanup;
    }

    if (regress_solve(&r, fit_beta, fit_scratch, &f) != 0) {
        fail("cannot fit group '%s': the result is not a finite line", label);
        goto cleanup;
    }
    pinned = f.pinned;

    model_from_beta(&fitted, nvars);
    if (format_table(label, &fitted, out, outsz) != 0) {
        fail("the fitted table does not fit in %zu bytes", outsz);
        goto cleanup;
    }
    {   char note[MAX_OUTPUT];
        if (format_pinned(label, &f, nvars, note, sizeof note)) {
            size_t used = strlen(out);
            if (used + strlen(note) + 2 < outsz) {
                out[used] = '\n';
                memcpy(out + used + 1, note, strlen(note) + 1);
            }
        }
    }

    if (info) {
        info->rows      = rows;
        info->pinned    = pinned;
        info->df        = f.df;
        info->r2        = f.r2;
        info->sigma     = f.sigma;
        info->condition = f.condition;
    }
    rc = 0;
cleanup:
    if (fp) fclose(fp);
    return rc;
}

/* One accumulator per group, in first-seen order so the emitted table is
 * reproducible. The hash gives O(1) lookup per row; the list gives an order. */
struct group_fit {
    struct group_fit *next;
    char   group[GROUP_MAX];
    struct regress r;
    double *beta;                   /* points into storage, after the fitter's */
    double storage[1];              /* regress_storage(nvars) + nvars + 1      */
};

static void group_fits_free(struct group_fit *head) {
    while (head) {
        struct group_fit *next = head->next;
        free(head);
        head = next;
    }
}

int process_train_all(const char *csv_path, FILE *out, struct fit_summary *sum) {
    return process_train_residuals(csv_path, out, NULL, sum);
}

int process_train_residuals(const char *csv_path, FILE *out, FILE *resid,
                            struct fit_summary *sum) {
    struct group_fit *head = NULL, *tail = NULL, *g;
    struct hash      *index = NULL;
    struct los_case   c;
    char   line[CSV_LINE_MAX];
    char   row[MAX_OUTPUT];
    FILE  *fp = NULL;
    size_t need;
    int    rc = -1, n, nvars = 0;
    long   rows = 0, seen = 0, groups = 0;

    if (sum) {
        sum->groups = 0; sum->rows = 0; sum->min_df = 0;
        sum->max_condition = 1.0; sum->pinned = 0; sum->max_sigma = -1.0;
    }

    if (open_training(csv_path, &fp, &nvars) != 0) goto cleanup;

    need  = regress_storage(nvars);
    index = hash_create(1024);

    /* One pass. A row is added to its group's accumulator and forgotten, so the
     * file may be any size; only the number of GROUPS costs memory. */
    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        double los;
        if (n == 2) continue;
        seen++;
        if (los_parse_training(line, &c, &los) != 0) {
            fail("%s row %ld: expected a group, a value, and %d terms",
                 csv_path, seen, nvars);
            goto cleanup;
        }
        g = hash_get(index, c.group);
        if (!g) {
            g = xmalloc(sizeof *g + (need + (size_t)nvars) * sizeof(double));
            g->next = NULL;
            g->beta = g->storage + need;     /* nvars+1 doubles, after the fitter */
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
        fail("the header does not fit in %zu bytes", sizeof row);
        goto cleanup;
    }
    (void)fprintf(out, "%s\n", row);

    for (g = head; g; g = g->next) {
        struct regress_fit f;
        struct los_model   fitted;

        if (regress_solve(&g->r, fit_beta, fit_scratch, &f) != 0) {
            fail("cannot fit group '%s': the result is not a finite line", g->group);
            goto cleanup;
        }
        /* Keep this group's line. The residual pass needs it after every group
         * has been solved, and fit_beta is one shared buffer the next group
         * overwrites. Without this the residual pass read whatever xmalloc had
         * left in the block and printed predictions around 1e161, the one
         * good thing about uninitialised memory being that it is obviously
         * wrong rather than plausibly wrong. */
        {   int b;
            for (b = 0; b <= nvars; b++) g->beta[b] = fit_beta[b];
        }
        model_from_beta(&fitted, nvars);

        if (los_format_model(g->group, &fitted, row, sizeof row) != 0) {
            fail("group '%s' does not fit in %zu bytes", g->group, sizeof row);
            goto cleanup;
        }
        (void)fprintf(out, "%s\n", row);
        {   char note[MAX_OUTPUT];
            if (format_pinned(g->group, &f, nvars, note, sizeof note))
                (void)fprintf(out, "%s\n", note);
        }

        if (sum) {
            sum->pinned += f.pinned;
            if (sum->groups == 0 || f.df < sum->min_df) sum->min_df = f.df;
            if (f.condition > sum->max_condition) sum->max_condition = f.condition;
            if (f.sigma > sum->max_sigma) sum->max_sigma = f.sigma;
            sum->groups++;
        }
    }
    if (sum) sum->rows = rows;

    /* The second pass. Re-read rather than remember: holding the rows would
     * make the footprint a function of the data, which is the one thing this
     * program does not do. */
    if (resid) {
        FILE *again;
        char  path[RESOLVE_PATH_MAX];
        int   k;

        if (resolve_file(csv_path, path, sizeof path) != 0 ||
            (again = fopen(path, "r")) == NULL) {
            fail("cannot re-read %s for the residuals", csv_path);
            goto cleanup;
        }
        fprintf(resid, "group,observed,predicted,residual\n");
        while ((n = csv_next(again, line, sizeof line)) == 2)
            ;                                    /* skip to past the header */
        while ((n = csv_next(again, line, sizeof line)) > 0) {
            double los, yhat;
            if (n == 2) continue;
            if (los_parse_training(line, &c, &los) != 0) continue;
            g = hash_get(index, c.group);
            if (!g) continue;
            yhat = g->beta[0];
            for (k = 0; k < nvars; k++) yhat += g->beta[k + 1] * c.x[k];
            fprintf(resid, "%s,%.12g,%.12g,%.12g\n", c.group, los, yhat, los - yhat);
        }
        fclose(again);
    }
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
