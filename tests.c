/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* tests.c -- in-place unit tests, run by `make ut` (which builds every source
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
 * (los_var_name, process_term_name, params_get, ...) -- a bare strcmp on one of
 * those is how this suite once turned a wrong return value into a SEGV. */
static int streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

#define COEF "conf/coefficients.csv"
#define TRIM "conf/trim_additions.csv"

/* The pair, as the scorer loads them. They are two calls because the trim
 * table is optional -- see test_los_trims. */
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
        CHECK(csv_next(fp, big, sizeof big) == 1, "csv_next reads");
        CHECK(strncmp(big, "GROUP,LOS,", 10) == 0, "csv_next skipped the comments to the header");
        CHECK(strchr(big, '\n') == NULL, "csv_next stripped the newline");
        /* A line that does not fit is refused, never silently split in two. */
        CHECK(csv_next(fp, big, 8) == -1, "csv_next refuses an over-long line");
        fclose(fp);
    }
}

static void test_regress(void) {
    struct regress r;
    double beta[REGRESS_MAX_TERMS];
    double x[2];
    int pinned;

    /* An exact line through exact points comes back exactly: y = 2 + 3x1 - x2. */
    regress_init(&r, 2);
    x[0] = 0; x[1] = 0; regress_add(&r, x, 2.0);
    x[0] = 1; x[1] = 0; regress_add(&r, x, 5.0);
    x[0] = 0; x[1] = 1; regress_add(&r, x, 1.0);
    x[0] = 2; x[1] = 3; regress_add(&r, x, 5.0);
    pinned = regress_solve(&r, beta);
    CHECK(pinned == 0, "regress: full rank, nothing pinned");
    CHECK(NEAR(beta[0], 2.0) && NEAR(beta[1], 3.0) && NEAR(beta[2], -1.0),
          "regress: recovers the coefficients");
    CHECK(NEAR(regress_r2(&r, beta), 1.0), "regress: R2 is 1 on an exact fit");

    /* Noise around that same line: the fit is close but no longer exact, and
     * R2 must fall below 1 rather than quietly stay there. */
    regress_init(&r, 1);
    x[0] = 0; regress_add(&r, x, 1.0);
    x[0] = 1; regress_add(&r, x, 3.1);
    x[0] = 2; regress_add(&r, x, 4.9);
    x[0] = 3; regress_add(&r, x, 7.2);
    regress_solve(&r, beta);
    CHECK(fabs(beta[1] - 2.0) < 0.1, "regress: slope through noisy points");
    CHECK(regress_r2(&r, beta) < 1.0 && regress_r2(&r, beta) > 0.99,
          "regress: R2 below 1 once the points are not collinear");

    /* A regressor that never varies carries no information, so it is pinned to
     * 0 and the rest are fitted around it rather than the fit failing. */
    regress_init(&r, 2);
    x[0] = 0; x[1] = 7; regress_add(&r, x, 1.0);
    x[0] = 1; x[1] = 7; regress_add(&r, x, 3.0);
    x[0] = 2; x[1] = 7; regress_add(&r, x, 5.0);
    pinned = regress_solve(&r, beta);
    CHECK(pinned == 1, "regress: the constant column is pinned");
    CHECK(beta[2] == 0.0, "regress: a pinned coefficient is exactly 0");
    CHECK(NEAR(beta[1], 2.0), "regress: the identified slope is still right");

    /* Two columns saying the same thing: one of them is pinned, and the fit
     * still reproduces the data instead of dividing by a zero pivot. */
    regress_init(&r, 2);
    x[0] = 1; x[1] = 2; regress_add(&r, x, 4.0);
    x[0] = 2; x[1] = 4; regress_add(&r, x, 6.0);
    x[0] = 3; x[1] = 6; regress_add(&r, x, 8.0);
    CHECK(regress_solve(&r, beta) == 1, "regress: collinear column is pinned");
    CHECK(NEAR(regress_r2(&r, beta), 1.0), "regress: the collinear fit still fits");

    /* An empty sample is not a fit, and says so. */
    regress_init(&r, 2);
    CHECK(regress_solve(&r, beta) == -1, "regress: refuses an empty sample");

    /* Fewer rows than terms is NOT refused -- it is the ordinary case here, and
     * pinning answers it. One row identifies the intercept and nothing else. */
    regress_init(&r, 2);
    x[0] = 1; x[1] = 1; regress_add(&r, x, 4.0);
    CHECK(regress_solve(&r, beta) == 2, "regress: one row identifies one term");
    CHECK(NEAR(beta[0], 4.0) && beta[1] == 0.0 && beta[2] == 0.0,
          "regress: that one term is the intercept, through the single point");

    CHECK(regress_init(&r, REGRESS_MAX_VARS + 1) == -1, "regress: refuses too many variables");
}

/* The ceiling is not decoration: fit a model as wide as the build allows and
 * check every coefficient comes back. 32 terms was a toy bound; a real table is
 * hundreds of columns wide, and this is the test that says so. */
static void test_wide_fit(void) {
    static struct regress r;                     /* ~1 MB at the default ceiling */
    static double beta[REGRESS_MAX_TERMS];
    static double x[REGRESS_MAX_VARS];
    const int p = REGRESS_MAX_VARS;
    int i, j, ok = 1;

    regress_init(&r, p);

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

    CHECK(regress_solve(&r, beta) == 0, "wide: a full-width fit is full rank");
    CHECK(NEAR(beta[0], 3.0), "wide: intercept");
    for (j = 0; j < p; j++)
        if (!NEAR(beta[j + 1], 0.25 * (j + 1))) ok = 0;
    CHECK(ok, "wide: every one of the terms comes back");
    CHECK(NEAR(regress_r2(&r, beta), 1.0), "wide: R2");

    /* And the schema will carry that many named columns. */
    {
        static char  names[LOS_MAX_VARS][LOS_NAME_MAX];
        static char *namep[LOS_MAX_VARS];
        for (i = 0; i < LOS_MAX_VARS; i++) {
            snprintf(names[i], sizeof names[i], "term_%d", i);
            namep[i] = names[i];
        }
        CHECK(los_schema_set(namep, LOS_MAX_VARS) == 0, "wide: a full-width schema");
        CHECK(los_nvars() == LOS_MAX_VARS, "wide: all of it kept");
        CHECK(streq(los_var_name(LOS_MAX_VARS - 1), names[LOS_MAX_VARS - 1]),
              "wide: the last column is named");
        los_free();
    }
}

/* Finding the files: the reason `linearr` used to work only in its own source
 * directory. g_prog must be set before the first call -- the program directory
 * is worked out once and remembered. */
static void test_resolve(void) {
    char path[RESOLVE_PATH_MAX];

    g_prog = "./linearr_ut";
    CHECK(streq(resolve_program_dir(), "."),
          "resolve: the program directory comes off argv[0]");

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

static void test_los_round(void) {
    /* Half away from zero -- NOT printf's round half to even, which would make
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

    CHECK(los_schema_set(names, 2) == 0, "schema: set");
    CHECK(los_nvars() == 2, "schema: term count");
    CHECK(streq(los_var_name(1), "stops"), "schema: names in order");
    CHECK(los_var_name(2) == NULL, "schema: nothing past the last term");
    CHECK(los_format_header(header, sizeof header) == 0 &&
          strcmp(header, "GROUP,Intercept,km,stops") == 0, "schema: writes its own header");
    CHECK(los_var_index("stops") == 1, "schema: term by name");
    CHECK(los_var_index("STOPS") == 1, "schema: name lookup ignores case");
    CHECK(los_var_index("nope") == -1, "schema: unknown name");

    CHECK(los_schema_set(names, 0) == -1, "schema: refuses no terms");
    CHECK(los_schema_set(names, LOS_MAX_VARS + 1) == -1, "schema: refuses too many terms");
    CHECK(los_schema_set(names, 3) == -1, "schema: refuses an empty column name");
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
    los_schema_set(names, 2);

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
     * it must give those coefficients back -- to every printed digit, which is
     * the strongest statement the fitter can make about itself. */
    CHECK(los_load(COEF) == 0, "train: reference table loads");
    m = los_model_get("001");
    CHECK(m != NULL, "train: reference group present");
    if (m) {
        struct los_model ref = *m;
        size_t used;
        ref.trim_addition = 0.0;
        CHECK(los_format_header(header, sizeof header) == 0, "train: format reference header");
        used = (size_t)snprintf(expect, sizeof expect, "%s\n", header);
        CHECK(los_format_model("001", &ref, expect + used, sizeof expect - used) == 0,
              "train: format reference row");

        CHECK(process_train("example/train.csv", "001", out, sizeof out, &info) == 0,
              "train: fits group 001");
        CHECK(strcmp(out, expect) == 0, "train: recovers the coefficients it was generated from");
        CHECK(NEAR(info.r2, 1.0), "train: R2 is 1 on exactly linear data");
        CHECK(info.rows == 17, "train: used only group 001's rows");
        /* 25 terms, 8 of them identified by this sample (the intercept and the
         * 7 live ones): the other 17 are 0, leaving 17 - 8 = 9 residual df. */
        CHECK(info.pinned == 17, "train: pins the terms the sample cannot identify");
        CHECK(info.df == 9, "train: reports the residual degrees of freedom");
    }

    /* The fitted output is a coefficient FILE, header and all, so it reads back. */
    CHECK(strncmp(out, "GROUP,Intercept,Cardioversion,", 30) == 0,
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
    CHECK(strcmp(out, "GROUP,Intercept,km,stops\nA,5.0000,2.5000,1.5000") == 0,
          "other schema: recovers 5 + 2.5*km + 1.5*stops");
    CHECK(info.pinned == 0 && info.df == 4, "other schema: full rank, 4 df");
    CHECK(NEAR(info.r2, 1.0), "other schema: R2");

    CHECK(process_train("example/simple-train.csv", "B", out, sizeof out, NULL) == 0,
          "other schema: the second group");
    CHECK(strcmp(out, "GROUP,Intercept,km,stops\nB,12.0000,2.5000,1.5000") == 0,
          "other schema: same slopes, its own intercept");

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
    printf("ut: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

#else
typedef int tests_translation_unit_not_empty;  /* ISO C forbids an empty TU */
#endif /* UNIT_TEST */
