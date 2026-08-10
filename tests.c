/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* tests.c: in-place unit tests, run by `make ut` (which builds every source
 * with -DUNIT_TEST; this file is empty otherwise, and main.c's main() is then
 * compiled out). Add a CHECK when you add a feature. Idempotent, and run from
 * the project root: conf/ and example/ are read by relative path. */
#ifdef UNIT_TEST

#include "common.h"
#include "utils.h"
#include "hash.h"
#include "params.h"
#include "csv.h"
#include "regress.h"
#include "qr.h"
#include "diag.h"
#include "los.h"
#include "process.h"
#include "resolve.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int pass, fail;
#define CHECK(cond, msg) do { \
    if (cond) pass++; \
    else { fail++; printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

/* A NULL where a string was expected is a FAIL, not a crash that takes the
 * whole run down and tells you nothing about the other tests. Use this for
 * EVERY comparison against a function documented as possibly returning NULL
 * (los_var_name, process_term_name, params_get, ...); a bare strcmp on one of
 * those is how this suite once turned a wrong return value into a SEGV. */
static int streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

/* Pull the numbers out of a "GROUP,b0,b1,..." row. Coefficients are written at
 * full precision now, so comparing the TEXT of a fit against the text of the
 * table it was generated from is a comparison of rounding, not of arithmetic.
 * Compare the values. */
static int coef_row(const char *row, double *v, int max) {
    const char *p = strchr(row, ',');
    const char *eol = strchr(row, '\n');       /* a '# pinned' note may follow */
    int n = 0;
    while (p && n < max && (!eol || p < eol)) {
        char *end;
        v[n++] = strtod(p + 1, &end);
        p = strchr(end, ',');
    }
    return n;
}

#define COEF "conf/coefficients.csv"
#define TRIM "conf/trim_additions.csv"

/* The pair, as the scorer loads them. They are two calls because the trim
 * table is optional; see test_los_trims. */
static int los_load_both(void) {
    if (los_load(COEF) != 0) return -1;
    return los_load_trims(TRIM);
}

/* Build "GROUP,x1,...,xp" for the loaded schema, with the listed terms set to
 * 1. Requires a schema: call after los_load. */
static void make_case(char *buf, size_t bufsz, const char *group,
                      const int *on, int non) {
    size_t used = (size_t)snprintf(buf, bufsz, "%s", group);
    int i, j;
    for (i = 0; i < los_nvars(); i++) {
        int one = 0;
        for (j = 0; j < non; j++) if (on[j] == i) one = 1;
        used += (size_t)snprintf(buf + used, bufsz - used, ",%d", one);
    }
}

static void test_utils(void) {
    char a[16];
    strcpy(a, "  hi  ");
    rtrim(a, ' '); CHECK(strcmp(a, "  hi") == 0, "rtrim trailing");
    ltrim(a, ' '); CHECK(strcmp(a, "hi") == 0, "ltrim leading");
    strcpy(a, "xxab"); ltrim(a, 'x'); CHECK(strcmp(a, "ab") == 0, "ltrim run");
}

static void test_hash(void) {
    struct hash *h = hash_create(16);
    hash_put(h, "k", "one");
    CHECK(strcmp((char *)hash_get(h, "k"), "one") == 0, "hash get");
    /* hash_put hands back what it displaced, so the caller can free it. */
    CHECK(strcmp((char *)hash_put(h, "k", "two"), "one") == 0, "hash replace returns the old");
    CHECK(strcmp((char *)hash_get(h, "k"), "two") == 0, "hash replace stored the new");
    CHECK(hash_get(h, "absent") == NULL, "hash miss");
    hash_delete(h);
}

static void test_params(void) {
    CHECK(params_load("system.properties") == 0, "params load");
    CHECK(streq(params_get("coef.file"), COEF),
          "params dotted key");
    CHECK(params_get("nope") == NULL, "params miss");
    CHECK(params_load("no-such-file") == -1, "params open error");
    params_free();
}

static void test_csv(void) {
    char  line[64];
    char *f[8];
    FILE *fp;

    strcpy(line, "a,b,c");
    CHECK(csv_split(line, f, 8) == 3 && strcmp(f[1], "b") == 0, "csv split");

    strcpy(line, " a , b ");                     /* spaces around a field go */
    CHECK(csv_split(line, f, 8) == 2 && strcmp(f[0], "a") == 0 && strcmp(f[1], "b") == 0,
          "csv trims fields");

    strcpy(line, "a,,c");                        /* an empty field is "", not NULL */
    CHECK(csv_split(line, f, 8) == 3 && f[1][0] == '\0', "csv empty field");

    strcpy(line, "one");
    CHECK(csv_split(line, f, 8) == 1, "csv single field");

    strcpy(line, "a,b,c");
    CHECK(csv_split(line, f, 2) == -1, "csv refuses more fields than it was given room for");

    /* csv_next skips blank and '#' lines and strips the line ending. */
    fp = fopen("example/train.csv", "r");
    CHECK(fp != NULL, "csv open example");
    if (fp) {
        char big[CSV_LINE_MAX];
        /* Comment lines are RETURNED with a 2 rather than swallowed, so a
         * caller can tell a comment from a data row it has misread. */
        int r;
        CHECK((r = csv_next(fp, big, sizeof big)) == 2, "csv_next returns comments to the caller");
        CHECK(big[0] == '#', "csv_next: and hands back the comment text");
        while ((r = csv_next(fp, big, sizeof big)) == 2)
            ;
        CHECK(r == 1, "csv_next reads the data line after them");
        CHECK(strncmp(big, "group,los,", 10) == 0, "csv_next reached the header");
        CHECK(strchr(big, '\n') == NULL, "csv_next stripped the newline");
        /* A line that does not fit is refused, never silently split in two. */
        CHECK(csv_next(fp, big, 8) == -1, "csv_next refuses an over-long line");
        (void)fclose(fp);
    }
}

/* Storage for the fitter, which allocates nothing itself. Static, because the
 * matrices are ~1 MB at the default ceiling and this is exactly where the
 * program used to blow a small stack. */
static double t_store[REGRESS_MAX_VARS * REGRESS_MAX_VARS + 2 * REGRESS_MAX_VARS];
static double t_scratch[REGRESS_MAX_VARS * (REGRESS_MAX_VARS + 1)];
static double t_beta[REGRESS_MAX_TERMS];
static double t_store2[(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2)];
static double t_beta2[REGRESS_MAX_TERMS];

static void test_regress(void) {
    struct regress r;
    struct regress_fit f;
    double x[2];

    /* An exact line through exact points comes back exactly: y = 2 + 3x1 - x2. */
    regress_init(&r, 2, t_store);
    x[0] = 0; x[1] = 0; regress_add(&r, x, 2.0);
    x[0] = 1; x[1] = 0; regress_add(&r, x, 5.0);
    x[0] = 0; x[1] = 1; regress_add(&r, x, 1.0);
    x[0] = 2; x[1] = 3; regress_add(&r, x, 5.0);
    CHECK(regress_solve(&r, t_beta, t_scratch, &f) == 0, "regress: solves");
    CHECK(f.pinned == 0, "regress: full rank, nothing pinned");
    CHECK(NEAR(t_beta[0], 2.0) && NEAR(t_beta[1], 3.0) && NEAR(t_beta[2], -1.0),
          "regress: recovers the coefficients");
    CHECK(NEAR(f.r2, 1.0), "regress: R2 is 1 on an exact fit");
    CHECK(f.df == 1, "regress: df");

    /* Noise around that line: close but not exact, and R2 must fall below 1. */
    regress_init(&r, 1, t_store);
    x[0] = 0; regress_add(&r, x, 1.0);
    x[0] = 1; regress_add(&r, x, 3.1);
    x[0] = 2; regress_add(&r, x, 4.9);
    x[0] = 3; regress_add(&r, x, 7.2);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(fabs(t_beta[1] - 2.0) < 0.1, "regress: slope through noisy points");
    CHECK(f.r2 < 1.0 && f.r2 > 0.99, "regress: R2 below 1 once points are not collinear");

    /* The residual SD is the number a prediction consumer needs and R2 cannot
     * give: how far a prediction typically lands from the truth, in the
     * response's own units. Built from noise of SD 2, it must come back near 2
     * and R2 near 1 at the same time, which is the point: a high R2 and a
     * large error are not contradictory. */
    {
        int i;
        regress_init(&r, 1, t_store);
        for (i = 0; i < 400; i++) {
            double noise = ((i * 37) % 11 - 5) * 0.606;   /* SD ~= 2, no RNG */
            x[0] = (double)i;
            regress_add(&r, x, 5.0 + 3.0 * x[0] + noise);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.sigma > 1.5 && f.sigma < 2.5, "residual SD: recovers the noise level");
        CHECK(f.r2 > 0.999, "residual SD: and R2 is near 1 at the same time");
        CHECK(f.rss > 0.0, "residual SD: the residual sum of squares is reported");
        CHECK(fabs(f.sigma - sqrt(f.rss / (double)f.df)) < 1e-12,
              "residual SD: is sqrt(rss/df), divided by the freedom not by n");
    }
    {   /* with no freedom left there is no spread to estimate */
        regress_init(&r, 1, t_store);
        x[0] = 0; regress_add(&r, x, 1.0);
        x[0] = 1; regress_add(&r, x, 2.0);
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.df == 0, "residual SD: df is zero here");
        CHECK(f.sigma < 0.0, "residual SD: and it is not reported");
    }

    /* THE UNITS TEST. y = 1 + 2e-6*big + 5*flag. The two columns differ by six
     * orders of magnitude, so their X'X diagonals differ by twelve, and the
     * old absolute rank tolerance deleted the indicator for being small,
     * reporting a constant model with a straight face. Whether a term exists
     * must not depend on whether you write dollars or thousands. */
    {
        int i;
        regress_init(&r, 2, t_store);
        for (i = 0; i < 40; i++) {
            x[0] = 1.0e5 + i * 2.0e4;
            x[1] = (double)(i % 2);
            regress_add(&r, x, 1.0 + 2.0e-6 * x[0] + 5.0 * x[1]);
        }
        CHECK(regress_solve(&r, t_beta, t_scratch, &f) == 0, "units: solves");
        CHECK(f.pinned == 0, "units: neither column is pinned for its scale");
        CHECK(fabs(t_beta[1] - 2.0e-6) < 1e-12, "units: the large column's slope");
        CHECK(fabs(t_beta[2] - 5.0) < 1e-6, "units: the small column's slope");
        CHECK(f.r2 > 0.999999999, "units: R2");
    }

    /* THE OFFSET TEST. y carries a 1e8 offset. Uncentered sums of squares are
     * differences of huge nearly-equal numbers, so this used to report R2=1.0000
     * for a model with a true R2 of 0: the clamp at sse<0 manufactured the
     * perfect score. Centered accumulation makes the offset cancel first. */
    {
        int i;
        regress_init(&r, 1, t_store);
        for (i = 0; i < 60; i++) {
            x[0] = (double)i;
            regress_add(&r, x, 1.0e8 + 3.0 + 4.0 * x[0]);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(fabs(t_beta[0] - (1.0e8 + 3.0)) < 1e-4, "offset: intercept survives 1e8");
        CHECK(fabs(t_beta[1] - 4.0) < 1e-9, "offset: slope survives 1e8");
        CHECK(f.r2 > 0.999999, "offset: R2 is still 1 on exact data");
    }
    {   /* and an offset response with real noise must not report a perfect fit */
        int i;
        regress_init(&r, 1, t_store);
        for (i = 0; i < 60; i++) {
            x[0] = (double)i;
            regress_add(&r, x, 1.0e8 + 3.0 + 4.0 * x[0] + ((i % 7) - 3) * 2.0);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.r2 < 0.9999, "offset: a noisy fit does not come back as R2=1");
        CHECK(f.r2 > 0.9, "offset: but it is still a good fit");
    }

    /* A constant column is collinear with the intercept: pinned, and its effect
     * correctly absorbed rather than fought over. */
    regress_init(&r, 2, t_store);
    x[0] = 0; x[1] = 7; regress_add(&r, x, 1.0);
    x[0] = 1; x[1] = 7; regress_add(&r, x, 3.0);
    x[0] = 2; x[1] = 7; regress_add(&r, x, 5.0);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(f.pinned == 1, "regress: the constant column is pinned");
    CHECK(t_beta[2] == 0.0, "regress: a pinned coefficient is exactly 0");
    CHECK(NEAR(t_beta[1], 2.0), "regress: the identified slope is still right");
    CHECK(NEAR(t_beta[0], 1.0), "regress: and the intercept is never pinned");

    /* Two columns saying the same thing: one is pinned, the fit still fits. */
    regress_init(&r, 2, t_store);
    x[0] = 1; x[1] = 2; regress_add(&r, x, 4.0);
    x[0] = 2; x[1] = 4; regress_add(&r, x, 6.0);
    x[0] = 3; x[1] = 6; regress_add(&r, x, 8.0);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(f.pinned == 1, "regress: collinear column is pinned");
    CHECK(NEAR(f.r2, 1.0), "regress: the collinear fit still fits");

    /* An ill-conditioned but not singular design must be REPORTED, because R2
     * cannot see it: the fit is beautiful on its own sample and predicts noise. */
    {
        int i;
        regress_init(&r, 2, t_store);
        for (i = 0; i < 50; i++) {
            x[0] = 1.0 + i * 0.01;
            /* Nearly the same column, but NOT proportional to it: a strictly
             * proportional column is exactly rank-deficient and gets pinned,
             * which is a different verdict. This one is identifiable and
             * badly conditioned, which is the case R2 cannot see. */
            x[1] = x[0] + ((i % 2) ? 1.0e-6 : 0.0);
            regress_add(&r, x, 1.0 + 2.0 * x[0] + 3.0 * x[1]);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.condition > 1e6, "conditioning: a near-duplicate column is reported");
    }
    {   /* and a healthy design reports a small number, so the warning is useful */
        int i;
        regress_init(&r, 2, t_store);
        for (i = 0; i < 50; i++) {
            x[0] = (double)(i % 5); x[1] = (double)(i % 3);
            regress_add(&r, x, 1.0 + 2.0 * x[0] + 3.0 * x[1]);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.condition < 1e3, "conditioning: a healthy design reports a small number");
    }

    /* Non-finite input is refused at the door rather than poisoning every
     * coefficient and being published as a table of "nan". */
    regress_init(&r, 1, t_store);
    x[0] = 1.0;
    CHECK(regress_add(&r, x, 1.0) == 0, "regress: a finite observation is accepted");
    CHECK(regress_add(&r, x, 0.0 / 0.0) == -1, "regress: a NaN response is refused");
    x[0] = 1.0 / 0.0;
    CHECK(regress_add(&r, x, 1.0) == -1, "regress: an infinite term is refused");

    /* An empty sample is not a fit, and says so. */
    regress_init(&r, 2, t_store);
    CHECK(regress_solve(&r, t_beta, t_scratch, &f) == -1, "regress: refuses an empty sample");

    /* Fewer rows than terms is NOT refused: ordinary here, and pinning answers
     * it. One row identifies the intercept and nothing else. */
    regress_init(&r, 2, t_store);
    x[0] = 1; x[1] = 1; regress_add(&r, x, 4.0);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(f.pinned == 2, "regress: one row identifies no slope");
    CHECK(NEAR(t_beta[0], 4.0) && t_beta[1] == 0.0 && t_beta[2] == 0.0,
          "regress: that one term is the intercept, through the single point");

    CHECK(regress_init(&r, REGRESS_MAX_VARS + 1, t_store) == -1,
          "regress: refuses too many variables");
    CHECK(regress_init(&r, 2, NULL) == -1, "regress: refuses no storage");
}

/* The ceiling is not decoration: fit a model as wide as the build allows and
 * check every coefficient comes back. 32 terms was a toy bound; a real table is
 * hundreds of columns wide, and this is the test that says so. */
static void test_wide_fit(void) {
    struct regress r;
    struct regress_fit f;
    static double x[REGRESS_MAX_VARS];
    const int p = REGRESS_MAX_VARS;
    int i, j, ok = 1;

    regress_init(&r, p, t_store);

    /* y = 3 + sum(0.25*j * xj). One row per term isolates it; a handful of
     * combinations afterwards leave residual degrees of freedom behind. */
    for (i = 0; i < p; i++) x[i] = 0.0;
    regress_add(&r, x, 3.0);
    for (j = 0; j < p; j++) {
        x[j] = 1.0;
        regress_add(&r, x, 3.0 + 0.25 * (j + 1));
        x[j] = 0.0;
    }
    for (j = 0; j + 1 < p && j < 10; j++) {
        x[j] = 1.0; x[j + 1] = 1.0;
        regress_add(&r, x, 3.0 + 0.25 * (j + 1) + 0.25 * (j + 2));
        x[j] = 0.0; x[j + 1] = 0.0;
    }

    CHECK(regress_solve(&r, t_beta, t_scratch, &f) == 0, "wide: a full-width fit solves");
    CHECK(f.pinned == 0, "wide: full rank");
    CHECK(fabs(t_beta[0] - 3.0) < 1e-6, "wide: intercept");
    for (j = 0; j < p; j++)
        if (fabs(t_beta[j + 1] - 0.25 * (j + 1)) > 1e-6) ok = 0;
    CHECK(ok, "wide: every one of the terms comes back");
    CHECK(f.r2 > 0.999999, "wide: R2");

    /* And the schema will carry that many named columns. */
    {
        static char  names[LOS_MAX_VARS][LOS_NAME_MAX];
        static char *namep[LOS_MAX_VARS];
        for (i = 0; i < LOS_MAX_VARS; i++) {
            (void)snprintf(names[i], sizeof names[i], "term_%d", i);
            namep[i] = names[i];
        }
        CHECK(los_schema_set((const char *const *)namep, LOS_MAX_VARS) == 0, "wide: a full-width schema");
        CHECK(los_nvars() == LOS_MAX_VARS, "wide: all of it kept");
        CHECK(streq(los_var_name(LOS_MAX_VARS - 1), names[LOS_MAX_VARS - 1]),
              "wide: the last column is named");
        los_free();
    }
}

/* Finding the files: the reason `linearr` used to work only in its own source
 * directory. g_prog must be set before the first call: the program directory
 * is worked out once and remembered. */
static void test_resolve(void) {
    char path[RESOLVE_PATH_MAX];

    g_prog = "./linearr_ut";
    /* Absolute, because argv[0] is run through realpath first: a symlinked
     * binary must find the files beside the REAL one, not beside the link. */
    CHECK(resolve_program_dir() != NULL && resolve_program_dir()[0] == '/',
          "resolve: the program directory is resolved to a real absolute path");

    CHECK(resolve_file(COEF, path, sizeof path) == 0, "resolve: finds a file in the cwd");
    CHECK(strcmp(path, COEF) == 0, "resolve: and prefers the cwd copy, unchanged");

    CHECK(resolve_file("/no/such/absolute", path, sizeof path) == -1,
          "resolve: an absolute path that is not there fails");
    CHECK(resolve_file("no-such-file-anywhere", path, sizeof path) == -1,
          "resolve: a name that is nowhere fails");
    /* The failure is not silent: out says where it looked, for the error. */
    CHECK(strstr(path, "looked in") != NULL, "resolve: says where it looked");
}

/* Naming the terms instead of counting commas. */
static void test_named_case(void) {
    char out[MAX_OUTPUT], row[MAX_INPUT];
    char *a[2];
    int on[2];

    a[0] = (char *)"Cardioversion=1";
    a[1] = (char *)"icu_indicator=1";
    CHECK(process_named("001", a, 2, out, sizeof out) == 0, "named: scores");

    /* It must agree with the row form to the character. Two ways of writing the
     * same case that disagree would be worse than having only one. */
    CHECK(los_load_both() == 0, "named: schema for the row form");
    on[0] = 0; on[1] = 16;
    make_case(row, sizeof row, "001", on, 2);
    los_free();
    {
        char rowout[MAX_OUTPUT];
        CHECK(process(row, rowout, sizeof rowout) == 0, "named: the row form scores");
        CHECK(strcmp(out, rowout) == 0, "named: both forms give the same answer");
    }

    /* Terms nobody mentioned are 0, which is the whole point at 256 columns. */
    a[0] = (char *)"icu_indicator=0";
    CHECK(process_named("001", a, 1, out, sizeof out) == 0, "named: one term");
    CHECK(strstr(out, "prediction=6.4832") != NULL, "named: unmentioned terms are 0");

    /* Case-insensitive, because a column name is a label, not an identifier. */
    a[0] = (char *)"ICU_INDICATOR=1";
    CHECK(process_named("001", a, 1, out, sizeof out) == 0, "named: case-insensitive");

    /* Every refusal says which thing was wrong, by name. */
    a[0] = (char *)"nosuchterm=1";
    CHECK(process_named("001", a, 1, out, sizeof out) == -1, "named: unknown term refused");
    CHECK(strstr(process_error(), "nosuchterm") != NULL, "named: and it names the term");

    a[0] = (char *)"icu_indicator=yes";
    CHECK(process_named("001", a, 1, out, sizeof out) == -1, "named: non-number refused");
    CHECK(strstr(process_error(), "yes") != NULL, "named: and it quotes the value");

    a[0] = (char *)"icu_indicator";
    CHECK(process_named("001", a, 1, out, sizeof out) == -1, "named: missing '=' refused");

    CHECK(process_named("nosuchgroup", NULL, 0, out, sizeof out) == -1,
          "named: unknown group refused");
    CHECK(strstr(process_error(), "nosuchgroup") != NULL, "named: and it names the group");

    /* The schema is reportable, which is what --terms prints. */
    CHECK(process_nterms() == 24, "named: term count");
    CHECK(process_ngroups() == 12, "named: group count");
    CHECK(streq(process_term_name(16), "icu_indicator"), "named: term by index");
    CHECK(process_coef_path() && strstr(process_coef_path(), "coefficients.csv") != NULL,
          "named: the table it opened");

    process_free();
}

/* QR reaches the same answer without forming X'X, so it keeps the digits that
 * squaring the condition number throws away. On a well-conditioned design the
 * two agree; on a bad one QR is right and the normal equations are not. */
static void test_qr(void) {
    struct qr q;
    struct regress r;
    struct regress_fit fq, fr;
    double x[2];
    int i;

    /* Agreement where it should not matter. */
    qr_init(&q, 2, t_store);
    x[0] = 0; x[1] = 0; qr_add(&q, x, 2.0);
    x[0] = 1; x[1] = 0; qr_add(&q, x, 5.0);
    x[0] = 0; x[1] = 1; qr_add(&q, x, 1.0);
    x[0] = 2; x[1] = 3; qr_add(&q, x, 5.0);
    CHECK(qr_solve(&q, t_beta, NULL, &fq) == 0, "qr: solves");
    CHECK(NEAR(t_beta[0], 2.0) && NEAR(t_beta[1], 3.0) && NEAR(t_beta[2], -1.0),
          "qr: recovers y = 2 + 3x1 - x2");
    CHECK(fq.pinned == 0 && fq.df == 1, "qr: rank and df");
    CHECK(fq.r2 > 0.999999, "qr: R2");

    /* The case the module exists for: two columns alike to the sixth decimal.
     * The normal equations lose about five digits here and QR loses about one. */
    {
        double eq, eqr;
        qr_init(&q, 2, t_store);
        regress_init(&r, 2, t_store2);
        for (i = 0; i < 40; i++) {
            x[0] = 1.0 + i * 0.01;
            x[1] = x[0] + ((i % 2) ? 1.0e-6 : 0.0);
            qr_add(&q, x, 1.0 + 2.0 * x[0] + 3.0 * x[1]);
            regress_add(&r, x, 1.0 + 2.0 * x[0] + 3.0 * x[1]);
        }
        qr_solve(&q, t_beta, NULL, &fq);
        eqr = fabs(t_beta[1] - 2.0);
        regress_solve(&r, t_beta2, t_scratch, &fr);
        eq = fabs(t_beta2[1] - 2.0);
        CHECK(eqr < eq / 100.0, "qr: at least two orders more accurate here");
        CHECK(eqr < 1e-7, "qr: and close to the truth in absolute terms");
        /* cond(X) rather than cond(X'X), so it is the smaller number. */
        CHECK(fq.condition < fr.condition, "qr: reports the unsquared conditioning");
    }

    /* THE UNITS TEST, for QR as for the normal equations. The first version of
     * this module judged each diagonal of R against the LARGEST column's
     * magnitude, so a term in a small unit was deleted for being small: the
     * same defect regress.c documents as fixed. A reviewer produced a case
     * where an indicator worth 5 was deleted beside a column of size 1e15 and
     * the fit then reported R2=1.0000 for a model whose residuals were 4. */
    {
        int u;
        for (u = 0; u < 3; u++) {
            double unit = (u == 0) ? 1.0 : (u == 1) ? 1e-6 : 1e-13;
            qr_init(&q, 2, t_store);
            for (i = 0; i < 30; i++) {
                x[0] = 1.0 + i * 0.1;
                x[1] = (1.0 + sin((double)i)) * unit;
                qr_add(&q, x, 1.0 + 2.0 * x[0] + (3.0 / unit) * x[1]);
            }
            CHECK(qr_solve(&q, t_beta, NULL, &fq) == 0, "qr units: solves");
            CHECK(fq.pinned == 0, "qr units: nothing deleted for being small");
            CHECK(fabs(t_beta[1] - 2.0) < 1e-6, "qr units: the ordinary slope");
            CHECK(fabs(t_beta[2] * unit - 3.0) < 1e-6, "qr units: the small one");
        }
    }

    /* When a column IS dropped, the residual of the rotation is the residual of
     * a model that was never returned. Withheld rather than reported: it once
     * understated the error by fifteen orders of magnitude. */
    {
        qr_init(&q, 2, t_store);
        for (i = 0; i < 20; i++) {
            x[0] = (double)i;
            x[1] = 7.0;                       /* constant: will be dropped */
            qr_add(&q, x, 3.0 + 2.0 * x[0] + ((i % 3) - 1));
        }
        qr_solve(&q, t_beta, NULL, &fq);
        CHECK(fq.pinned == 1, "qr: the constant column is dropped");
        CHECK(fq.r2 < 0.0 && fq.sigma < 0.0,
              "qr: and R2 and the residual SD are withheld, not guessed");
    }

    /* A column that never varies is dropped, as in the normal equations. */
    qr_init(&q, 2, t_store);
    x[0] = 0; x[1] = 7; qr_add(&q, x, 1.0);
    x[0] = 1; x[1] = 7; qr_add(&q, x, 3.0);
    x[0] = 2; x[1] = 7; qr_add(&q, x, 5.0);
    CHECK(qr_solve(&q, t_beta, NULL, &fq) == 0, "qr: solves a rank-deficient design");
    CHECK(fq.pinned == 1, "qr: the constant column is pinned");
    CHECK(NEAR(t_beta[1], 2.0), "qr: the identified slope is right");
    CHECK(fq.term[1] == REGRESS_CONSTANT,
          "qr: a column with no spread is reported constant, not collinear");
    CHECK(NEAR(t_beta[0], 1.0), "qr: and the intercept is never dropped");

    /* Non-finite input is refused at the door, as in regress.c. */
    qr_init(&q, 1, t_store);
    x[0] = 1.0;
    CHECK(qr_add(&q, x, 0.0 / 0.0) == -1, "qr: a NaN response is refused");
    CHECK(qr_solve(&q, t_beta, NULL, &fq) == -1, "qr: an empty sample is not a fit");
    CHECK(qr_init(&q, REGRESS_MAX_VARS + 1, t_store) == -1, "qr: refuses too many terms");
}

/* The residual checks. Structure in the residuals is what says a straight line
 * was the wrong shape, and every number in the fit summary is an average over
 * them, so none of those can see it. */
static void test_diag(void) {
    static double dstore[REGRESS_MAX_VARS * 3 + 8];
    struct diag d;
    struct diag_result r;
    double x[1];
    int i;

    /* A parabola fitted with a line: the residual is x^2 up to a constant. */
    diag_init(&d, 1, dstore);
    for (i = -10; i <= 10; i++) {
        x[0] = (double)i;
        diag_add(&d, x, (double)(i * i) - 38.5, 24.0);
    }
    diag_result(&d, &r);
    CHECK(r.curved_term == 0, "diag: names the term whose shape is wrong");
    CHECK(fabs(r.curved_t) > 5.0, "diag: and the evidence is strong");

    /* An error that grows with the prediction. */
    diag_init(&d, 1, dstore);
    for (i = 1; i <= 60; i++) {
        x[0] = (double)i;
        diag_add(&d, x, ((i % 2) ? 1.0 : -1.0) * (double)i, (double)i * 2.0);
    }
    diag_result(&d, &r);
    CHECK(fabs(r.spread_t) > 3.5, "diag: sees the error growing with the prediction");

    /* A correct model with ordinary noise must stay silent, or the check is
     * worthless: a warning that always fires is not a warning. */
    diag_init(&d, 1, dstore);
    for (i = 0; i < 200; i++) {
        x[0] = (double)(i % 17);
        diag_add(&d, x, ((i * 7919) % 23) - 11.0, 5.0 + x[0]);
    }
    diag_result(&d, &r);
    CHECK(r.curved_term == -1, "diag: quiet on an unstructured residual");
    CHECK(r.spread_t == 0.0, "diag: and quiet about its spread");

    /* THE LOCATION TEST. The same quadratic with x moved away from zero. The
     * first version correlated against RAW x^2, and since the residual is
     * already orthogonal to x, the signal fell from 0.20 to 0.0001 as the
     * offset grew: invisible on any variable with an origin, which is most of
     * them. Partialling x^2 on [1, x] makes it invariant. */
    {
        int k;
        double first = 0.0;
        for (k = 0; k < 4; k++) {
            double off = (k == 0) ? 0.0 : (k == 1) ? 1.0 : (k == 2) ? 10.0 : 1000.0;
            diag_init(&d, 1, dstore);
            for (i = -60; i <= 60; i++) {
                double xx = off + i * 0.05;
                double c = (xx - off) * (xx - off);
                x[0] = xx;
                diag_add(&d, x, c - 3.0, 24.0);      /* residual is the curve */
            }
            diag_result(&d, &r);
            CHECK(r.curved_term == 0, "diag location: the curve is seen at every offset");
            if (k == 0) first = fabs(r.curved_t);
            else CHECK(fabs(fabs(r.curved_t) - first) < 0.01 * first,
                       "diag location: and with the same strength");
        }
    }

    /* A cubic is odd in x, and the square probe is even, so it cannot see one.
     * The cube probe can. */
    {
        diag_init(&d, 1, dstore);
        for (i = -60; i <= 60; i++) {
            x[0] = i * 0.05;
            diag_add(&d, x, x[0] * x[0] * x[0], 5.0);
        }
        diag_result(&d, &r);
        CHECK(r.curved_term == 0, "diag: a cubic departure is seen");
        CHECK(r.curved_pow == 3, "diag: and it is named as a cube, not a square");
    }

    /* Too few rows to say anything. */
    diag_init(&d, 1, dstore);
    for (i = 0; i < 5; i++) { x[0] = (double)i; diag_add(&d, x, (double)(i*i), 1.0); }
    diag_result(&d, &r);
    CHECK(r.curved_term == -1, "diag: says nothing from five rows");
}

static void test_los_round(void) {
    /* Half away from zero, NOT printf's round half to even, which would make
     * these 2 and -2. */
    CHECK(NEAR(los_round(2.5, 0), 3.0), "round: half up");
    CHECK(NEAR(los_round(-2.5, 0), -3.0), "round: half away from zero when negative");
    CHECK(NEAR(los_round(15.63514, 4), 15.6351), "round: to four digits");
    CHECK(NEAR(los_round(46.5139, 1), 46.5), "round: to one digit");
}

static void test_los_schema(void) {
    char *names[3];
    char  header[MAX_OUTPUT];

    names[0] = (char *)"km"; names[1] = (char *)"stops"; names[2] = (char *)"";

    CHECK(los_schema_set((const char *const *)names, 2) == 0, "schema: set");
    CHECK(los_nvars() == 2, "schema: term count");
    CHECK(streq(los_var_name(1), "stops"), "schema: names in order");
    CHECK(los_var_name(2) == NULL, "schema: nothing past the last term");
    CHECK(los_format_header(header, sizeof header) == 0 &&
          strcmp(header, "group,intercept,km,stops") == 0, "schema: writes its own header");
    CHECK(los_var_index("stops") == 1, "schema: term by name");
    CHECK(los_var_index("STOPS") == 1, "schema: name lookup ignores case");
    CHECK(los_var_index("nope") == -1, "schema: unknown name");

    CHECK(los_schema_set((const char *const *)names, 0) == -1, "schema: refuses no terms");
    CHECK(los_schema_set((const char *const *)names, LOS_MAX_VARS + 1) == -1, "schema: refuses too many terms");
    CHECK(los_schema_set((const char *const *)names, 3) == -1, "schema: refuses an empty column name");
    /* A rejected header must leave the working schema alone, not half-replace it. */
    CHECK(los_nvars() == 2 && streq(los_var_name(0), "km"),
          "schema: a rejected header changes nothing");
    los_free();
}

static void test_los_tables(void) {
    const struct los_model *m;

    CHECK(los_load_both() == 0, "los: tables load");
    /* The coefficient header defines the model: 24 terms in this example table,
     * two in example/simple-train.csv, and neither is compiled in anywhere. */
    CHECK(los_nvars() == 24, "los: the table's header set the term count");
    CHECK(streq(los_var_name(0), "Cardioversion"), "los: first term named");
    CHECK(streq(los_var_name(16), "icu_indicator"), "los: term 17 named");

    m = los_model_get("001");
    CHECK(m != NULL, "los: group 001 is in the table");
    if (m) {
        CHECK(NEAR(m->intercept, 6.4832), "los: 001 intercept");
        CHECK(NEAR(m->b[0], 6.0308), "los: 001 Cardioversion");
        CHECK(NEAR(m->b[16], 7.4471), "los: 001 icu_indicator");
        CHECK(NEAR(m->b[1], 0.0), "los: 001 Cell_saver is zero");
        CHECK(NEAR(m->trim_addition, 26.5528), "los: 001 trim addition");
    }
    CHECK(los_model_get("nosuchgroup") == NULL, "los: unknown group");
    CHECK(los_load("no-such-file") == -1, "los: missing table is an error");
    CHECK(los_model_get("001") == NULL, "los: a failed load leaves no half-built table");
    CHECK(los_nvars() == 0, "los: a failed load leaves no schema");
    los_free();
    los_free();                                  /* idempotent */
}

static void test_los_trims(void) {
    const struct los_model *m;

    /* Coefficients alone are a working model: every trim addition is 0 and the
     * trim point is the prediction. A table produced by -t has no trim file,
     * and demanding one made such a table impossible to score against. */
    CHECK(los_load(COEF) == 0, "trims: coefficients load on their own");
    m = los_model_get("001");
    CHECK(m && m->trim_addition == 0.0, "trims: none loaded means an addition of 0");
    if (m) CHECK(NEAR(los_trim_point(m, 10.0), 10.0), "trims: so the trim point is the prediction");

    CHECK(los_load_trims(TRIM) == 0, "trims: load onto the table");
    m = los_model_get("001");
    CHECK(m && NEAR(m->trim_addition, 26.5528), "trims: attached to their group");

    CHECK(los_load_trims("no-such-file") == -1, "trims: a missing file is an error");
    los_free();
    CHECK(los_load_trims(TRIM) == -1, "trims: refuse to load with no table to attach to");
}

static void test_los_case(void) {
    struct los_case c;
    char line[MAX_INPUT];
    double los;
    int on[2];

    CHECK(los_load_both() == 0, "los: tables load for the case tests");

    on[0] = 0; on[1] = 16;                       /* Cardioversion, ICU */
    make_case(line, sizeof line, "001", on, 2);
    CHECK(los_parse_case(line, &c) == 0, "los: parse a case");
    CHECK(strcmp(c.group, "001") == 0, "los: case group");
    CHECK(c.x[0] == 1.0 && c.x[16] == 1.0 && c.x[1] == 0.0, "los: case terms");

    CHECK(los_parse_case("001,1,1", &c) == -1, "los: refuses the wrong field count");
    make_case(line, sizeof line, "001", on, 2);
    line[strlen(line) - 1] = 'x';
    CHECK(los_parse_case(line, &c) == -1, "los: refuses a field that is not a number");
    CHECK(los_parse_case(",0,0", &c) == -1, "los: refuses an empty group");

    CHECK(los_parse_training("001,12.5,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0",
                             &c, &los) == 0, "los: parse a training row");
    CHECK(NEAR(los, 12.5) && c.x[0] == 1.0, "los: training row carries the observed value");
    los_free();
}

static void test_los_predict(void) {
    struct los_model m;
    struct los_case c;
    char *names[2];
    int i;

    names[0] = (char *)"a"; names[1] = (char *)"b";
    los_schema_set((const char *const *)names, 2);

    for (i = 0; i < LOS_MAX_VARS; i++) { m.b[i] = 0.0; c.x[i] = 0.0; }
    m.intercept = 2.0;
    m.trim_addition = 10.0;
    m.b[0] = 3.0; c.x[0] = 2.0;
    m.b[1] = 5.0;                                /* a coefficient with no case */

    CHECK(NEAR(los_predict(&m, &c), 8.0), "los: prediction is the intercept plus the terms");
    CHECK(NEAR(los_trim_point(&m, 8.0), 18.0), "los: trim point is the prediction plus the addition");
    los_free();
}

static void test_process_score(void) {
    char line[MAX_INPUT], out[MAX_OUTPUT];
    int on[2];

    CHECK(los_load_both() == 0, "process: schema for the scoring tests");

    /* 6.4832 (intercept) + 6.0308 (Cardioversion) + 7.4471 (ICU) = 19.9611,
     * and 19.9611 + 26.5528 (001's trim addition) = 46.5139 -> 46.5. */
    on[0] = 0; on[1] = 16;
    make_case(line, sizeof line, "001", on, 2);
    los_free();                                  /* process() loads its own */

    CHECK(process(line, out, sizeof out) == 0, "process: scores a case");
    CHECK(strcmp(out, "001 prediction=19.9611 trim=46.5") == 0,
          "process: the prediction and the trim point");

    CHECK(process(line, out, 8) == -1, "process: guards a small buffer");
    CHECK(process("001,1,2", out, sizeof out) == -1, "process: refuses a malformed case");

    on[0] = 5; on[1] = 16;
    make_case(line, sizeof line, "001", on, 2);
    CHECK(process(line, out, sizeof out) == 0 &&
          strcmp(out, "001 prediction=19.9611 trim=46.5") != 0,
          "process: distinguishes cases");

    on[0] = 0; on[1] = 16;
    make_case(line, sizeof line, "nosuchgroup", on, 2);
    CHECK(process(line, out, sizeof out) == -1, "process: refuses an unknown group");

    process_free();
}

static void test_process_train(void) {
    char out[MAX_OUTPUT], expect[MAX_OUTPUT], header[MAX_OUTPUT];
    struct fit_info info;
    const struct los_model *m;

    /* example/train.csv was generated from the example coefficients, so fitting
     * it must give those coefficients back, to every printed digit, which is
     * the strongest statement the fitter can make about itself. */
    CHECK(los_load(COEF) == 0, "train: reference table loads");
    m = los_model_get("001");
    CHECK(m != NULL, "train: reference group present");
    if (m) {
        struct los_model ref = *m;
        double got[LOS_MAX_VARS + 2];
        int k, nf, same = 1;
        ref.trim_addition = 0.0;
        CHECK(los_format_header(header, sizeof header) == 0, "train: format reference header");
        (void)expect;

        CHECK(process_train("example/train.csv", "001", out, sizeof out, &info) == 0,
              "train: fits group 001");
        nf = coef_row(strchr(out, '\n') + 1, got, LOS_MAX_VARS + 2);
        CHECK(nf == 25, "train: the fitted row has an intercept and 24 terms");
        if (fabs(got[0] - ref.intercept) > 1e-9) same = 0;
        for (k = 1; k < nf; k++)
            if (fabs(got[k] - ref.b[k - 1]) > 1e-9) same = 0;
        CHECK(same, "train: recovers the coefficients it was generated from");
        CHECK(info.r2 > 0.999999999, "train: R2 is 1 on exactly linear data");
        CHECK(info.rows == 17, "train: used only group 001's rows");
        /* 25 terms, 8 of them identified by this sample (the intercept and the
         * 7 live ones): the other 17 are 0, leaving 17 - 8 = 9 residual df. */
        CHECK(info.pinned == 17, "train: pins the terms the sample cannot identify");
        CHECK(info.df == 9, "train: reports the residual degrees of freedom");
    }

    /* The fitted output is a coefficient FILE, header and all, so it reads back. */
    CHECK(strncmp(out, "group,intercept,Cardioversion,", 30) == 0,
          "train: output carries its own header");
    CHECK(strchr(out, '\n') != NULL && strstr(out, "\n001,") != NULL,
          "train: header then the fitted row");

    CHECK(process_train("example/train.csv", "nosuchgroup", out, sizeof out, NULL) == -1,
          "train: a group with no rows is an error");
    CHECK(process_train("no-such-file", "001", out, sizeof out, NULL) == -1,
          "train: a missing training file is an error");
    CHECK(process_train("example/train.csv", "001", out, 16, NULL) == -1,
          "train: guards a small buffer");
    CHECK(process_train("example/train.csv", "*", out, sizeof out, &info) == 0,
          "train: pools every row under '*'");
    CHECK(info.rows == 34, "train: pooled row count");

    los_free();
    process_free();
}

static void test_other_schema(void) {
    char out[MAX_OUTPUT];
    struct fit_info info;

    /* The whole claim of this program in one test: a file with two columns
     * nobody wrote any code for, fitted by the same binary that does the
     * twenty-four-term model. MINUTES = 5 + 2.5*km + 1.5*stops. */
    CHECK(process_train("example/simple-train.csv", "A", out, sizeof out, &info) == 0,
          "other schema: fits a two-term file");
    CHECK(los_nvars() == 2, "other schema: took its terms from that file's header");
    {
        double got[4];
        CHECK(strncmp(out, "group,intercept,km,stops\nA,", 27) == 0,
              "other schema: header and group");
        CHECK(coef_row(strchr(out, '\n') + 1, got, 4) == 3 &&
              fabs(got[0] - 5.0) < 1e-9 && fabs(got[1] - 2.5) < 1e-9 &&
              fabs(got[2] - 1.5) < 1e-9,
              "other schema: recovers 5 + 2.5*km + 1.5*stops");
    }
    CHECK(info.pinned == 0 && info.df == 4, "other schema: full rank, 4 df");
    CHECK(info.r2 > 0.999999999, "other schema: R2");

    CHECK(process_train("example/simple-train.csv", "B", out, sizeof out, NULL) == 0,
          "other schema: the second group");
    {
        double got[4];
        CHECK(coef_row(strchr(out, '\n') + 1, got, 4) == 3 &&
              fabs(got[0] - 12.0) < 1e-9 && fabs(got[1] - 2.5) < 1e-9 &&
              fabs(got[2] - 1.5) < 1e-9,
              "other schema: same slopes, its own intercept");
    }

    los_free();
    process_free();
}

int main(void) {
    test_utils();
    test_hash();
    test_params();
    test_csv();
    test_regress();
    test_wide_fit();
    test_qr();
    test_diag();
    test_resolve();
    test_named_case();
    test_los_round();
    test_los_schema();
    test_los_tables();
    test_los_trims();
    test_los_case();
    test_los_predict();
    test_process_score();
    test_process_train();
    test_other_schema();
    (void)printf("ut: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

#else
typedef int tests_translation_unit_not_empty;  /* ISO C forbids an empty TU */
#endif /* UNIT_TEST */
