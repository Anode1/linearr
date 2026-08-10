/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* tests.c: in-place unit tests, run by `make ut` (which builds every source
 * with -DUNIT_TEST; this file is empty otherwise, and main.c's main() is then
 * compiled out). Add a CHECK when you add a feature. Idempotent, and run from
 * the project root: example/ is read by relative path. */
#ifdef UNIT_TEST

#include "common.h"
#include "utils.h"
#include "hash.h"
#include "csv.h"
#include "regress.h"
#include "qr.h"
#include "diag.h"
#include "los.h"
#include "process.h"
#include "resolve.h"
#include "constants.h"
#include "canon.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int pass, fail;

/* argv[0], so test_resolve can ask about the binary that is actually running
 * rather than about a name written down a second time in a file that cannot
 * see the Makefile. Kept as a fallback for a caller that passes none. */
static const char *t_argv0 = "./linearr_ut";
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

#define COEF "example/coefficients.csv"
#define TRIM "example/trim_additions.csv"

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

/* The two roundings, which were keys in a properties file and are options now.
 * The properties path accepted a value outside 0..9 by falling back to the
 * default without a word; the option refuses it. */
static void test_scales(void) {
    CHECK(process_set_scale(0) == 0 && process_set_scale(9) == 0,
          "scale: 0 and 9 are accepted");
    CHECK(process_set_scale(-1) == -1 && process_set_scale(10) == -1,
          "scale: outside 0..9 is refused, not silently defaulted");
    CHECK(process_set_trim_scale(3) == 0 && process_set_trim_scale(99) == -1,
          "trim scale: the same");
    (void)process_set_scale(4);              /* leave the defaults as found */
    (void)process_set_trim_scale(1);
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
static double t_diag[DIAG_PER_TERM * 2 + DIAG_SHARED];   /* one term */
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

    /* This binary's own argv[0], not a copy of its name. It was written out as
     * "./linearr_ut", which is the Makefile's TESTBIN spelled a second time in
     * a file that cannot see the Makefile: renaming one and not the other made
     * resolve_program_dir() resolve a path that does not exist. */
    g_prog = t_argv0;
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

    /* Named, because there is no longer anything to fall back to. Scoring used
     * to find a table by searching: a properties file, then one shipped beside
     * the binary. These tests passed on that search, which meant they were also
     * testing the search rather than the scoring. */
    process_use_coef(COEF);
    process_use_trim(TRIM);

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

    /* When a column IS dropped, the residual of the ROTATION belongs to a model
     * that was never returned; it once understated the error by fifteen orders
     * of magnitude. The answer used to be to withhold R2 and the residual SD,
     * which left a fit nobody could judge. Now the residual of the model
     * actually returned is computed, and this is the test that it is the right
     * number: the same data fitted WITHOUT the redundant column must give the
     * same residual, because it is the same model. */
    {
        struct qr q2;
        struct regress_fit f2;
        double x1[1];
        qr_init(&q, 2, t_store);
        qr_init(&q2, 1, t_store2);
        for (i = 0; i < 20; i++) {
            double y = 3.0 + 2.0 * (double)i + (double)((i % 3) - 1);
            x[0] = (double)i;
            x[1] = 7.0;                       /* constant: will be dropped */
            x1[0] = (double)i;
            (void)qr_add(&q, x, y);
            (void)qr_add(&q2, x1, y);
        }
        (void)qr_solve(&q, t_beta, NULL, &fq);
        (void)qr_solve(&q2, t_beta2, NULL, &f2);
        CHECK(fq.pinned == 1, "qr: the constant column is dropped");
        CHECK(fq.rss > 0.0 && fq.sigma > 0.0,
              "qr: and the residual is reported, not withheld");
        CHECK(fabs(fq.rss - f2.rss) < 1e-9 * f2.rss,
              "qr: the residual is that of the model returned, not of the rotation");
        CHECK(fabs(fq.sigma - f2.sigma) < 1e-9 * f2.sigma,
              "qr: and so is the residual SD");
        CHECK(fabs(fq.r2 - f2.r2) < 1e-12, "qr: and R2");
        CHECK(fabs(t_beta[1] - t_beta2[1]) < 1e-12,
              "qr: the slope is the one the reduced model gives");
    }

    /* The same, with a column that is collinear rather than constant, and with
     * the whole design sitting a long way from zero: the case the 2-norm scale
     * exists for. A term that merely has an offset must not be deleted. */
    {
        qr_init(&q, 2, t_store);
        for (i = 0; i < 30; i++) {
            x[0] = 1.0e6 + (double)i;              /* offset, but informative */
            x[1] = 5.0e3 + (double)(i % 7) * 0.5;  /* offset, and independent */
            /* Noiseless on purpose. With noise the fitted slope differs from
             * the one the data was built from by the amount the noise moves
             * it, which on thirty rows is a few per cent, and a test that then
             * demands six digits is testing the noise. */
            qr_add(&q, x, 1.0 + 2.0 * x[0] - 4.0 * x[1]);
        }
        (void)qr_solve(&q, t_beta, NULL, &fq);
        CHECK(fq.pinned == 0, "qr scale: an offset column is not mistaken for collinear");
        CHECK(fabs(t_beta[1] - 2.0) < 1e-6 && fabs(t_beta[2] + 4.0) < 1e-6,
              "qr scale: and both slopes come back");
        /* And a genuinely dependent column at the same offsets IS dropped, so
         * the loosened scale has not simply stopped finding rank deficiency. */
        qr_init(&q, 2, t_store);
        for (i = 0; i < 30; i++) {
            x[0] = 1.0e6 + (double)i;
            x[1] = 3.0 + (x[0] - 1.0e6) * 0.5;     /* exactly a function of x0 */
            qr_add(&q, x, 1.0 + 2.0 * x[0]);
        }
        (void)qr_solve(&q, t_beta, NULL, &fq);
        CHECK(fq.pinned == 1, "qr scale: a dependent column at the same offset is dropped");
        CHECK(fq.term[1] == REGRESS_COLLINEAR,
              "qr scale: and reported collinear rather than constant");
        /* Where it stops working, under test so it cannot quietly change. At
         * an offset of 1e9 an uncentred factorisation cannot separate a
         * dependent column from an independent one; the fit says so through
         * cond= and not through the rank. See QR_RANK_EPS in qr.c. */
        qr_init(&q, 2, t_store);
        for (i = 0; i < 30; i++) {
            x[0] = 1.0e9 + (double)i;
            x[1] = 3.0 + (x[0] - 1.0e9) * 0.5;
            qr_add(&q, x, 1.0 + 2.0 * x[0]);
        }
        (void)qr_solve(&q, t_beta, NULL, &fq);
        CHECK(fq.condition > 1e7,
              "qr scale: at 1e9 the rank test misses, and cond= reports it instead");

        /* And a column so large that squaring it overflows. colss used to be a
         * plain sum of squares, so any column past about 1.3e154 sent it to
         * +inf, made the scaled diagonal |R_ii|/inf = 0, and deleted a
         * perfectly identified term as COLLINEAR. The fit must be the same at
         * every magnitude, since scaling a column scales its coefficient and
         * nothing else. */
        {   double huge[] = { 1.0, 1.0e153, 1.0e154, 1.0e200 };
            size_t h;
            for (h = 0; h < sizeof huge / sizeof huge[0]; h++) {
                qr_init(&q, 2, t_store);
                for (i = 1; i <= 6; i++) {
                    x[0] = (double)i * huge[h];
                    x[1] = (double)(i % 3) + 1.0;
                    qr_add(&q, x, 2.0 * (double)i + x[1]);
                }
                (void)qr_solve(&q, t_beta, NULL, &fq);
                CHECK(fq.pinned == 0,
                      "qr scale: a column that overflows when squared is not deleted");
                CHECK(fq.df == 3, "qr scale: and the rank is the same at every magnitude");
                CHECK(fabs(t_beta[2] - 1.0) < 1e-9,
                      "qr scale: the unscaled column keeps its coefficient");
            }
        }
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
     * them. Partialling x^2 on [1, x], about the column's own centre, makes it
     * invariant.
     *
     * The curve carries noise on purpose. Without it the residual is an EXACT
     * quadratic, the correlation is 1 to the last bit, and what the test then
     * compares across offsets is the rounding error in 1 - r^2, which is free
     * to vary by a factor of ten and did. An exact relation is checked
     * separately, below, and only for the cap it is supposed to print. */
    {
        int k;
        double first = 0.0;
        for (k = 0; k < 5; k++) {
            double off = (k == 0) ? 0.0 : (k == 1) ? 1.0 : (k == 2) ? 10.0 :
                         (k == 3) ? 1000.0 : 1000000.0;
            diag_init(&d, 1, dstore);
            for (i = -60; i <= 60; i++) {
                double u = i * 0.05;
                double noise = ((double)((i * 7919) % 23) - 11.0) * 0.1;
                x[0] = off + u;
                diag_add(&d, x, u * u + noise, 24.0);
            }
            diag_result(&d, &r);
            CHECK(r.curved_term == 0, "diag location: the curve is seen at every offset");
            if (k == 0) first = fabs(r.curved_t);
            else CHECK(fabs(fabs(r.curved_t) - first) < 0.01 * first,
                       "diag location: and with the same strength");
        }
    }

    /* An exact relation prints the cap rather than a t of 1e8: past r = 1 to
     * within 1e-12 the divisor has no digits left and the number would be an
     * artefact of the summation order. */
    {
        int k;
        for (k = 0; k < 4; k++) {
            double off = (k == 0) ? 0.0 : (k == 1) ? 1.0 : (k == 2) ? 10.0 : 1000.0;
            diag_init(&d, 1, dstore);
            for (i = -60; i <= 60; i++) {
                double u = i * 0.05;
                x[0] = off + u;
                diag_add(&d, x, u * u - 3.0, 24.0);
            }
            diag_result(&d, &r);
            CHECK(r.curved_term == 0 && fabs(r.curved_t) == 9999.0,
                  "diag: an exact curve reports the cap, at any offset");
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

/* ---------------------------------------------------------------------------
 * The certified sets. See canon.h for why these are here and the rest of this
 * file is not enough on its own.
 *
 * The tolerances below are relative and were MEASURED, not chosen: each is
 * roughly ten times the error the solver actually makes today, which is loose
 * enough to survive a compiler with a different order of operations and tight
 * enough that losing a digit fails the build. They are stated per solver
 * because the two do not agree, and the difference is the point.
 */
static double worst_rel(const double *got, const double *want, int m) {
    double w = 0.0;
    int i;
    for (i = 0; i < m; i++) {
        double d = fabs(got[i] - want[i]);
        double s = fabs(want[i]);
        double rel = (s > 1e-300) ? d / s : d;    /* Wampler's zero residual */
        if (rel > w) w = rel;
    }
    return w;
}

/* Fit one set with each solver and report the worst relative error in the
 * coefficients, plus the residual SD each reported. */
static void canon_fit(const struct canon_set *c, int use_qr, double *rel,
                      double *sigma, double *r2) {
    struct regress_fit f;
    struct regress rg;
    struct qr q;
    const double *row;
    int i;

    if (use_qr) (void)qr_init(&q, c->nvars, t_store);
    else        (void)regress_init(&rg, c->nvars, t_store);
    for (i = 0; i < c->n; i++) {
        row = c->data + (size_t)i * (c->nvars + 1);
        if (use_qr) (void)qr_add(&q, row + 1, row[0]);
        else        (void)regress_add(&rg, row + 1, row[0]);
    }
    if (use_qr) (void)qr_solve(&q, t_beta, t_scratch, &f);
    else        (void)regress_solve(&rg, t_beta, t_scratch, &f);
    *rel   = worst_rel(t_beta, c->beta, c->nvars + 1);
    *sigma = f.sigma;
    *r2    = f.r2;
}

static void test_canonical(void) {
    double rel, sigma, r2;

    /* NORRIS. The easy set. Both solvers should be at the double's own limit,
     * and if either is not, something ordinary is broken. */
    canon_fit(&canon_norris, 0, &rel, &sigma, &r2);
    CHECK(rel < 1e-12, "Norris: normal equations match the certified coefficients");
    /* 1e-9 and not the 1e-12 the coefficients get. The normal equations form
     * RSS by subtraction, Syy - beta'Sxy, and Norris fits so well that those
     * two agree to five digits before they are subtracted. What survives is
     * the certified value to about eleven digits. This is not a defect to fix
     * in this solver; it is what one pass over X'X can tell you, and it is
     * measured as a property of its own below. */
    CHECK(fabs(sigma - canon_norris.sigma) < 1e-9 * canon_norris.sigma,
          "Norris: and the certified residual SD");
    CHECK(fabs(r2 - canon_norris.r2) < 1e-12, "Norris: and the certified R2");
    canon_fit(&canon_norris, 1, &rel, &sigma, &r2);
    CHECK(rel < 1e-11, "Norris: QR matches the certified coefficients");
    CHECK(fabs(sigma - canon_norris.sigma) < 1e-13 * canon_norris.sigma,
          "Norris: QR and the certified residual SD, to the double's own limit");

    /* LONGLEY. The set published because packages of the day returned two
     * correct digits on it. Both solvers here return eleven. The normal
     * equations manage it only because regress.c accumulates CENTERED
     * co-moments: on the raw cross-products this set is the textbook
     * catastrophe, and a change that quietly drops the centering will be
     * caught here and nowhere else in this file. */
    canon_fit(&canon_longley, 0, &rel, &sigma, &r2);
    CHECK(rel < 1e-9, "Longley: normal equations match the certified coefficients");
    CHECK(fabs(sigma - canon_longley.sigma) < 1e-9 * canon_longley.sigma,
          "Longley: and the certified residual SD");
    CHECK(fabs(r2 - canon_longley.r2) < 1e-11, "Longley: and the certified R2");
    canon_fit(&canon_longley, 1, &rel, &sigma, &r2);
    CHECK(rel < 1e-10, "Longley: QR matches the certified coefficients");
    CHECK(fabs(sigma - canon_longley.sigma) < 1e-10 * canon_longley.sigma,
          "Longley: QR and the certified residual SD");

    /* WAMPLER1. An exact quintic: certified coefficients all 1, certified
     * residual 0. Nothing in the data can absorb a solver's error, so this is
     * the cleanest statement of the difference between the two, and the
     * measured numbers are worth writing down.
     *
     *                 worst coefficient error     reported residual SD
     *   normal eqns          4.4e-9                     2.3e-2
     *   QR                   4.4e-10                    6.7e-11
     *
     * The certified residual SD is zero. The normal equations report 0.023,
     * which is not a small number in a column of y running to four million,
     * but is the honest size of what squaring x^5 threw away. QR reports 7e-11.
     * Neither is wrong about its own arithmetic; only one of them is close to
     * the answer, and this is what --qr is for. */
    canon_fit(&canon_wampler1, 0, &rel, &sigma, &r2);
    CHECK(rel < 1e-7, "Wampler1: normal equations recover the quintic");
    CHECK(sigma > 1e-4 && sigma < 1.0,
          "Wampler1: and report a residual SD that is visibly not zero");
    canon_fit(&canon_wampler1, 1, &rel, &sigma, &r2);
    CHECK(rel < 1e-8, "Wampler1: QR recovers the quintic");
    CHECK(sigma < 1e-6, "Wampler1: and its residual SD is near the certified zero");
    {   /* Stated as a comparison, so the ordering itself is under test: a
         * change that makes QR the worse solver here should not pass. */
        double rq, rn, sq, sn, junk;
        canon_fit(&canon_wampler1, 0, &rn, &sn, &junk);
        canon_fit(&canon_wampler1, 1, &rq, &sq, &junk);
        CHECK(rq < rn, "Wampler1: QR is the more accurate of the two");
        CHECK(sq < sn * 1e-6, "Wampler1: by orders of magnitude on the residual");
    }
}

/* The residual SD from the normal equations loses digits in proportion to how
 * well the model fits, because RSS is recovered as Syy - beta'Sxy and those two
 * agree to more and more places as R^2 approaches 1. Measured on the same data
 * at six noise levels:
 *
 *      1 - R^2      relative error in the reported residual SD
 *      2.5e-01                 8e-16
 *      3.3e-03                 4e-13
 *      3.3e-05                 2e-11
 *      3.3e-07                 1e-09
 *      3.3e-09                 2e-07
 *      3.3e-11                 8e-06
 *
 * A hundredfold better fit costs a hundredfold worse residual SD. QR does not
 * pay it: it carries the residual through the rotation instead of subtracting
 * for it. Nothing in the fit summary would tell you this was happening, which
 * is why it is written down here and in regress.h rather than left to be
 * rediscovered.
 *
 * Under test as a RATIO, not as absolute numbers: the absolute figures depend
 * on the compiler's order of operations, but that the loss tracks 1/(1-R^2),
 * and that QR does not share it, are properties of the two methods. */
static void test_fit_quality_cost(void) {
    struct regress rg;
    struct qr q;
    struct regress_fit fr, fq;
    double loose = 0.0, tight = 0.0, r2_tight = 0.0;
    int k;

    for (k = 0; k < 2; k++) {
        double amp = (k == 0) ? 1.0 : 1e-4;
        double x[1], err;
        int i;

        regress_init(&rg, 1, t_store);
        qr_init(&q, 1, t_store2);
        for (i = 0; i < 2000; i++) {
            double y;
            x[0] = 100.0 + i * 0.01;
            y = 1.0 + 2.0 * x[0] + amp * (double)(((i * 7919) % 23) - 11);
            (void)regress_add(&rg, x, y);
            (void)qr_add(&q, x, y);
        }
        (void)regress_solve(&rg, t_beta, t_scratch, &fr);
        (void)qr_solve(&q, t_beta2, t_scratch, &fq);
        /* QR is the reference: on this data it agrees with the exact answer to
         * the last bit at both noise levels. */
        err = fabs(fr.sigma - fq.sigma) / fq.sigma;
        if (k == 0) loose = err; else { tight = err; r2_tight = fr.r2; }
    }
    CHECK(1.0 - r2_tight < 1e-6, "sigma cost: the tight fit really is tight");
    CHECK(loose < 1e-13, "sigma cost: a loose fit costs the normal equations nothing");
    CHECK(tight > loose * 1e3,
          "sigma cost: a tight fit costs them digits, in proportion to the fit");
    CHECK(tight < 1e-6, "sigma cost: but not so many that the number is useless");
}

/* The two defects that this file did not catch, now with tests that do.
 *
 * Both were found by review, and neither could have failed a test that existed
 * at the time: one wrote outside its block but still inside the allocation, and
 * the other only misbehaved on data whose columns had an origin. Every test
 * here used columns centred on zero and one term at a time. */
static void test_diag_probe_isolation(void) {
    /* DEFECT ONE. probe_add() wrote eleven doubles into a block declared as
     * ten, so each term's last sum landed on the next term's first slot. It was
     * invisible for two reasons: nothing was written outside the allocation, so
     * AddressSanitizer had nothing to say, and the effect was that the probes
     * went QUIET rather than reporting something wrong.
     *
     * The slot that overran is the LAST one a probe writes, the sum of
     * residual times u cubed. Only the cube probe reads it, so data with a
     * quadratic departure cannot detect the fault at all: the square probe uses
     * the first ten slots and is untouched. The first version of this test made
     * exactly that mistake and passed against the defect it was written for.
     *
     * With a cubic departure and the block one short, the probe reports NOTHING
     * AT ALL: curved_term comes back -1 at every model size. That is the whole
     * character of the defect, a check that goes quiet rather than wrong, and
     * it is what the first assertion below tests. The second pins the value,
     * which is fixed by the geometry since the data carries no noise, so a
     * partial corruption that still leaves something above the bound cannot
     * pass either. */
    {
        double t0 = 0.0;
        int k;

        for (k = 0; k < 3; k++) {
            int nvars = k + 1;
            struct diag d;
            struct diag_result r;
            double *store = xmalloc(diag_storage(nvars) * sizeof *store);
            double x[3];
            int i;

            diag_init(&d, nvars, store);
            for (i = -60; i <= 60; i++) {
                double u = i * 0.05;
                x[0] = 100.0 + u;                       /* the curved term    */
                x[1] = 7.0 + (double)((i * 31) % 13);   /* unrelated to resid */
                x[2] = -4.0 + (double)((i * 17) % 11);  /* also unrelated     */
                diag_add(&d, x, u * u * u, 24.0);       /* exactly a cubic    */
            }
            diag_result(&d, &r);
            CHECK(r.curved_term == 0 && r.curved_pow == 3,
                  "diag blocks: the cubic is found in term 0, at every model size");
            if (k == 0) t0 = r.curved_t;
            else CHECK(fabs(r.curved_t - t0) < 1e-9 * fabs(t0),
                       "diag blocks: and to the same value, whatever else is in the model");
            free(store);
        }
    }

    /* The same fault seen from the other side. Term j's overrun landed on term
     * j+1's SHIFT, the value its powers are taken about, so the term AFTER a
     * curved one is the one whose arithmetic is destroyed. Put a term at a
     * large offset behind another: with its shift intact the offset costs
     * nothing, and with the shift gone the cancellation that the shift exists
     * to prevent comes straight back. */
    {
        struct diag d;
        struct diag_result r;
        double *store = xmalloc(diag_storage(2) * sizeof *store);
        double x[2];
        int i;

        diag_init(&d, 2, store);
        for (i = -60; i <= 60; i++) {
            double u = i * 0.05;
            x[0] = 3.0 + (double)((i * 31) % 13);   /* unrelated to resid  */
            x[1] = 1.0e6 + u;                       /* curved, far from 0  */
            diag_add(&d, x, u * u * u, 24.0);
        }
        diag_result(&d, &r);
        CHECK(r.curved_term == 1 && r.curved_pow == 3,
              "diag blocks: a curved term behind another is still found");
        CHECK(fabs(r.curved_t) > DIAG_T,
              "diag blocks: and its shift survived the term in front of it");
        free(store);
    }

    /* And a guard past the end, for the ordinary kind of overrun. The one above
     * would not trip this, which is the point of having both. */
    {
        struct diag d;
        struct diag_result r;
        int nvars = 3;
        size_t need = diag_storage(nvars);
        size_t g;
        double *store = xmalloc((need + 8) * sizeof *store);
        double x[3];
        int i;

        for (g = 0; g < 8; g++) store[need + g] = -12345.5;
        diag_init(&d, nvars, store);
        for (i = 0; i < 200; i++) {
            x[0] = (double)(i % 17); x[1] = (double)(i % 7); x[2] = (double)i;
            diag_add(&d, x, (double)((i * 7919) % 23) - 11.0, 5.0 + x[0]);
        }
        diag_result(&d, &r);
        for (g = 0; g < 8; g++)
            if (store[need + g] != -12345.5) break;
        CHECK(g == 8, "diag blocks: nothing is written past diag_storage()");
        free(store);
    }
}

static void test_diag_offsets(void) {
    /* DEFECT TWO. The probes accumulated raw powers of the column, up to the
     * sixth, and recovered variances by subtracting: the naive formula
     * regress.c refuses to use, in a file that cites regress.c for refusing it.
     * The offsets it names as the reason the probe exists -- a year, a price, a
     * temperature in Kelvin -- are exactly where a raw sum of v^6 has no
     * significant digits left.
     *
     * Two failures, and a test for each. The probe went SILENT on a real curve
     * at an offset of 1e5, and it INVENTED one at 1e4 where the model was
     * right. Both directions are checked, at the offsets where they happened
     * and past them, because a check that only covers zero is what let this
     * through. */
    static const double OFFSET[] = { 0.0, 1.0e3, 1.0e4, 1.0e5, 1.0e6, 1.0e9 };
    const int NOFF = (int)(sizeof OFFSET / sizeof OFFSET[0]);
    struct diag d;
    struct diag_result r;
    double first_sq = 0.0, first_cu = 0.0;
    double x[1];
    int k, i;

    /* A real quadratic departure. Seen at every offset, and with the same
     * strength: shifting a column changes none of these correlations. */
    for (k = 0; k < NOFF; k++) {
        diag_init(&d, 1, t_diag);
        for (i = -60; i <= 60; i++) {
            double u = i * 0.05;
            x[0] = OFFSET[k] + u;
            diag_add(&d, x, u * u + ((double)((i * 7919) % 23) - 11.0) * 0.1, 24.0);
        }
        diag_result(&d, &r);
        CHECK(r.curved_term == 0 && r.curved_pow == 2,
              "diag offsets: a quadratic departure is seen, wherever the column sits");
        if (k == 0) first_sq = fabs(r.curved_t);
        else CHECK(fabs(fabs(r.curved_t) - first_sq) < 0.01 * first_sq,
                   "diag offsets: and with the same strength");
    }

    /* A real cubic departure, which needs the cube probe partialled on 1, u and
     * u^2. On [1, u] it was not offset-invariant even in exact arithmetic. */
    for (k = 0; k < NOFF; k++) {
        diag_init(&d, 1, t_diag);
        for (i = -60; i <= 60; i++) {
            double u = i * 0.05;
            x[0] = OFFSET[k] + u;
            diag_add(&d, x, u * u * u + ((double)((i * 7919) % 23) - 11.0) * 0.1, 24.0);
        }
        diag_result(&d, &r);
        CHECK(r.curved_term == 0 && r.curved_pow == 3,
              "diag offsets: a cubic departure is seen, wherever the column sits");
        if (k == 0) first_cu = fabs(r.curved_t);
        else CHECK(fabs(fabs(r.curved_t) - first_cu) < 0.01 * first_cu,
                   "diag offsets: and with the same strength");
    }

    /* The other direction, and the one that matters more: a model that is
     * RIGHT must stay quiet wherever its column sits. The raw-power version
     * reported a departure at an offset of 1e4 on data that had none. */
    for (k = 0; k < NOFF; k++) {
        diag_init(&d, 1, t_diag);
        for (i = -60; i <= 60; i++) {
            x[0] = OFFSET[k] + i * 0.05;
            diag_add(&d, x, (double)((i * 7919) % 23) - 11.0, 24.0);
        }
        diag_result(&d, &r);
        CHECK(r.curved_term == -1,
              "diag offsets: and an unstructured residual stays quiet at every offset");
    }
}

/* The memory figure quoted to a user and the memory the program asks for. They
 * were three different numbers in three files, none of them counting more than
 * the fitter, so this checks the arithmetic rather than the prose: the growth
 * per term must match what the three sizing functions actually return. */
/* The progress line on a long run. The decision is split out of the printing
 * so it can be tested without waiting a minute for one. */
static void test_progress(void) {
    const long M = 1048576;                  /* the clock is read this often */

    CHECK(process_progress_due(M, 120, 120) == 1,
          "progress: a long run past the interval prints");
    CHECK(process_progress_due(M + 1, 120, 120) == 0,
          "progress: and only on a row where the clock is read");
    CHECK(process_progress_due(M, 59, 59) == 0,
          "progress: nothing in the first minute, so a short run stays silent");
    CHECK(process_progress_due(M, 3600, 30) == 0,
          "progress: and not again until the interval has passed");
    CHECK(process_progress_due(M, 60, 60) == 1,
          "progress: the first line is due exactly at the bound");
    CHECK(process_progress_due(0, 3600, 3600) == 1,
          "progress: row 0 is a clock row, which is where a run begins");
    {   /* A fit of a million rows takes well under a second, so no ordinary
         * run can print anything. This is the property that keeps the line out
         * of every test and transcript in the project. */
        long r, printed = 0;
        for (r = 0; r < 4L * M; r++)
            if (process_progress_due(r, 0, 0)) printed++;
        CHECK(printed == 0, "progress: a run that takes no time prints nothing");
    }
}

static void test_footprint(void) {
    size_t two   = process_group_bytes(2);
    size_t three = process_group_bytes(3);
    size_t fixed;

    CHECK(process_group_bytes(0) == 0 && process_group_bytes(-1) == 0,
          "footprint: a model with no terms costs nothing to report");
    CHECK(two > 0 && three > two, "footprint: another term costs more");

    /* The figure must cover the residual-check block, which is the part every
     * earlier statement of it left out: three files quoted the fitter alone.
     * fitter_storage() is private to process.c, so the check is that the growth
     * per term exceeds what the fitter alone would explain. */
    fixed = (size_t)(diag_storage(3) - diag_storage(2)) * sizeof(double);
    CHECK(three - two > fixed,
          "footprint: a term costs more than its residual-check block alone");
    CHECK(three - two > (size_t)(3 + 1) * sizeof(double),
          "footprint: and more than its coefficients alone");
    CHECK(two > (size_t)sizeof(void *) + GROUP_MAX,
          "footprint: the record around the arrays is counted too");

    /* The scoring side is a different number, and it does NOT move with the
     * term count: the coefficient array is dimensioned at the build ceiling.
     * This was the figure being quoted for the fitting side. */
    CHECK(process_model_bytes() >= (size_t)(LOS_MAX_VARS + 2) * sizeof(double),
          "footprint: a loaded model carries the build's whole ceiling");
    CHECK(process_model_bytes() != process_group_bytes(LOS_MAX_VARS < REGRESS_MAX_VARS
                                                       ? LOS_MAX_VARS : REGRESS_MAX_VARS),
          "footprint: fitting and scoring are not the same figure");

    /* The two formulas the documents quote. qr.h said its factor was smaller
     * than the normal equations' and README.md said larger, and neither counted
     * the three per-column vectors qr.c keeps. Stated here as arithmetic so the
     * prose cannot drift from the allocation again. */
    {
        int w[] = { 1, 2, 8, 24, 35, 64 };
        size_t i;
        int ok_r = 1, ok_q = 1, ok_d = 1;
        for (i = 0; i < sizeof w / sizeof w[0]; i++) {
            size_t n = (size_t)w[i];
            if (regress_storage(w[i]) != n * n + 2 * n) ok_r = 0;
            if (qr_storage(w[i]) != n * n + 7 * n + 6) ok_q = 0;
            if (qr_storage(w[i]) != regress_storage(w[i]) + 5 * n + 6) ok_d = 0;
        }
        CHECK(ok_r, "footprint: the normal equations hold p^2 + 2p doubles");
        CHECK(ok_q, "footprint: QR holds p^2 + 7p + 6");
        CHECK(ok_d, "footprint: which is 5p + 6 MORE than the normal equations, not less");
    }
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

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] && argv[0][0]) t_argv0 = argv[0];
    test_utils();
    test_hash();
    test_scales();
    test_csv();
    test_regress();
    test_wide_fit();
    test_qr();
    test_diag();
    test_diag_probe_isolation();
    test_diag_offsets();
    test_progress();
    test_footprint();
    test_canonical();
    test_fit_quality_cost();
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
