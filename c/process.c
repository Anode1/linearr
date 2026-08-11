/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* process.c: see process.h. Fit the line from a training file; score a case.
 * Prediction = intercept + sum of coefficient*term, rounded half away from zero
 * to 4 digits; trim point = that plus the group's trim addition, rounded to 1.
 * The rounded number is the answer. Both scales are configurable; the term count
 * comes from the file's header. */
/* Before any header, process.h included: glibc resolves <features.h> on the
 * first one. Defined later this does nothing and progress_now() uses time(). */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime */

#include "process.h"
#include "los.h"
#include "csv.h"
#include "regress.h"
#include "qr.h"
#include "diag.h"
#include "resolve.h"
#include "hash.h"
#include "common.h"
#include "constants.h"

#include <time.h>
#ifndef _WIN32
#include <unistd.h>   /* _POSIX_TIMERS, which <time.h> does not define */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>

/* The model may not be wider than the fitter; overriding one ceiling alone
 * would otherwise fail at run time. */
typedef char los_fits_in_regress[(LOS_MAX_VARS <= REGRESS_MAX_VARS) ? 1 : -1];

/* A case line has to fit in a CSV line, and its fields in the field array. */
typedef char case_fits_in_csv[(LOS_MAX_VARS + 2 <= CSV_MAX_FIELDS) ? 1 : -1];

/* The line buffers derive from LOS_MAX_VARS and CSV_FIELD_MAX: a field must be
 * at most CSV_FIELD_MAX bytes and the widest header must fit in a line. */
typedef char name_fits_in_field[(LOS_NAME_MAX + 1 <= CSV_FIELD_MAX) ? 1 : -1];
typedef char header_fits_in_line[
    ((LOS_MAX_VARS + 2) * (LOS_NAME_MAX + 1) + GROUP_MAX < CSV_LINE_MAX) ? 1 : -1];

#define DEFAULT_PREDICT_SCALE 4
#define DEFAULT_TRIM_SCALE 1

/* Static, not automatic: 606 KB + 531 KB live at once needs 1.18 MB of
 * contiguous stack, against the 128 KB a thread gets on the embedded targets
 * this is built for. Cost is the same in BSS. A fit is therefore not reentrant.
 * A fit still needs 188 KB of stack, so 128 KB does not fit a default-ceiling
 * fit; scoring fits in 113 KB. The remaining driver is LINEARR_MAX_OUTPUT, two
 * of which sit in process_train_residuals' frame beside a line buffer. A
 * 32-term build fits in 54 KB. Table in doc/INTERNALS.md. */
/* One buffer for either solver: both are O(terms^2) and data-independent, so
 * each formula is written out and the array takes the larger. */
#define REGRESS_STORE_DOUBLES ((size_t)REGRESS_MAX_VARS * (size_t)REGRESS_MAX_VARS \
                               + 2u * (size_t)REGRESS_MAX_VARS)
#define QR_STORE_DOUBLES (((size_t)REGRESS_MAX_VARS + 1u) * ((size_t)REGRESS_MAX_VARS + 2u) \
                          + 4u * ((size_t)REGRESS_MAX_VARS + 1u))
#define FIT_STORE_DOUBLES (QR_STORE_DOUBLES > REGRESS_STORE_DOUBLES \
                           ? QR_STORE_DOUBLES : REGRESS_STORE_DOUBLES)
typedef char fit_store_holds_regress[
    (FIT_STORE_DOUBLES >= REGRESS_STORE_DOUBLES) ? 1 : -1];
typedef char fit_store_holds_qr[
    (FIT_STORE_DOUBLES >= QR_STORE_DOUBLES) ? 1 : -1];

static double fit_store[FIT_STORE_DOUBLES];
/* Either solver: regress_solve's elimination workspace, or qr_solve's
 * re-triangularisation of the kept columns on a rank-deficient design. */
static double fit_scratch[(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2)];
static double fit_beta[REGRESS_MAX_TERMS];

static int use_qr;
void process_use_qr(int on) { use_qr = on; }
const char *process_solver(void) { return use_qr ? "QR" : "normal equations"; }

/* One accumulator, one of two shapes; the caller owns the storage. A union:
 * struct qr is 4 KB (column ranges inline), so both inline would cost a two-term
 * model 4232 bytes a group. is_qr is set at init, so init/add/solve agree. */
struct fitter {
    int is_qr;
    union {
        struct regress r;
        struct qr      q;
    } u;
};

static size_t fitter_storage(int nvars) {
    size_t a = regress_storage(nvars), b = qr_storage(nvars);
    return (a > b) ? a : b;
}

static int fitter_init(struct fitter *f, int nvars, double *storage) {
    f->is_qr = use_qr;
    return f->is_qr ? qr_init(&f->u.q, nvars, storage)
                    : regress_init(&f->u.r, nvars, storage);
}

/* Where each column sits, for the residual probes. The normal equations hold the
 * means; QR does not centre, so its rank test's column midpoints stand in. */
static void fitter_centers(const struct fitter *f, int nvars, double *out) {
    int j;
    if (!f->is_qr) {
        for (j = 0; j < nvars; j++) out[j] = f->u.r.mean[j];
    } else {
        for (j = 0; j < nvars; j++)
            out[j] = 0.5 * (f->u.q.colmin[j + 1] + f->u.q.colmax[j + 1]);
    }
}

static int fitter_add(struct fitter *f, const double *x, double y) {
    return f->is_qr ? qr_add(&f->u.q, x, y) : regress_add(&f->u.r, x, y);
}

static int fitter_solve(const struct fitter *f, double *beta, double *scratch,
                        struct regress_fit *fit) {
    return f->is_qr ? qr_solve(&f->u.q, beta, scratch, fit)
                    : regress_solve(&f->u.r, beta, scratch, fit);
}

static const char *coef_override;
static const char *response_override;
static const char *trim_override;
static int  trim_override_set;

void process_use_coef(const char *path) { coef_override = path; }
void process_use_trim(const char *path) { trim_override = path; trim_override_set = 1; }

static int  tables_loaded;

/* Why a solve failed, in the caller's words. -2 is the one cause a user can act
 * on: a column whose cross-products overflowed, which QR does not have. */
static const char *solve_failure(int rv, const char *group) {
    static char msg[320];
    if (rv == -2)
        (void)snprintf(msg, sizeof msg,
                       "group '%s': a term's values are so large "
                       "that squaring them overflowed (near 1e160 or beyond). "
                       "Rescale that column, or use --qr, which does not square "
                       "them. Run with -d to see which term", group);
    else if (rv == -3)
        /* Different advice from -2's: --qr spares the columns, but both solvers
         * square the response for its residual. */
        (void)snprintf(msg, sizeof msg,
                       "group '%s': the response's values are so "
                       "large that squaring them overflowed (near 1e160 or "
                       "beyond). Rescale that column; --qr squares the "
                       "response too, so it is not the remedy here", group);
    else
        (void)snprintf(msg, sizeof msg,
                       "group '%s': the result is not a finite line",
                       group);
    return msg;
}

/* A pinned term and an estimated no-effect are both 0, so a '#' note names the
 * pinned; readers skip '#', so the table round-trips. 0 if nothing to say. */
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
static char err_buf[RESOLVE_PATH_MAX + 512] = "no error";

/* Reason and failure in one statement: no path returns -1 with a stale err_buf. */
static int fail(const char *fmt, ...) LINEARR_PRINTF(1, 2);
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err_buf, sizeof err_buf, fmt, ap);
    va_end(ap);
    debug("%s", err_buf);
    return -1;
}

const char *process_error(void) { return err_buf; }

/* Progress to stderr, so stdout stays a coefficient file. First line after a
 * minute, then once a minute. The clock is read per million rows, not per row: a
 * syscall on some systems. Monotonic where there is one; time()'s NTP and DST
 * steps make elapsed negative. */
#define PROGRESS_AFTER  60      /* seconds of silence before the first line */
#define PROGRESS_EVERY  60      /* seconds between lines after that        */
#define PROGRESS_MASK   0xFFFFFL/* check the clock every 1,048,576 rows    */

int process_progress_due(long long rows, long elapsed, long since_last) {
    if ((rows & PROGRESS_MASK) != 0) return 0;
    if (elapsed < PROGRESS_AFTER) return 0;
    return since_last >= PROGRESS_EVERY;
}

static long prog_start, prog_last;

/* Seconds from some fixed point; only differences are ever used. */
static long progress_now(void) {
/* Not on Windows: MinGW declares CLOCK_MONOTONIC and _POSIX_TIMERS but its
 * default libraries have no clock_gettime, so the guard fails to link. */
#if defined(CLOCK_MONOTONIC) && defined(_POSIX_TIMERS) && !defined(_WIN32)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) return (long)ts.tv_sec;
#endif
    return (long)time(NULL);
}

static void progress_begin(void) { prog_start = prog_last = progress_now(); }

/* "1h 4m 12s", "4m 12s", "12s": the parts that are not zero. */
static void progress_elapsed(char *out, size_t outsz, long sec) {
    long h = sec / 3600, m = (sec % 3600) / 60, t = sec % 60;
    if (h)      (void)snprintf(out, outsz, "%ldh %ldm %lds", h, m, t);
    else if (m) (void)snprintf(out, outsz, "%ldm %lds", m, t);
    else        (void)snprintf(out, outsz, "%lds", t);
}

static void progress_row(long long rows, const char *what) {
    char ela[32];
    long now, elapsed;

    if ((rows & PROGRESS_MASK) != 0) return;      /* cheap test comes first */
    now     = progress_now();
    elapsed = now - prog_start;
    if (elapsed < 0) { prog_start = prog_last = now; return; }   /* clock moved */
    if (!process_progress_due(rows, elapsed, now - prog_last)) return;
    prog_last = now;
    progress_elapsed(ela, sizeof ela, elapsed);
    (void)fprintf(stderr, "%s: %lld rows in %s, %.2fM rows/s\n", what, rows, ela,
                  elapsed > 0 ? (double)rows / (double)elapsed / 1e6 : 0.0);
}

static int predict_scale = DEFAULT_PREDICT_SCALE;
static int trim_scale    = DEFAULT_TRIM_SCALE;

void process_use_response(const char *name) { response_override = name; }

int process_response_named(void) { return response_override != NULL; }

int process_set_scale(int decimals) {
    if (decimals < 0 || decimals > 9) return -1;
    predict_scale = decimals;
    return 0;
}

int process_set_trim_scale(int decimals) {
    if (decimals < 0 || decimals > 9) return -1;
    trim_scale = decimals;
    return 0;
}

static int ensure_tables(void) {
    const char *trim, *want;
    char path[RESOLVE_PATH_MAX];

    /* Both halves are load-bearing: los.c's schema is module state los_free
     * clears and process_train repurposes, so the flag alone is not a fact. */
    if (tables_loaded && los_nvars() > 0) return 0;
    tables_loaded = 0;

    /* Named, or nothing: there is no search path for a model. */
    if (!coef_override)
        return fail("no coefficient table. Name one with -c FILE, or fit one "
                    "first: linearr -t TRAIN.CSV > model.csv");
    want = coef_override;
    if (resolve_file(want, path, sizeof path) != 0)
        return fail("cannot find the coefficient table %s", path);
    if (los_load(path) != 0) return fail("%s", los_error());
    (void)snprintf(coef_path, sizeof coef_path, "%s", path);

    /* Optional two ways: named with --trim and unreadable is a failure; not
     * named means there is none, and the trim point is the prediction. */
    trim = trim_override_set ? trim_override : NULL;
    if (!trim || trim[0] == '\0') {
        debug("process: no --trim; the trim point is the prediction");
    } else if (resolve_file(trim, path, sizeof path) != 0) {
        /* los_load_trims never ran, so los_error() has nothing to say here. */
        los_free();
        return fail("cannot find the trim table %s (leave --trim off if there "
                    "is none)", path);
    } else if (los_load_trims(path) != 0) {
        /* Read BEFORE los_free, which clears it. */
        char why[LINEARR_MAX_OUTPUT];
        (void)snprintf(why, sizeof why, "%s", los_error());
        los_free();
        return fail("%s (leave --trim off if there is none)", why);
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
long long process_ngroups(void) { return los_ngroups(); }
const char *process_term_name(int i) { return los_var_name(i); }
const char *process_coef_path(void) { return coef_path; }

/* Shared by both entry points: a case becomes two rounded numbers and a line. */
static int score_case(const struct los_case *c, char *out, size_t outsz) {
    const struct los_model *m;
    double prediction, trim;
    int pscale, tscale, w;

    m = los_model_get(c->group);
    if (!m)
        return fail("no group '%s' in %s (%lld groups there)",
                    c->group, coef_path, los_ngroups());

    pscale = predict_scale;
    tscale = trim_scale;

    /* On the ROUNDED prediction: the published figure is what may be used. */
    prediction = los_round(los_predict(m, c), pscale);
    trim       = los_round(los_trim_point(m, prediction), tscale);

    /* No trim table: the trim point is the prediction, not a second quantity. */
    if (los_has_trims())
        w = snprintf(out, outsz, "%s prediction=%.*f trim=%.*f",
                     c->group, pscale, prediction, tscale, trim);
    else
        w = snprintf(out, outsz, "%s prediction=%.*f", c->group, pscale, prediction);
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

        /* Trimmed, hexadecimal refused, as csv_split and los.c do for a CSV
         * field. The scan skips what strtod skips, catching a=<tab>0x10. */
        {   const char *why;
            if (los_parse_number(eq + 1, &v, &why) != 0)
                return fail("'%s' %s", eq + 1, why);
        }
        c.x[j] = v;
    }
    return score_case(&c, out, outsz);
}

int process(const char *input, char *out, size_t outsz) {
    struct los_case c;

    if (ensure_tables() != 0) return -1;

    if (los_parse_case(input, &c) != 0)
        /* los.c already named the column and the reason. */
        return fail("%s", los_parse_error());

    return score_case(&c, out, outsz);
}

/* Resolve, skip leading comments, take the schema from the header: GROUP, the
 * observed value, one column per term, so a column added is a term added. On
 * return the file sits at the first data row, *nvars holds the term count, and
 * *fpp is set if the file opened at all, so the caller's cleanup closes it. */
static int open_training(const char *csv_path, FILE **fpp, int *nvars) {
    char   path[RESOLVE_PATH_MAX];
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    int    n;

    *fpp   = NULL;
    *nvars = 0;

    if (strcmp(csv_path, "-") == 0) {
        /* One pass, so a pipe suffices; --residuals is refused separately. */
        *fpp = stdin;
    } else {
        /* as for the tables: your file, then what shipped with the program */
        if (resolve_file(csv_path, path, sizeof path) != 0)
            return fail("cannot open the training file %s", path);
        *fpp = fopen(path, "r");
        if (!*fpp) return fail("cannot open the training file '%s'", path);
    }

    while ((n = csv_next(*fpp, line, sizeof line)) == 2)
        ;                                   /* leading comments precede a header */
    /* Why before what: a CR-only file arrives here as one refused line. */
    if (n < 0)
        return fail("%s %s", csv_path, csv_line_error(n, sizeof line));
    if (n != 1)
        return fail("%s has no header line", csv_path);
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        if (n == 1 && (strchr(field[0], ';') || strchr(field[0], '\t')))
            return fail("%s has no commas in its header, but does have %s. It "
                        "looks %s-separated; this program reads commas only",
                        csv_path, strchr(field[0], ';') ? "semicolons" : "tabs",
                        strchr(field[0], ';') ? "semicolon" : "tab");
        return fail("%s needs a header of group, the observed value, and at "
                    "least one term", csv_path);
    }
    /* Which column is predicted, and so which are terms: 2 unless --response. */
    if (response_override) {
        int rc = los_schema_set_response((const char *const *)field, n,
                                         response_override);
        if (rc == -2) {
            char have[CSV_LINE_MAX];
            size_t used = 0;
            int i;
            have[0] = '\0';
            for (i = 1; i < n; i++) {
                int w = snprintf(have + used, sizeof have - used, "%s%s",
                                 used ? ", " : "", field[i]);
                if (w < 0 || (size_t)w >= sizeof have - used) break;
                used += (size_t)w;
            }
            return fail("%s has no column '%s'. Its header offers: %s",
                        csv_path, response_override, have);
        }
        if (rc == -3)
            return fail("--response names '%s', which is the first column, and "
                        "the first column is the group", response_override);
        if (rc != 0)
            return fail("%s does not name usable terms once '%s' is taken as "
                        "the value: they must be 1..%d, non-empty, under %d "
                        "characters, and distinct ignoring case",
                        csv_path, response_override, LOS_MAX_VARS, LOS_NAME_MAX);
        los_set_response_name(response_override);
    } else {
        if (los_schema_set((const char *const *)(field + 2), n - 2) != 0)
            return fail("%s does not name %d usable terms: they must be 1..%d, "
                        "non-empty, under %d characters, and distinct ignoring "
                        "case (run with -d for which one)", csv_path, n - 2,
                        LOS_MAX_VARS, LOS_NAME_MAX);
        los_set_response_name(field[1]);
    }

    /* The schema now belongs to this training file, so later scoring must load
     * the real one again. Set here, not on the caller's success path: no
     * `goto cleanup` may leave it installed with tables_loaded still set. */
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

/* The solved coefficients as a model row: beta[0] the intercept, the rest in
 * schema order. The trim table is a separate input, so a fresh fit has none. */
static void model_from_beta(struct los_model *m, int nvars) {
    int i;
    m->intercept = fit_beta[0];
    for (i = 0; i < nvars; i++) m->b[i] = fit_beta[i + 1];
    m->trim_addition = 0.0;
}

int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info) {
    struct fitter      r;
    struct regress_fit f;
    struct los_model   fitted;
    struct los_case    c;
    char   line[CSV_LINE_MAX];
    FILE  *fp;
    int    rc = -1, n, pinned, nvars;
    int    pool = (!group || strcmp(group, "*") == 0);
    /* The fitted row's label: a pooled fit is one line named '*'. */
    const char *label = pool ? "*" : group;
    long long rows = 0, seen = 0;

    if (open_training(csv_path, &fp, &nvars) != 0) goto cleanup;

    progress_begin();
    if (fitter_init(&r, nvars, fit_store) != 0) {
        fail("%s has %d terms, more than the fitter's %d", csv_path, nvars,
             REGRESS_MAX_VARS);
        goto cleanup;
    }
    /* Read, add to the cross-products, forget. The file may be any size. */
    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        double los;
        if (n == 2) {
            /* A '#' line splitting into a data row's fields is a row whose group
             * starts with '#'; skipping it fits the file minus that group. */
            if (csv_comment_is_data_shaped(line, nvars + 2)) {
                fail("%s has a line beginning with '#' that has the shape of a "
                     "data row: a group code cannot start with '#', because "
                     "the line reads as a comment", csv_path);
                goto cleanup;
            }
            continue;
        }
        seen++;
        if (los_parse_training(line, &c, &los) != 0) {
            fail("%s row %lld: %s", csv_path, seen, los_parse_error());
            goto cleanup;
        }
        if (!pool && strcmp(c.group, group) != 0) continue;
        if (fitter_add(&r, c.x, los) != 0) {
            fail("%s row %lld holds a value that is not finite", csv_path, seen);
            goto cleanup;
        }
        rows++;
        progress_row(seen, "fitting");
    }
    if (n < 0) { fail("%s %s", csv_path, csv_line_error(n, sizeof line)); goto cleanup; }

    if (rows == 0) {
        fail("no rows for group '%s' in %s (%lld rows read)", label, csv_path, seen);
        goto cleanup;
    }

    {   int srv = fitter_solve(&r, fit_beta, fit_scratch, &f);
        if (srv != 0) { fail("%s", solve_failure(srv, label)); goto cleanup; }
    }
    pinned = f.pinned;

    model_from_beta(&fitted, nvars);
    if (format_table(label, &fitted, out, outsz) != 0) {
        fail("the fitted table does not fit in %zu bytes", outsz);
        goto cleanup;
    }
    {   char note[LINEARR_MAX_OUTPUT];
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
        info->sigma_is_bound = f.sigma_is_bound;
        info->condition = f.condition;
    }
    rc = 0;
cleanup:
    if (fp && fp != stdin) fclose(fp);
    return rc;
}

/* One accumulator per group, first-seen order so the table is reproducible: the
 * hash gives O(1) lookup per row, the list the order. */
struct group_fit {
    struct group_fit *next;
    char   group[GROUP_MAX];
    struct fitter r;
    struct diag   d;                /* its own, so groups are not pooled       */
    double *beta;
    /* Residual SD and a Welford pair for the response; the checks compare the
     * two for anything left to explain. Per group, so a group that fits exactly
     * is not judged by its neighbour's error. */
    double sigma;
    long long ny;                   /* long long, as every other row counter is:
                                       `long` overflows past 2^31 rows in one
                                       group on LLP64 and on 32-bit            */
    double ymean, ym2;
    double storage[1];              /* fitter + (nvars+1) beta + diag          */
};

/* The allocation below calls this, so reported and allocated cannot differ. */
size_t process_group_bytes(int nvars) {
    if (nvars < 1) return 0;
    return sizeof(struct group_fit)
         + (fitter_storage(nvars) + (size_t)nvars + diag_storage(nvars))
           * sizeof(double);
}

/* Dimensioned at the build ceiling: a two-term model pays for LOS_MAX_VARS. */
size_t process_model_bytes(void) {
    return sizeof(struct los_model);
}

static void group_fits_free(struct group_fit *head) {
    while (head) {
        struct group_fit *next = head->next;
        free(head);
        head = next;
    }
}

int process_train_all(const char *csv_path, FILE *out, struct fit_summary *sum) {
    return process_train_residuals(csv_path, NULL, out, NULL, NULL, sum);
}

int process_train_residuals(const char *csv_path, const char *only, FILE *out,
                            FILE *resid, FILE *stats, struct fit_summary *sum) {
    struct group_fit *head = NULL, *tail = NULL, *g;
    struct hash      *index = NULL;
    struct los_case   c;
    char   line[CSV_LINE_MAX];
    char   row[LINEARR_MAX_OUTPUT];
    FILE  *fp = NULL;
    size_t need;
    int    rc = -1, n, nvars = 0;
    long long rows = 0, seen = 0, groups = 0;
    int    first_stat = 1;
    /* -g '*' pools every row into one line named '*'; not a literal group. */
    const int pool = (only != NULL && strcmp(only, "*") == 0);

    /* Checked before anything is read or written: the refusal must not follow
     * the table to stdout. */
    if (resid && strcmp(csv_path, "-") == 0)
        return fail("--residuals needs to read the training data a second time, "
                    "to subtract each row from its own prediction, and a pipe "
                    "cannot be rewound. Give it a file instead of -");

    if (sum) {
        sum->groups = 0; sum->rows = 0; sum->min_df = 0;
        sum->max_condition = 1.0; sum->pinned = 0; sum->max_sigma = -1.0; sum->sigma_is_bound = 0;
        sum->min_r2 = -1.0;
        sum->groups_flat_y = 0; sum->groups_r2_lost = 0;
        sum->curved_term = -1; sum->curved_t = 0.0; sum->curved_pow = 0;
        sum->fitted_t = 0.0; sum->spread_t = 0.0; sum->worst_group[0] = '\0';
    }

    if (open_training(csv_path, &fp, &nvars) != 0) goto cleanup;

    progress_begin();
    need  = fitter_storage(nvars);
    index = hash_create(1024);

    /* One pass. A row joins its group's accumulator and is forgotten, so only
     * the number of GROUPS costs memory. */
    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        double los;
        if (n == 2) {
            /* As in process_train: a data-shaped '#' line is a swallowed row.
             * The second pass keeps its plain skip; this refusal ran first. */
            if (csv_comment_is_data_shaped(line, nvars + 2)) {
                fail("%s has a line beginning with '#' that has the shape of a "
                     "data row: a group code cannot start with '#', because "
                     "the line reads as a comment", csv_path);
                goto cleanup;
            }
            continue;
        }
        seen++;
        if (los_parse_training(line, &c, &los) != 0) {
            fail("%s row %lld: %s", csv_path, seen, los_parse_error());
            goto cleanup;
        }
        if (pool) {
            (void)strcpy(c.group, "*");        /* one byte into GROUP_MAX */
        } else if (only && strcmp(c.group, only) != 0) {
            continue;
        }
        g = hash_get(index, c.group);
        if (!g) {
            g = xmalloc(process_group_bytes(nvars));
            g->next = NULL;
            g->beta = g->storage + need;     /* nvars+1 doubles, after the fitter */
            (void)diag_init(&g->d, nvars, g->beta + nvars + 1);
            /* strcpy, not memcpy: c is automatic; copy_group stops at NUL. */
            strcpy(g->group, c.group);       /* both are GROUP_MAX, c is bounded */
            if (fitter_init(&g->r, nvars, g->storage) != 0) {
                free(g);
                fail("%s has %d terms, more than the fitter's %d",
                     csv_path, nvars, REGRESS_MAX_VARS);
                goto cleanup;
            }
            g->sigma = -1.0;
            g->ny    = 0;
            g->ymean = 0.0;
            g->ym2   = 0.0;
            free(hash_put(index, c.group, g));
            if (tail) tail->next = g; else head = g;
            tail = g;
            groups++;
        }
        {   /* Welford over the response: two doubles and a count. */
            double dy = los - g->ymean;
            g->ny++;
            g->ymean += dy / (double)g->ny;
            g->ym2   += dy * (los - g->ymean);
        }
        if (fitter_add(&g->r, c.x, los) != 0) {
            fail("%s row %lld holds a value that is not finite", csv_path, seen);
            goto cleanup;
        }
        rows++;
        progress_row(seen, "fitting");
    }
    if (n < 0) {
        fail("%s %s", csv_path, csv_line_error(n, sizeof line));
        goto cleanup;
    }
    if (groups == 0) {
        if (only) { fail("%s has no rows in group %s", csv_path, only); goto cleanup; }
        fail("%s has no data rows", csv_path);
        goto cleanup;
    }
    /* A line through one point is not a fit, and this is what omitting the group
     * column looks like. */
    if (rows >= 3 && groups == rows) {
        fail("%s puts every one of its %lld rows in a different group, so there "
             "is nothing to fit. The first column is the group; if your data "
             "has no groups, add a column with the same value on every row",
             csv_path, rows);
        goto cleanup;
    }

    if (los_response_name()[0] != '\0')
        (void)fprintf(out, "# response: %s\n", los_response_name());
    if (los_format_header(row, sizeof row) != 0) {
        fail("the header does not fit in %zu bytes", sizeof row);
        goto cleanup;
    }
    (void)fprintf(out, "%s\n", row);

    for (g = head; g; g = g->next) {
        struct regress_fit f;
        struct los_model   fitted;

        {   int srv = fitter_solve(&g->r, fit_beta, fit_scratch, &f);
            if (srv != 0) { fail("%s", solve_failure(srv, g->group)); goto cleanup; }
        }
        /* Keep this group's line: the next group overwrites fit_beta, and the
         * residual pass needs it after all groups are solved. */
        {   int b;
            for (b = 0; b <= nvars; b++) g->beta[b] = fit_beta[b];
        }
        model_from_beta(&fitted, nvars);

        if (los_format_model(g->group, &fitted, row, sizeof row) != 0) {
            fail("group '%s' does not fit in %zu bytes", g->group, sizeof row);
            goto cleanup;
        }
        (void)fprintf(out, "%s\n", row);
        {   char note[LINEARR_MAX_OUTPUT];
            if (format_pinned(g->group, &f, nvars, note, sizeof note))
                (void)fprintf(out, "%s\n", note);
        }

        g->sigma = f.sigma;
        if (stats) {
            if (first_stat) {
                (void)fprintf(stats, "group,rows,df,r2,resid_sd,cond,pinned\n");
                first_stat = 0;
            }
            (void)fprintf(stats, "%s,%lld,%lld,", g->group, g->ny, f.df);
            if (f.r2 >= 0.0) (void)fprintf(stats, "%.6f,", f.r2);
            else             (void)fprintf(stats, ",");
            if (f.sigma >= 0.0)
                (void)fprintf(stats, "%s%.12g,", f.sigma_is_bound ? "<" : "", f.sigma);
            else (void)fprintf(stats, ",");
            (void)fprintf(stats, "%.6g,%d\n", f.condition, f.pinned);
        }
        if (sum) {
            sum->pinned += f.pinned;
            if (sum->groups == 0 || f.df < sum->min_df) sum->min_df = f.df;
            if (f.condition > sum->max_condition) sum->max_condition = f.condition;
            /* Lowest R2, skipping undefined ones; --stats names the group. */
            if (f.r2 >= 0.0 && (sum->min_r2 < 0.0 || f.r2 < sum->min_r2))
                sum->min_r2 = f.r2;
            else if (f.r2 == REGRESS_R2_FLAT_Y) sum->groups_flat_y++;
            else if (f.r2 == REGRESS_R2_LOST)   sum->groups_r2_lost++;
            if (f.sigma > sum->max_sigma) {
                sum->max_sigma = f.sigma;
                sum->sigma_is_bound = f.sigma_is_bound;
            }
            sum->groups++;
        }
    }
    if (sum) sum->rows = rows;

    /* Second pass, re-read: holding the rows would tie the footprint to it. */
    if (resid) {
        FILE *again;
        char  path[RESOLVE_PATH_MAX];
        long long resid_rows = 0;
        int   k;

        if (resolve_file(csv_path, path, sizeof path) != 0 ||
            (again = fopen(path, "r")) == NULL) {
            fail("cannot re-read %s for the residuals", csv_path);
            goto cleanup;
        }
        /* Probes centred on each group's own fit, judged against its spread. */
        for (g = head; g; g = g->next) {
            double ctr[REGRESS_MAX_VARS];
            fitter_centers(&g->r, nvars, ctr);
            diag_center(&g->d, ctr, (g->ny > 0) ? g->ymean : 0.0);
        }
        for (g = head; g; g = g->next)
            diag_scale(&g->d, g->sigma,
                       (g->ny > 1) ? sqrt(g->ym2 / (double)(g->ny - 1)) : -1.0);
        fprintf(resid, "group,observed,predicted,residual\n");
        progress_begin();                 /* the second pass is its own run */
        while ((n = csv_next(again, line, sizeof line)) == 2)
            ;                                    /* skip to past the header */
        while ((n = csv_next(again, line, sizeof line)) > 0) {
            double los, yhat;
            if (n == 2) continue;
            if (los_parse_training(line, &c, &los) != 0) continue;
            if (pool) (void)strcpy(c.group, "*");   /* as the first pass did */
            g = hash_get(index, c.group);
            if (!g) continue;
            yhat = g->beta[0];
            for (k = 0; k < nvars; k++) yhat += g->beta[k + 1] * c.x[k];
            fprintf(resid, "%s,%.12g,%.12g,%.12g\n", c.group, los, yhat, los - yhat);
            diag_add(&g->d, c.x, los - yhat, yhat);
            progress_row(++resid_rows, "residuals");
        }
        fclose(again);

        if (sum) {
            /* One bound for the whole file: the summary reports the largest
             * probe over every group, and the probe count sets it. */
            double bound = diag_bound(nvars, groups);
            for (g = head; g; g = g->next) {
                struct diag_result dr;
                diag_result(&g->d, bound, &dr);
                if (dr.curved_term >= 0 && fabs(dr.curved_t) > fabs(sum->curved_t)) {
                    sum->curved_term = dr.curved_term;
                    sum->curved_t    = dr.curved_t;
                    sum->curved_pow  = dr.curved_pow;
                    /* snprintf, not memcpy of the whole field: g->group is heap
                     * filled by strcpy, so bytes past its NUL are junk. */
                    (void)snprintf(sum->worst_group, sizeof sum->worst_group,
                                   "%s", g->group);
                }
                if (fabs(dr.fitted_t) > fabs(sum->fitted_t)) sum->fitted_t = dr.fitted_t;
                if (fabs(dr.spread_t) > fabs(sum->spread_t)) sum->spread_t = dr.spread_t;
            }
        }
    }
    rc = 0;
cleanup:
    if (fp && fp != stdin) fclose(fp);
    if (index) hash_delete(index);      /* the models are freed by the list */
    group_fits_free(head);
    return rc;
}

void process_free(void) {
    los_free();
    tables_loaded = 0;
}
