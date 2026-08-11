/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* process.c: see process.h. The two halves of the model: fit the line from a
 * training file, then use the line to score a case.
 *
 * The prediction is the intercept plus one coefficient-times-term product per
 * column, rounded half away from zero to four digits; the trim point is that
 * plus the group's trim addition, rounded to one. The rounding is part of the
 * answer, not presentation: the published figure is the rounded number. Both
 * scales are configurable, and so is the number of terms; it comes from the
 * file's header, not from this file. */
/* Before any header, including this module's own: process.h pulls in <stdio.h>,
 * and glibc resolves <features.h> on the first standard header it sees. Defined
 * after that, as it was, this macro does nothing at all -- CLOCK_MONOTONIC and
 * _POSIX_TIMERS stay undefined, the guard in progress_now() is false, and the
 * timer silently falls back to time(NULL), the wall clock whose NTP and DST
 * steps the comment there says were fixed. `nm -u linearr` showed no
 * clock_gettime at all. resolve.c has always had this the right way round. */
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

#define DEFAULT_PREDICT_SCALE 4
#define DEFAULT_TRIM_SCALE 1

/* The fitter's two matrices, in static storage rather than on the stack.
 * As automatics they were 606 KB and 531 KB, live at the same time, so the
 * default build needed 1.18 MB of contiguous stack and died with SIGSEGV and no
 * diagnostic under `ulimit -s 1024`, on a program whose README sells embedded
 * and locked-down clinical targets, where a 128 KB thread stack is normal. The
 * same bytes in BSS are allocated once, cost the same, and cannot blow a stack.
 * The consequence is that a fit is not reentrant, which is true of this
 * single-threaded CLI anyway and is now written down instead of implied.
 *
 * That fixed the matrices and not the buffers. A fit still needs 188 KB of
 * stack, bisected, and still dies with SIGSEGV and no diagnostic below it, so
 * the 128 KB thread stack named above does NOT fit a default-ceiling fit --
 * scoring fits in 113 KB, fitting does not. The remaining driver is
 * MAX_OUTPUT, two of which live in process_train_residuals' frame beside a
 * line buffer. Shrinking the ceiling shrinks all of them together: a 32-term
 * build fits in 54 KB. doc/INTERNALS.md carries the table. */
/* Big enough for whichever solver is chosen. Both are O(terms^2) and neither
 * depends on the data, so one buffer serves both.
 *
 * "Big enough" was a comment and not a check, and it stopped being true when
 * the QR gained column pivoting: qr_storage() grew four vectors of p+1, for
 * each column's range and its running 2-norm, and this figure did not. At the
 * default ceiling qr_init() then memset 8 KB past the end of this array. The
 * release build printed a plausible table and exited 0; only ASan saw it, and
 * only at 255 terms or more, which nothing tested.
 *
 * So the two solvers' own formulas are written out here and the array takes
 * the larger, with the asserts below tying it to them. A solver that grows its
 * storage again now fails to compile instead of writing past this. */
#define REGRESS_STORE_DOUBLES ((size_t)REGRESS_MAX_VARS * (size_t)REGRESS_MAX_VARS \
                               + 2u * (size_t)REGRESS_MAX_VARS)
#define QR_STORE_DOUBLES (((size_t)REGRESS_MAX_VARS + 1u) * ((size_t)REGRESS_MAX_VARS + 2u) \
                          + 4u * ((size_t)REGRESS_MAX_VARS + 1u))
#define FIT_STORE_DOUBLES (QR_STORE_DOUBLES > REGRESS_STORE_DOUBLES \
                           ? QR_STORE_DOUBLES : REGRESS_STORE_DOUBLES)
/* If either solver's storage grows again, this stops compiling, which is the
 * diagnostic whose absence cost 8 KB of silent overwrite. */
typedef char fit_store_holds_regress[
    (FIT_STORE_DOUBLES >= REGRESS_STORE_DOUBLES) ? 1 : -1];
typedef char fit_store_holds_qr[
    (FIT_STORE_DOUBLES >= QR_STORE_DOUBLES) ? 1 : -1];

static double fit_store[FIT_STORE_DOUBLES];
/* Large enough for either solver: regress_solve's elimination workspace, and
 * qr_solve's re-triangularisation of the kept columns when a design turns out
 * to be rank deficient. */
static double fit_scratch[(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2)];
static double fit_beta[REGRESS_MAX_TERMS];

static int use_qr;
void process_use_qr(int on) { use_qr = on; }
const char *process_solver(void) { return use_qr ? "QR" : "normal equations"; }

/* One accumulator, one of two shapes. The caller owns the storage in both
 * cases, so the choice costs nothing but a branch. */
/* A union, not a struct: as a struct every group carried BOTH solvers, and
 * struct qr is 4 KB because it holds its column ranges inline, so a two-term
 * model paid 4232 bytes a group for the one it was not using.
 *
 * And it remembers which one it is. use_qr was read afresh in init, add and
 * solve, so nothing bound the three: a caller that changed the flag between
 * them would have a struct regress block reinterpreted as a struct qr, with
 * q->r read as a garbage pointer. Unreachable from today's main, which parses
 * options before it fits, and one field to remove for good. */
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
    f->is_qr = use_qr;                  /* decided once, here, and remembered */
    return f->is_qr ? qr_init(&f->u.q, nvars, storage)
                    : regress_init(&f->u.r, nvars, storage);
}

/* Where each column sits, for the residual probes to take their powers about.
 * The normal equations already hold the means; QR does not centre, so the
 * midpoint of each column's range stands in, which is what its rank test
 * already keeps. Either is enormously better than the first row's value. */
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
/* Why a solve failed, in the caller's words. -2 is the one cause a user can act
 * on: a column so large that its cross-products overflowed while they were
 * being accumulated, which the normal equations cannot survive and the QR does
 * not have. It used to be reported as "the result is not a finite line", which
 * is true and useless, and which blamed the whole fit for one column. */
static const char *solve_failure(int rv, const char *group) {
    static char msg[320];
    if (rv == -2)
        (void)snprintf(msg, sizeof msg,
                       "group '%s': a term's values are so large "
                       "that squaring them overflowed (near 1e160 or beyond). "
                       "Rescale that column, or use --qr, which does not square "
                       "them. Run with -d to see which term", group);
    else if (rv == -3)
        /* The advice differs from -2's, and saying why matters: --qr keeps
         * the COLUMNS from being squared, but both solvers square the
         * response for its residual, so the remedy that saves an overflowing
         * term does nothing for an overflowing response. */
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

/* Set the reason and fail in one statement, so no path can return -1 while
 * leaving the previous run's explanation behind. */
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

/* Progress on a long run.
 *
 * A fit over a billion rows takes minutes and a fit over a trillion takes
 * days, and until it finishes the program says nothing at all. A run that is
 * working and a run that is wedged look identical, which is the wrong thing
 * for a program whose whole argument is that the row count is not a limit.
 *
 * The first line appears only after a minute, so nothing that finishes quickly
 * ever prints one, and then once a minute after that. The clock is read once
 * per million rows rather than once per row, since reading it is a syscall on
 * some systems and the row path is where the time goes.
 *
 * A MONOTONIC clock where there is one. time() is a wall clock: an NTP or DST
 * step backwards makes the elapsed figure negative, and since prog_last only
 * advances when a line prints, one backward step used to stop the reporting
 * for the rest of the run.
 *
 * To stderr, which is where this program's commentary already goes, so stdout
 * stays a coefficient file. */
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
/* Not on Windows: MinGW's headers declare CLOCK_MONOTONIC and _POSIX_TIMERS
 * and its default libraries have no clock_gettime, so the guard those two
 * suggest compiles and then fails to link. Found by cross-compiling, which is
 * the only way to find it from here. */
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

/* The two roundings, set by --scale and --trim-scale. A scale outside 0 to 9 is
 * rejected here rather than falling back to the default, so what the caller
 * asked for and what the program does cannot differ silently. */
static int predict_scale = DEFAULT_PREDICT_SCALE;
static int trim_scale    = DEFAULT_TRIM_SCALE;

/* Name the column being predicted, instead of taking column 2. NULL restores
 * the positional default. */
void process_use_response(const char *name) { response_override = name; }

/* Whether the response was named or taken from the column order. The summary
 * line says so, and offers the remedy only when one is needed: a reader who
 * passed -y does not need to be told it exists. */
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

    /* Named, or nothing. There used to be a search: a properties file, then a
     * table shipped beside the binary, so a bare run scored against a demo
     * model the reader had never seen, and the same command in two directories
     * could answer with two different models without saying so. A model is the
     * whole of what the answer means; it is not something to find by
     * convention. */
    if (!coef_override)
        return fail("no coefficient table. Name one with -c FILE, or fit one "
                    "first: linearr -t TRAIN.CSV > model.csv");
    want = coef_override;
    if (resolve_file(want, path, sizeof path) != 0)
        return fail("cannot find the coefficient table %s", path);
    if (los_load(path) != 0) return fail("%s", los_error());
    (void)snprintf(coef_path, sizeof coef_path, "%s", path);

    /* The trim table is optional in two ways, and they are different: named
     * with --trim and unreadable is a failure the user asked for and must hear
     * about; not named at all means there is none, and the trim point is the
     * prediction. */
    trim = trim_override_set ? trim_override : NULL;
    if (!trim || trim[0] == '\0') {
        debug("process: no --trim; the trim point is the prediction");
    } else if (resolve_file(trim, path, sizeof path) != 0) {
        /* Two different failures, and they were reported as one. When
         * resolve_file is what failed, los_load_trims never ran and los_error()
         * still held whatever was there before, which on a first load is
         * nothing: the message was a leading space and a parenthesis. */
        los_free();
        return fail("cannot find the trim table %s (leave --trim off if there "
                    "is none)", path);
    } else if (los_load_trims(path) != 0) {
        /* Read BEFORE los_free, which clears it. */
        char why[MAX_OUTPUT];
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

/* The half both entry points share: a filled-in case becomes two rounded
 * numbers and a line of text. */
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

    /* The trim point is built on the ROUNDED prediction, not the raw one: the
     * published figure is what the next step is entitled to use. */
    prediction = los_round(los_predict(m, c), pscale);
    trim       = los_round(los_trim_point(m, prediction), tscale);

    /* Without a trim table the trim point IS the prediction, so printing it
     * again invents a second quantity. A reader who passed --no-trim and still
     * saw trim= could not tell what the flag had done. */
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
        /* Hexadecimal is refused, as los.c refuses it in a CSV field, for the
         * same rule: the two forms must not disagree, and a=0x10 scored
         * prediction=32 while the file form of the same case was refused.
         * The scan skips what strtod skips, so a=<tab>0x10 is caught too. */
        {   const char *t = eq + 1;
            while (*t == ' ' || *t == '\t' || *t == '\n'
                || *t == '\v' || *t == '\f' || *t == '\r') t++;
            if (*t == '-' || *t == '+') t++;
            if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
                return fail("'%s' is hexadecimal, which is not a number here: "
                            "write the value in decimal", eq + 1);
        }
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
        /* los.c has already composed a sentence naming the column and the
         * reason. Both training paths print it; this one asked for a generic
         * one instead, which is the message the rewrite existed to remove. */
        return fail("%s", los_parse_error());

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

    if (strcmp(csv_path, "-") == 0) {
        /* A pipe. This is what a cloud invocation looks like: object storage
         * streamed straight in, nothing landing on disk. The fit needs exactly
         * one pass, so a pipe is enough for it; --residuals needs a second and
         * is refused separately. */
        *fpp = stdin;
    } else {
        /* the same two places as the tables: your file first, then the ones
         * that shipped beside the program, so the examples work from anywhere */
        if (resolve_file(csv_path, path, sizeof path) != 0)
            return fail("cannot open the training file %s", path);
        *fpp = fopen(path, "r");
        if (!*fpp) return fail("cannot open the training file '%s'", path);
    }

    while ((n = csv_next(*fpp, line, sizeof line)) == 2)
        ;                                   /* leading comments precede a header */
    if (n != 1)
        return fail("%s has no header line", csv_path);
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3)
    {
        if (n == 1 && (strchr(field[0], ';') || strchr(field[0], '\t')))
            return fail("%s has no commas in its header, but does have %s. It "
                        "looks %s-separated; this program reads commas only",
                        csv_path, strchr(field[0], ';') ? "semicolons" : "tabs",
                        strchr(field[0], ';') ? "semicolon" : "tab");
        return fail("%s needs a header of group, the observed value, and at "
                    "least one term", csv_path);
    }
    /* Which column is being predicted, and therefore which are terms. Column 2
     * unless --response names another: nothing in the data can say which is
     * which, so a file written in a different order fits perfectly well and
     * answers a question nobody asked. */
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
    struct fitter      r;
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
    long long rows = 0, seen = 0;

    if (open_training(csv_path, &fp, &nvars) != 0) goto cleanup;

    progress_begin();
    if (fitter_init(&r, nvars, fit_store) != 0) {
        fail("%s has %d terms, more than the fitter's %d", csv_path, nvars,
             REGRESS_MAX_VARS);
        goto cleanup;
    }
    /* One row at a time: read it, add it to the cross-products, forget it. The
     * file may be any size; the fitter's footprint is the same either way. */
    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        double los;
        if (n == 2) {
            /* The check the coefficient loader has always run, and this reader
             * did not: a '#' line that splits into a data row's fields is a
             * training row whose group starts with '#', and skipping it fitted
             * the file MINUS that group, exit 0, nothing on screen. */
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
    if (n < 0) { fail("%s has a line longer than %d bytes", csv_path, CSV_LINE_MAX); goto cleanup; }

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
        info->sigma_is_bound = f.sigma_is_bound;
        info->condition = f.condition;
    }
    rc = 0;
cleanup:
    if (fp && fp != stdin) fclose(fp);
    return rc;
}

/* One accumulator per group, in first-seen order so the emitted table is
 * reproducible. The hash gives O(1) lookup per row; the list gives an order. */
struct group_fit {
    struct group_fit *next;
    char   group[GROUP_MAX];
    struct fitter r;
    struct diag   d;                /* its own, so groups are not pooled       */
    double *beta;
    /* This group's own residual SD, and a Welford pair for its own response.
     * The residual checks compare the two to decide whether there is anything
     * left to explain, and both used to be taken from the WHOLE file: the worst
     * residual SD of any group against the spread of every row together. A
     * group that fits exactly, sitting beside one that does not, was therefore
     * judged by the other group's error, and the guard that exists to stop the
     * checks correlating rounding error never fired for it. */
    double sigma;
    long   ny;
    double ymean, ym2;
    double storage[1];              /* fitter + (nvars+1) beta + diag          */
};

/* What ONE group costs, exactly. The allocation below calls this rather than
 * repeating the arithmetic, so the number a user is told and the number the
 * program asks for cannot differ.
 *
 * They did. scale.sh said "groups x (terms + 1) doubles", the README said
 * "about 2 KB per group", and process.h said the bound was groups x terms^2.
 * All three were describing the fitter alone, or a guess at it, and none
 * counted the beta vector or the residual-check block. */
size_t process_group_bytes(int nvars) {
    if (nvars < 1) return 0;
    return sizeof(struct group_fit)
         + (fitter_storage(nvars) + (size_t)nvars + diag_storage(nvars))
           * sizeof(double);
}

/* And what one group costs on the SCORING side, which is a different number
 * and was being quoted as if it were this one. A loaded model is a fixed
 * struct: the coefficient array is dimensioned at the build ceiling, not at
 * the model's own term count, so a two-term model pays for LOS_MAX_VARS. That
 * is a real cost and it is stated rather than averaged away. */
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
    char   row[MAX_OUTPUT];
    FILE  *fp = NULL;
    size_t need;
    int    rc = -1, n, nvars = 0;
    long long rows = 0, seen = 0, groups = 0;
    int    first_stat = 1;
    /* -g '*' means pool every row into one line named '*', which is what it
     * means in process_train. Here it was read as a literal group code, so a
     * pooled fit that worked without --residuals became "no rows in group *"
     * with it: adding a flag that asks for MORE output made a working fit stop
     * working, and it was the pooled fit -- the one whose residuals a reader
     * most wants, since pooling is what a wrong shape hides in. */
    const int pool = (only != NULL && strcmp(only, "*") == 0);

    /* Checked before anything is read or written: the refusal used to arrive
     * after the coefficient table had already gone to stdout. */
    if (resid && strcmp(csv_path, "-") == 0)
        return fail("--residuals needs to read the training data a second time, "
                    "to subtract each row from its own prediction, and a pipe "
                    "cannot be rewound. Give it a file instead of -");

    if (sum) {
        sum->groups = 0; sum->rows = 0; sum->min_df = 0;
        sum->max_condition = 1.0; sum->pinned = 0; sum->max_sigma = -1.0; sum->sigma_is_bound = 0;
        sum->min_r2 = -1.0;
        sum->curved_term = -1; sum->curved_t = 0.0; sum->curved_pow = 0;
        sum->fitted_t = 0.0; sum->spread_t = 0.0; sum->worst_group[0] = '\0';
    }

    if (open_training(csv_path, &fp, &nvars) != 0) goto cleanup;

    progress_begin();
    need  = fitter_storage(nvars);
    index = hash_create(1024);

    /* One pass. A row is added to its group's accumulator and forgotten, so the
     * file may be any size; only the number of GROUPS costs memory. */
    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        double los;
        if (n == 2) {
            /* As in process_train: a data-shaped '#' line is a swallowed row,
             * not a comment. The residual second pass keeps its plain skip,
             * because this refusal has already run before it can start. */
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
            /* one accumulator, whatever the row said its group was */
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
            /* strcpy, not memcpy of the whole field: c is an automatic and
             * copy_group only writes up to the NUL, so the bytes past it are
             * indeterminate on the first row and used to propagate into
             * sum->worst_group. Neither sanitizer sees that. */
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
        {   /* Welford over this group's response. It is what the group's
             * residual checks are judged against, and it is two doubles and a
             * count, so it does not make the footprint a function of the data.
             * There used to be one of these for the WHOLE FILE instead, which
             * meant a group that fits exactly was judged by the spread of every
             * other group's rows. */
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
        fail("%s has a line that is over-long or holds a NUL byte", csv_path);
        goto cleanup;
    }
    if (groups == 0) {
        if (only) { fail("%s has no rows in group %s", csv_path, only); goto cleanup; }
        fail("%s has no data rows", csv_path);
        goto cleanup;
    }
    /* Every row in a group of its own cannot be fitted: a line through one
     * point is not a fit. It is also what omitting the group column looks
     * like, which is the commonest way to write this file wrongly, and it used
     * to produce a table of one-row models and exit 0. */
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

        g->sigma = f.sigma;
        if (stats) {
            if (first_stat) {
                (void)fprintf(stats, "group,rows,df,r2,resid_sd,cond,pinned\n");
                first_stat = 0;
            }
            (void)fprintf(stats, "%s,%lld,%lld,", g->group, (long long)g->ny, f.df);
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
            /* The lowest R2 over the groups, skipping the ones where it is
             * not defined. With 580 groups it is as useful as the worst
             * residual SD, and --stats says which group it came from. */
            if (f.r2 >= 0.0 && (sum->min_r2 < 0.0 || f.r2 < sum->min_r2))
                sum->min_r2 = f.r2;
            if (f.sigma > sum->max_sigma) {
                sum->max_sigma = f.sigma;
                sum->sigma_is_bound = f.sigma_is_bound;
            }
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
        long long resid_rows = 0;
        int   k;

        if (resolve_file(csv_path, path, sizeof path) != 0 ||
            (again = fopen(path, "r")) == NULL) {
            fail("cannot re-read %s for the residuals", csv_path);
            goto cleanup;
        }
        /* Each group's probes centred on its own fit, and each judged against
         * its own spread. */
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

        /* The residuals are the only place a wrong SHAPE shows: every number in
         * the summary is an average over them, and an average cannot see a
         * pattern. */
        if (sum) {
            /* One bound for the whole file, because the summary reports the
             * largest probe over every group: the number of probes read is
             * what decides how large a maximum is unsurprising. */
            double bound = diag_bound(nvars, groups);
            for (g = head; g; g = g->next) {
                struct diag_result dr;
                diag_result(&g->d, bound, &dr);
                if (dr.curved_term >= 0 && fabs(dr.curved_t) > fabs(sum->curved_t)) {
                    sum->curved_term = dr.curved_term;
                    sum->curved_t    = dr.curved_t;
                    sum->curved_pow  = dr.curved_pow;
                    memcpy(sum->worst_group, g->group, sizeof sum->worst_group);
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
