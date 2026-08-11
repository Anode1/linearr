/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* tests.c: unit tests, run by `make ut` (-DUNIT_TEST on every source; this file
 * is empty otherwise and main.c's main() is compiled out). Add a CHECK with any
 * feature. Idempotent. Run from the project root: example/ is a relative path. */
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

/* argv[0] of the binary actually running; the literal is only the fallback. */
static const char *t_argv0 = "./linearr_ut";
#define CHECK(cond, msg) do { \
    if (cond) pass++; \
    else { fail++; printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

/* NULL is a FAIL, not a crash: use for los_var_name, process_term_name, etc. */
static int streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

/* Numbers out of a "GROUP,b0,b1,..." row. Compare values; text compares the
 * rounding, coefficients being printed at full precision. */
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

/* The pair, as the scorer loads them. Two calls: the trim table is optional. */
static int los_load_both(void) {
    if (los_load(COEF) != 0) return -1;
    return los_load_trims(TRIM);
}

/* Build "GROUP,x1,...,xp" with the listed terms set to 1. After los_load. */
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

static void test_scales(void) {
    /* The two rounding scales: 0..9, refused outside. */
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

    fp = fopen("example/train.csv", "r");
    CHECK(fp != NULL, "csv open example");
    if (fp) {
        char big[CSV_LINE_MAX];
        /* Comments come back as 2, telling one from a misread data row. */
        int r;
        CHECK((r = csv_next(fp, big, sizeof big)) == 2, "csv_next returns comments to the caller");
        CHECK(big[0] == '#', "csv_next: and hands back the comment text");
        while ((r = csv_next(fp, big, sizeof big)) == 2)
            ;
        CHECK(r == 1, "csv_next reads the data line after them");
        CHECK(strncmp(big, "group,los,", 10) == 0, "csv_next reached the header");
        CHECK(strchr(big, '\n') == NULL, "csv_next stripped the newline");
        CHECK(csv_next(fp, big, 8) == CSV_ERR_TOO_LONG,
              "csv_next refuses an over-long line");
        (void)fclose(fp);
    }

    /* Byte counts, not strlen: a NUL ends the string short of what fgets wrote. */
    {   const char *path = "test-csv-reader.csv";
        struct { const char *bytes; size_t len; int want; const char *what; } t[] = {
            { "a,1\0002,2\nb,2,2\n",         15, CSV_ERR_NUL,
              "a NUL mid-file is refused" },
            { "a,1,1\nb,2\0002,2",           14, CSV_ERR_NUL,
              "a NUL in an unterminated last line is refused, not truncated" },
            { "a,1,1\rb,2,2\r",              12, CSV_ERR_CR,
              "a CR-only file is refused rather than cut at the first record" },
            { "a,1,1\r\n",                    7, 0,
              "CRLF is an ordinary line ending" },
            { "a,1,1",                        5, 0,
              "and so is a last line with no ending at all" }
        };
        size_t k;
        for (k = 0; k < sizeof t / sizeof t[0]; k++) {
            char buf[CSV_LINE_MAX];
            int  rv = 0;
            FILE *w = fopen(path, "wb");
            if (!w) { CHECK(0, "csv reader: cannot write the fixture"); break; }
            (void)fwrite(t[k].bytes, 1, t[k].len, w);
            (void)fclose(w);
            w = fopen(path, "rb");
            /* To the end: asserted is the value the reader stops on, 0 at end
             * of file or the error code. */
            if (w) {
                while ((rv = csv_next(w, buf, sizeof buf)) > 0)
                    ;
                (void)fclose(w);
            }
            CHECK(rv == t[k].want, t[k].what);
        }
        (void)remove(path);
    }
}

/* Fitter storage; the fitter allocates nothing. Static: ~1 MB at the default
 * ceiling. Sized for the larger solver, qr_storage() wanting four further
 * vectors of nvars+1 for column scaling. Grow this if either solver's grows. */
static double t_store[(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2)
                      + 4 * (REGRESS_MAX_VARS + 1)];
static double t_scratch[(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2)];
static double t_beta[REGRESS_MAX_TERMS];
static double t_store2[(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2)];
static double t_diag[DIAG_PER_TERM * 2 + DIAG_SHARED];   /* one term */
static double t_beta2[REGRESS_MAX_TERMS];

static void test_regress(void) {
    struct regress r;
    struct regress_fit f;
    double x[2];

    /* An exact line through exact points: y = 2 + 3x1 - x2. */
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

    /* Noise around that line: close but not exact. */
    regress_init(&r, 1, t_store);
    x[0] = 0; regress_add(&r, x, 1.0);
    x[0] = 1; regress_add(&r, x, 3.1);
    x[0] = 2; regress_add(&r, x, 4.9);
    x[0] = 3; regress_add(&r, x, 7.2);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(fabs(t_beta[1] - 2.0) < 0.1, "regress: slope through noisy points");
    CHECK(f.r2 < 1.0 && f.r2 > 0.99, "regress: R2 below 1 once points are not collinear");

    /* Residual SD: error in the response's own units, which R2 cannot give.
     * SD-2 noise returns near 2, with R2 near 1. */
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

    /* Units. y = 1 + 2e-6*big + 5*flag: columns six orders of magnitude apart,
     * X'X diagonals twelve. A term's existence must not depend on the unit. */
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

    /* Offset. y carries 1e8. Uncentered sums of squares cancel to noise, scored
     * perfect by the sse<0 clamp. Centered accumulation cancels the offset. */
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
    {
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

    /* A constant column is collinear with the intercept: pinned, absorbed. */
    regress_init(&r, 2, t_store);
    x[0] = 0; x[1] = 7; regress_add(&r, x, 1.0);
    x[0] = 1; x[1] = 7; regress_add(&r, x, 3.0);
    x[0] = 2; x[1] = 7; regress_add(&r, x, 5.0);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(f.pinned == 1, "regress: the constant column is pinned");
    CHECK(t_beta[2] == 0.0, "regress: a pinned coefficient is exactly 0");
    CHECK(NEAR(t_beta[1], 2.0), "regress: the identified slope is still right");
    CHECK(NEAR(t_beta[0], 1.0), "regress: and the intercept is never pinned");

    regress_init(&r, 2, t_store);
    x[0] = 1; x[1] = 2; regress_add(&r, x, 4.0);
    x[0] = 2; x[1] = 4; regress_add(&r, x, 6.0);
    x[0] = 3; x[1] = 6; regress_add(&r, x, 8.0);
    regress_solve(&r, t_beta, t_scratch, &f);
    CHECK(f.pinned == 1, "regress: collinear column is pinned");
    CHECK(NEAR(f.r2, 1.0), "regress: the collinear fit still fits");

    /* Ill-conditioned but not singular must be reported: R2 cannot see it. */
    {
        int i;
        regress_init(&r, 2, t_store);
        for (i = 0; i < 50; i++) {
            x[0] = 1.0 + i * 0.01;
            /* Near-duplicate, not proportional: proportional would be pinned. */
            x[1] = x[0] + ((i % 2) ? 1.0e-6 : 0.0);
            regress_add(&r, x, 1.0 + 2.0 * x[0] + 3.0 * x[1]);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.condition > 1e6, "conditioning: a near-duplicate column is reported");
    }
    {
        int i;
        regress_init(&r, 2, t_store);
        for (i = 0; i < 50; i++) {
            x[0] = (double)(i % 5); x[1] = (double)(i % 3);
            regress_add(&r, x, 1.0 + 2.0 * x[0] + 3.0 * x[1]);
        }
        regress_solve(&r, t_beta, t_scratch, &f);
        CHECK(f.condition < 1e3, "conditioning: a healthy design reports a small number");
    }

    /* Non-finite input is refused at the door, not left to poison the fit. */
    regress_init(&r, 1, t_store);
    x[0] = 1.0;
    CHECK(regress_add(&r, x, 1.0) == 0, "regress: a finite observation is accepted");
    CHECK(regress_add(&r, x, 0.0 / 0.0) == -1, "regress: a NaN response is refused");
    x[0] = 1.0 / 0.0;
    CHECK(regress_add(&r, x, 1.0) == -1, "regress: an infinite term is refused");

    regress_init(&r, 2, t_store);
    CHECK(regress_solve(&r, t_beta, t_scratch, &f) == -1, "regress: refuses an empty sample");

    /* Fewer rows than terms is not refused: pinning answers it. */
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

/* A model as wide as the build allows; every coefficient must come back. */
static void test_wide_fit(void) {
    struct regress r;
    struct regress_fit f;
    static double x[REGRESS_MAX_VARS];
    const int p = REGRESS_MAX_VARS;
    int i, j, ok = 1;

    regress_init(&r, p, t_store);

    /* y = 3 + sum(0.25*j * xj): one row per term, then combinations for the df. */
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

/* g_prog must be set before the first call: the program directory is found once. */
static void test_resolve(void) {
    char path[RESOLVE_PATH_MAX];

    g_prog = t_argv0;
    {   /* argv[0] goes through realpath: a symlinked binary finds the files
         * beside the real one. Absolute is '/', a drive letter, or a UNC prefix. */
        const char *dir = resolve_program_dir();
        int absolute = dir != NULL &&
                       (dir[0] == '/' || dir[0] == '\\' ||
                        (dir[0] != '\0' && dir[1] == ':'));
        CHECK(absolute, "resolve: the program directory is a real absolute path");
    }

    CHECK(resolve_file(COEF, path, sizeof path) == 0, "resolve: finds a file in the cwd");
    CHECK(strcmp(path, COEF) == 0, "resolve: and prefers the cwd copy, unchanged");

    CHECK(resolve_file("/no/such/absolute", path, sizeof path) == -1,
          "resolve: an absolute path that is not there fails");
    CHECK(resolve_file("no-such-file-anywhere", path, sizeof path) == -1,
          "resolve: a name that is nowhere fails");
    CHECK(strstr(path, "looked in") != NULL, "resolve: says where it looked");
}

/* Naming the terms instead of counting commas. */
static void test_named_case(void) {
    char out[LINEARR_MAX_OUTPUT], row[LINEARR_MAX_INPUT];
    char *a[2];
    int on[2];

    /* Named outright: under test is the scoring, not the search for a table. */
    process_use_coef(COEF);
    process_use_trim(TRIM);

    a[0] = (char *)"Cardioversion=1";
    a[1] = (char *)"icu_indicator=1";
    CHECK(process_named("001", a, 2, out, sizeof out) == 0, "named: scores");

    CHECK(los_load_both() == 0, "named: schema for the row form");
    on[0] = 0; on[1] = 16;
    make_case(row, sizeof row, "001", on, 2);
    los_free();
    {
        char rowout[LINEARR_MAX_OUTPUT];
        CHECK(process(row, rowout, sizeof rowout) == 0, "named: the row form scores");
        CHECK(strcmp(out, rowout) == 0, "named: both forms give the same answer");
    }

    /* Terms nobody mentioned are 0: at 256 columns, most of them. */
    a[0] = (char *)"icu_indicator=0";
    CHECK(process_named("001", a, 1, out, sizeof out) == 0, "named: one term");
    CHECK(strstr(out, "prediction=6.4832") != NULL, "named: unmentioned terms are 0");

    a[0] = (char *)"ICU_INDICATOR=1";
    CHECK(process_named("001", a, 1, out, sizeof out) == 0, "named: case-insensitive");

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

/* QR never forms X'X, so it keeps the digits squaring the condition number
 * throws away. Well-conditioned, the two agree; ill-conditioned, only QR. */
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
    CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == 0, "qr: solves");
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
        qr_solve(&q, t_beta, t_scratch, &fq);
        eqr = fabs(t_beta[1] - 2.0);
        regress_solve(&r, t_beta2, t_scratch, &fr);
        eq = fabs(t_beta2[1] - 2.0);
        CHECK(eqr < eq / 100.0, "qr: at least two orders more accurate here");
        CHECK(eqr < 1e-7, "qr: and close to the truth in absolute terms");
        /* cond(X) rather than cond(X'X), so it is the smaller number. */
        CHECK(fq.condition < fr.condition, "qr: reports the unsquared conditioning");
    }

    /* Units, for QR: each R diagonal is judged against its own column's size. */
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
            CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == 0, "qr units: solves");
            CHECK(fq.pinned == 0, "qr units: nothing deleted for being small");
            CHECK(fabs(t_beta[1] - 2.0) < 1e-6, "qr units: the ordinary slope");
            CHECK(fabs(t_beta[2] * unit - 3.0) < 1e-6, "qr units: the small one");
        }
    }

    /* With a column dropped, the rotation's residual belongs to a model never
     * returned. Reported is the returned model's; the reduced fit must match. */
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
        (void)qr_solve(&q, t_beta, t_scratch, &fq);
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

    /* A dropped column in every position. Back substitution alone is the answer
     * only when no kept column sits right of a dropped one: re-triangularised. */
    {
        int where;
        double x3[3];                    /* this block fits three terms */
        for (where = 0; where < 3; where++) {
            double want0;
            qr_init(&q, 3, t_store);
            for (i = 0; i < 20; i++) {
                double xi = (double)i;
                double y  = 3.0 + 2.0 * xi + (double)((i % 3) - 1) * 0.9;
                if (where == 0) { x3[0] = 7.0; x3[1] = xi; x3[2] = (double)((i * 7) % 5); }
                if (where == 1) { x3[0] = xi; x3[1] = 7.0; x3[2] = (double)((i * 7) % 5); }
                if (where == 2) { x3[0] = xi; x3[1] = (double)((i * 7) % 5); x3[2] = 7.0; }
                (void)qr_add(&q, x3, y);
            }
            CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == 0,
                  "qr pivot: a rank-deficient design solves");
            CHECK(fq.pinned == 1, "qr pivot: exactly one column is dropped");
            /* The same fit without the dead column at all. */
            {   struct qr q2;
                double x2[2];
                qr_init(&q2, 2, t_store2);
                for (i = 0; i < 20; i++) {
                    double xi = (double)i;
                    x2[0] = xi; x2[1] = (double)((i * 7) % 5);
                    (void)qr_add(&q2, x2, 3.0 + 2.0 * xi + (double)((i % 3) - 1) * 0.9);
                }
                (void)qr_solve(&q2, t_beta2, t_scratch, &fr);
                want0 = t_beta2[0];
            }
            CHECK(fabs(t_beta[0] - want0) < 1e-9 * (fabs(want0) + 1.0),
                  "qr pivot: the intercept is the reduced model's, at any position");
            CHECK(fabs(fq.rss - fr.rss) < 1e-9 * fr.rss,
                  "qr pivot: and so is the residual");
        }
    }

    /* Collinear, not constant, far from zero: what the 2-norm scale is for. */
    {
        qr_init(&q, 2, t_store);
        for (i = 0; i < 30; i++) {
            x[0] = 1.0e6 + (double)i;              /* offset, but informative */
            x[1] = 5.0e3 + (double)(i % 7) * 0.5;  /* offset, and independent */
            /* Noiseless on purpose: with noise the slope moves by a few per
             * cent on thirty rows, and a six-digit test then tests the noise. */
            qr_add(&q, x, 1.0 + 2.0 * x[0] - 4.0 * x[1]);
        }
        (void)qr_solve(&q, t_beta, t_scratch, &fq);
        CHECK(fq.pinned == 0, "qr scale: an offset column is not mistaken for collinear");
        CHECK(fabs(t_beta[1] - 2.0) < 1e-6 && fabs(t_beta[2] + 4.0) < 1e-6,
              "qr scale: and both slopes come back");
        qr_init(&q, 2, t_store);
        for (i = 0; i < 30; i++) {
            x[0] = 1.0e6 + (double)i;
            x[1] = 3.0 + (x[0] - 1.0e6) * 0.5;     /* exactly a function of x0 */
            qr_add(&q, x, 1.0 + 2.0 * x[0]);
        }
        (void)qr_solve(&q, t_beta, t_scratch, &fq);
        CHECK(fq.pinned == 1, "qr scale: a dependent column at the same offset is dropped");
        CHECK(fq.term[1] == REGRESS_COLLINEAR,
              "qr scale: and reported collinear rather than constant");
        /* Where it stops working, under test so it cannot change quietly. At
         * offset 1e9 an uncentred factorisation cannot separate a dependent from
         * an independent column: cond= says so, not the rank (QR_RANK_EPS, qr.c). */
        qr_init(&q, 2, t_store);
        for (i = 0; i < 30; i++) {
            x[0] = 1.0e9 + (double)i;
            x[1] = 3.0 + (x[0] - 1.0e9) * 0.5;
            qr_add(&q, x, 1.0 + 2.0 * x[0]);
        }
        (void)qr_solve(&q, t_beta, t_scratch, &fq);
        CHECK(fq.condition > 1e7,
              "qr scale: at 1e9 the rank test misses, and cond= reports it instead");

        /* A column so large that squaring it overflows: a plain sum of squares
         * goes to +inf past about 1.3e154, making |R_ii|/inf = 0. Scaling a
         * column scales its coefficient only, so the fit is magnitude-invariant. */
        {   double huge[] = { 1.0, 1.0e153, 1.0e154, 1.0e200 };
            size_t h;
            for (h = 0; h < sizeof huge / sizeof huge[0]; h++) {
                qr_init(&q, 2, t_store);
                for (i = 1; i <= 6; i++) {
                    x[0] = (double)i * huge[h];
                    x[1] = (double)(i % 3) + 1.0;
                    qr_add(&q, x, 2.0 * (double)i + x[1]);
                }
                (void)qr_solve(&q, t_beta, t_scratch, &fq);
                CHECK(fq.pinned == 0,
                      "qr scale: a column that overflows when squared is not deleted");
                CHECK(fq.df == 3, "qr scale: and the rank is the same at every magnitude");
                CHECK(fabs(t_beta[2] - 1.0) < 1e-9,
                      "qr scale: the unscaled column keeps its coefficient");
            }
        }

        /* A response so large that squaring it overflows. colscale protects the
         * columns, not cyy or the rotated-out residual: both solvers refuse -3. */
        {   struct regress rn;
            struct regress_fit fn;
            qr_init(&q, 1, t_store);
            regress_init(&rn, 1, t_store2);
            for (i = 0; i < 20; i++) {
                x[0] = (double)i;
                qr_add(&q, x, (i % 2) ? 1.0e160 : -1.0e160);
                regress_add(&rn, x, (i % 2) ? 1.0e160 : -1.0e160);
            }
            CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == -3,
                  "qr: an overflowing response is refused, not published");
            CHECK(regress_solve(&rn, t_beta, t_scratch, &fn) == -3,
                  "regress: an overflowing response is refused, not published");
        }
    }

    qr_init(&q, 2, t_store);
    x[0] = 0; x[1] = 7; qr_add(&q, x, 1.0);
    x[0] = 1; x[1] = 7; qr_add(&q, x, 3.0);
    x[0] = 2; x[1] = 7; qr_add(&q, x, 5.0);
    CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == 0, "qr: solves a rank-deficient design");
    CHECK(fq.pinned == 1, "qr: the constant column is pinned");
    CHECK(NEAR(t_beta[1], 2.0), "qr: the identified slope is right");
    CHECK(fq.term[1] == REGRESS_CONSTANT,
          "qr: a column with no spread is reported constant, not collinear");
    CHECK(NEAR(t_beta[0], 1.0), "qr: and the intercept is never dropped");

    /* Non-finite input is refused at the door, as in regress.c. */
    qr_init(&q, 1, t_store);
    x[0] = 1.0;
    CHECK(qr_add(&q, x, 0.0 / 0.0) == -1, "qr: a NaN response is refused");
    CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == -1, "qr: an empty sample is not a fit");
    CHECK(qr_init(&q, REGRESS_MAX_VARS + 1, t_store) == -1, "qr: refuses too many terms");

    /* The residual comes from the rotation, never a bound. The caller prints '<'
     * or '=' from this flag, so the solver must assign it, initialised or not. */
    qr_init(&q, 1, t_store);
    x[0] = 0.0; qr_add(&q, x, 1.0);
    x[0] = 1.0; qr_add(&q, x, 3.0);
    x[0] = 2.0; qr_add(&q, x, 5.0);
    x[0] = 3.0; qr_add(&q, x, 7.5);
    fq.sigma_is_bound = 1;                  /* poisoned: the solver must clear it */
    CHECK(qr_solve(&q, t_beta, t_scratch, &fq) == 0, "qr: solves for the bound flag");
    CHECK(fq.sigma_is_bound == 0, "qr: reports a residual value, never a bound");

    /* The shared static buffer must hold the wider solver at the term ceiling;
     * process.c asserts this at compile time. */
    CHECK(qr_storage(REGRESS_MAX_VARS) <= (size_t)(sizeof t_store / sizeof t_store[0]),
          "qr: the shared fit buffer holds the QR at the term ceiling");
    CHECK(qr_storage(REGRESS_MAX_VARS) > (size_t)(REGRESS_MAX_VARS + 1) * (REGRESS_MAX_VARS + 2),
          "qr: and needs more than R alone, which is what the old sizing assumed");
}

/* Residual checks: structure in the residuals says the line was wrong. */
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
    diag_result(&d, DIAG_T, &r);
    CHECK(r.curved_term == 0, "diag: names the term whose shape is wrong");
    CHECK(fabs(r.curved_t) > 5.0, "diag: and the evidence is strong");

    diag_init(&d, 1, dstore);
    for (i = 1; i <= 60; i++) {
        x[0] = (double)i;
        diag_add(&d, x, ((i % 2) ? 1.0 : -1.0) * (double)i, (double)i * 2.0);
    }
    diag_result(&d, DIAG_T, &r);
    CHECK(fabs(r.spread_t) > 3.5, "diag: sees the error growing with the prediction");

    diag_init(&d, 1, dstore);
    for (i = 0; i < 200; i++) {
        x[0] = (double)(i % 17);
        diag_add(&d, x, ((i * 7919) % 23) - 11.0, 5.0 + x[0]);
    }
    diag_result(&d, DIAG_T, &r);
    CHECK(r.curved_term == -1, "diag: quiet on an unstructured residual");
    CHECK(r.spread_t == 0.0, "diag: and quiet about its spread");

    /* Location. The same quadratic with x moved off zero. Correlation against
     * raw x^2 fades with the offset, the residual being already orthogonal to x;
     * partialling x^2 on [1, x] about the column's own centre is invariant.
     * Noise on purpose: exact, r is 1 and the test compares rounding in 1-r^2. */
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
            diag_result(&d, DIAG_T, &r);
            CHECK(r.curved_term == 0, "diag location: the curve is seen at every offset");
            if (k == 0) first = fabs(r.curved_t);
            else CHECK(fabs(fabs(r.curved_t) - first) < 0.01 * first,
                       "diag location: and with the same strength");
        }
    }

    /* An exact relation prints the cap, not a t of 1e8: past r = 1 to within
     * 1e-12 the divisor has no digits left. */
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
            diag_result(&d, DIAG_T, &r);
            CHECK(r.curved_term == 0 && fabs(r.curved_t) == 9999.0,
                  "diag: an exact curve reports the cap, at any offset");
        }
    }

    /* A cubic is odd in x and the square probe even, so only the cube sees it. */
    {
        diag_init(&d, 1, dstore);
        for (i = -60; i <= 60; i++) {
            x[0] = i * 0.05;
            diag_add(&d, x, x[0] * x[0] * x[0], 5.0);
        }
        diag_result(&d, DIAG_T, &r);
        CHECK(r.curved_term == 0, "diag: a cubic departure is seen");
        CHECK(r.curved_pow == 3, "diag: and it is named as a cube, not a square");
    }

    diag_init(&d, 1, dstore);
    for (i = 0; i < 5; i++) { x[0] = (double)i; diag_add(&d, x, (double)(i*i), 1.0); }
    diag_result(&d, DIAG_T, &r);
    CHECK(r.curved_term == -1, "diag: says nothing from five rows");
}

/* ---------------------------------------------------------------------------
 * The certified sets; see canon.h. Tolerances below are relative and measured,
 * not chosen: about ten times today's error, loose enough for a different order
 * of operations, tight enough that a lost digit fails. Per solver: they differ. */
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

/* Fit one set with one solver: worst relative coefficient error, sigma, R2. */
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

    /* Norris. The easy set: both solvers at the double's own limit. */
    canon_fit(&canon_norris, 0, &rel, &sigma, &r2);
    CHECK(rel < 1e-12, "Norris: normal equations match the certified coefficients");
    /* 1e-9, not the coefficients' 1e-12: RSS is formed as Syy - beta'Sxy, and
     * Norris fits so well the two agree to five digits. Eleven survive. */
    CHECK(fabs(sigma - canon_norris.sigma) < 1e-9 * canon_norris.sigma,
          "Norris: and the certified residual SD");
    CHECK(fabs(r2 - canon_norris.r2) < 1e-12, "Norris: and the certified R2");
    canon_fit(&canon_norris, 1, &rel, &sigma, &r2);
    CHECK(rel < 1e-11, "Norris: QR matches the certified coefficients");
    CHECK(fabs(sigma - canon_norris.sigma) < 1e-13 * canon_norris.sigma,
          "Norris: QR and the certified residual SD, to the double's own limit");

    /* Longley. Published because packages of the day returned two correct digits
     * on it; both solvers here return eleven. The normal equations manage it only
     * through regress.c's centered co-moments, caught here and nowhere else. */
    canon_fit(&canon_longley, 0, &rel, &sigma, &r2);
    CHECK(rel < 1e-9, "Longley: normal equations match the certified coefficients");
    CHECK(fabs(sigma - canon_longley.sigma) < 1e-9 * canon_longley.sigma,
          "Longley: and the certified residual SD");
    CHECK(fabs(r2 - canon_longley.r2) < 1e-11, "Longley: and the certified R2");
    canon_fit(&canon_longley, 1, &rel, &sigma, &r2);
    CHECK(rel < 1e-10, "Longley: QR matches the certified coefficients");
    CHECK(fabs(sigma - canon_longley.sigma) < 1e-10 * canon_longley.sigma,
          "Longley: QR and the certified residual SD");

    /* Wampler1. An exact quintic: certified coefficients all 1, certified
     * residual 0. Nothing absorbs a solver's error.
     *
     *                 worst coefficient error     reported residual SD
     *   normal eqns          4.4e-9                     2.3e-2
     *   QR                   4.4e-10                    6.7e-11
     *
     * Certified residual SD is 0; 0.023 is what squaring x^5 lost, QR's 7e-11. */
    canon_fit(&canon_wampler1, 0, &rel, &sigma, &r2);
    /* 1e-4, loose on purpose. x^5 times x^5 reaches 1e16, the edge of a double
     * for the normal equations, and how far over depends on the compiler:
     * 4.4e-9 on x86-64 with gcc, past 1e-7 on arm64 with clang. */
    CHECK(rel < 1e-4, "Wampler1: normal equations roughly recover the quintic");
    CHECK(sigma > 1e-4 && sigma < 1.0,
          "Wampler1: and report a residual SD that is visibly not zero");
    canon_fit(&canon_wampler1, 1, &rel, &sigma, &r2);
    CHECK(rel < 1e-8, "Wampler1: QR recovers the quintic");
    CHECK(sigma < 1e-6, "Wampler1: and its residual SD is near the certified zero");
    {   /* The ordering itself under test: QR becoming the worse must fail. */
        double rq, rn, sq, sn, junk;
        canon_fit(&canon_wampler1, 0, &rn, &sn, &junk);
        canon_fit(&canon_wampler1, 1, &rq, &sq, &junk);
        CHECK(rq < rn, "Wampler1: QR is the more accurate of the two");
        CHECK(sq < sn * 1e-6, "Wampler1: by orders of magnitude on the residual");
    }
}

/* The normal equations' residual SD loses digits in proportion to the fit: RSS
 * is Syy - beta'Sxy, and those agree to more places as R^2 nears 1. Measured:
 *
 *      1 - R^2      relative error in the reported residual SD
 *      2.5e-01                 8e-16
 *      3.3e-03                 4e-13
 *      3.3e-05                 2e-11
 *      3.3e-07                 1e-09
 *      3.3e-09                 2e-07
 *      3.3e-11                 8e-06
 *
 * A hundredfold better fit costs a hundredfold worse residual SD. QR carries the
 * residual through the rotation and does not pay it. Tested as a ratio: absolute
 * figures depend on the compiler's order of operations, the 1/(1-R^2) does not. */
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
        /* QR is the reference: exact to the last bit at both noise levels. */
        err = fabs(fr.sigma - fq.sigma) / fq.sigma;
        if (k == 0) loose = err; else { tight = err; r2_tight = fr.r2; }
    }
    CHECK(1.0 - r2_tight < 1e-6, "sigma cost: the tight fit really is tight");
    CHECK(loose < 1e-13, "sigma cost: a loose fit costs the normal equations nothing");
    CHECK(tight > loose * 1e3,
          "sigma cost: a tight fit costs them digits, in proportion to the fit");
    CHECK(tight < 1e-6, "sigma cost: but not so many that the number is useless");
}

/* Each probe block stays in its own slots; an overrun of one lands inside the
 * allocation, where a guard past the end cannot see it. Hence both checks. */
static void test_diag_probe_isolation(void) {
    /* The slot at risk is the last a probe writes, sum of residual times u
     * cubed, read only by the cube probe: hence a cubic departure, a quadratic
     * one cannot detect the fault. One slot short, curved_term returns -1. */
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
            diag_result(&d, DIAG_T, &r);
            CHECK(r.curved_term == 0 && r.curved_pow == 3,
                  "diag blocks: the cubic is found in term 0, at every model size");
            /* The correlation cannot depend on the other terms; t does, through
             * df = n - p - 2. Dividing it out leaves r/sqrt(1-r^2). */
            {   double df = 121.0 - (double)nvars - 2.0;
                double z  = r.curved_t / sqrt(df);
                if (k == 0) t0 = z;
                else CHECK(fabs(z - t0) < 1e-9 * fabs(t0),
                           "diag blocks: and the same correlation, whatever else is in the model");
            }
            free(store);
        }
    }

    /* From the other side: term j's block runs into term j+1's shift, the centre
     * its powers are taken about, so the term after a curved one is at risk. */
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
        diag_result(&d, DIAG_T, &r);
        CHECK(r.curved_term == 1 && r.curved_pow == 3,
              "diag blocks: a curved term behind another is still found");
        CHECK(fabs(r.curved_t) > DIAG_T,
              "diag blocks: and its shift survived the term in front of it");
        free(store);
    }

    /* A guard past the end, for the ordinary overrun the block above misses. */
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
        diag_result(&d, DIAG_T, &r);
        for (g = 0; g < 8; g++)
            if (store[need + g] != -12345.5) break;
        CHECK(g == 8, "diag blocks: nothing is written past diag_storage()");
        free(store);
    }
}

/* Powers are taken about a centre from the completed fit, not the first row. */
static void test_diag_center(void) {
    struct diag d;
    struct diag_result r;
    double x[1], ctr[1], t_first = 0.0, t_last = 0.0;
    int pass_no, i;

    for (pass_no = 0; pass_no < 2; pass_no++) {
        diag_init(&d, 1, t_diag);
        ctr[0] = 0.0;                     /* the fit's mean of x, near zero */
        diag_center(&d, ctr, 0.0);
        if (pass_no == 0) { x[0] = 30.0; diag_add(&d, x, 900.0, 0.0); }
        for (i = -100; i <= 100; i++) {
            double u = i * 0.05;
            x[0] = u;
            diag_add(&d, x, u * u, 0.0);
        }
        if (pass_no == 1) { x[0] = 30.0; diag_add(&d, x, 900.0, 0.0); }
        diag_result(&d, DIAG_T, &r);
        if (pass_no == 0) t_first = r.curved_t; else t_last = r.curved_t;
    }
    CHECK(t_first != 0.0, "diag centre: the curve is found either way round");
    CHECK(fabs(t_first - t_last) < 1e-9 * fabs(t_first),
          "diag centre: and to the same value, whichever row arrives first");
}

/* A spread growing symmetrically: |r| against the prediction correlates zero. */
static void test_diag_spread(void) {
    struct diag d;
    struct diag_result r;
    double x[1];
    int i;

    /* Symmetric: the residual's size grows with |fitted|, its sign does not. */
    diag_init(&d, 1, t_diag);
    for (i = -150; i <= 150; i++) {
        double u = i * 0.02;
        double e = ((double)((i * 7919) % 23) - 11.0) / 11.0 * (0.2 + 3.0 * fabs(u));
        x[0] = u;
        diag_add(&d, x, e, u);
    }
    diag_result(&d, DIAG_T, &r);
    CHECK(r.spread_t > DIAG_T, "diag spread: a symmetric change in spread is seen");

    /* Constant spread: noise unstructured in u, so a generator, not i mod n. */
    {   unsigned long seed = 12345UL;
        diag_init(&d, 1, t_diag);
        for (i = -150; i <= 150; i++) {
            double u = i * 0.02, e;
            seed = seed * 6364136223846793005UL + 1442695040888963407UL;
            e = (double)((seed >> 33) % 2001) / 1000.0 - 1.0;
            x[0] = u;
            diag_add(&d, x, e, u);
        }
    }
    diag_result(&d, DIAG_T, &r);
    CHECK(r.spread_t == 0.0, "diag spread: and a constant one is not");

    /* DIAG_T_CAP is a sentinel for "exact", not a measurement, and diag_result
     * picks the worst by magnitude: an unclamped F would outrank an exact
     * quadratic. Four decades of jitter. */
    {   double jitter[4];
        int k;
        jitter[0] = 1e-3; jitter[1] = 1e-5; jitter[2] = 1e-7; jitter[3] = 1e-9;
        for (k = 0; k < 4; k++) {
            unsigned long seed = 99UL;
            diag_init(&d, 1, t_diag);
            for (i = 1; i <= 200; i++) {
                double u = 1.0 + i * 0.37, e;
                seed = seed * 6364136223846793005UL + 1442695040888963407UL;
                e = 0.01 * u * (1.0 + jitter[k] * (double)((seed >> 33) % 1000));
                x[0] = u;
                diag_add(&d, x, (i % 2) ? e : -e, u);
            }
            diag_result(&d, DIAG_T, &r);
            CHECK(fabs(r.spread_t) <= DIAG_T_CAP,
                  "diag spread: the reported figure never exceeds the cap");
        }
    }
}

static void test_diag_offsets(void) {
    /* No raw powers up to the sixth recovered by subtraction: at the offsets the
     * probe exists for -- a year, a price, a Kelvin temperature -- a raw sum of
     * v^6 has no digits left. Both failures checked: silence, and invention. */
    static const double OFFSET[] = { 0.0, 1.0e3, 1.0e4, 1.0e5, 1.0e6, 1.0e9 };
    const int NOFF = (int)(sizeof OFFSET / sizeof OFFSET[0]);
    struct diag d;
    struct diag_result r;
    double first_sq = 0.0, first_cu = 0.0;
    double x[1];
    int k, i;

    /* A quadratic departure. Shifting a column changes no correlation: same t. */
    for (k = 0; k < NOFF; k++) {
        diag_init(&d, 1, t_diag);
        for (i = -60; i <= 60; i++) {
            double u = i * 0.05;
            x[0] = OFFSET[k] + u;
            diag_add(&d, x, u * u + ((double)((i * 7919) % 23) - 11.0) * 0.1, 24.0);
        }
        diag_result(&d, DIAG_T, &r);
        CHECK(r.curved_term == 0 && r.curved_pow == 2,
              "diag offsets: a quadratic departure is seen, wherever the column sits");
        if (k == 0) first_sq = fabs(r.curved_t);
        else CHECK(fabs(fabs(r.curved_t) - first_sq) < 0.01 * first_sq,
                   "diag offsets: and with the same strength");
    }

    /* The cube probe is partialled on 1, u, u^2; on [1, u] it was not invariant. */
    for (k = 0; k < NOFF; k++) {
        diag_init(&d, 1, t_diag);
        for (i = -60; i <= 60; i++) {
            double u = i * 0.05;
            x[0] = OFFSET[k] + u;
            diag_add(&d, x, u * u * u + ((double)((i * 7919) % 23) - 11.0) * 0.1, 24.0);
        }
        diag_result(&d, DIAG_T, &r);
        CHECK(r.curved_term == 0 && r.curved_pow == 3,
              "diag offsets: a cubic departure is seen, wherever the column sits");
        if (k == 0) first_cu = fabs(r.curved_t);
        else CHECK(fabs(fabs(r.curved_t) - first_cu) < 0.01 * first_cu,
                   "diag offsets: and with the same strength");
    }

    for (k = 0; k < NOFF; k++) {
        diag_init(&d, 1, t_diag);
        for (i = -60; i <= 60; i++) {
            x[0] = OFFSET[k] + i * 0.05;
            diag_add(&d, x, (double)((i * 7919) % 23) - 11.0, 24.0);
        }
        diag_result(&d, DIAG_T, &r);
        CHECK(r.curved_term == -1,
              "diag offsets: and an unstructured residual stays quiet at every offset");
    }
}

/* The progress line; the decision is split from the printing to be testable. */
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
    {   /* A million rows takes well under a second, so no ordinary run prints. */
        long r, printed = 0;
        for (r = 0; r < 4L * M; r++)
            if (process_progress_due(r, 0, 0)) printed++;
        CHECK(printed == 0, "progress: a run that takes no time prints nothing");
    }
}

/* The quoted memory figure: growth per term must match the sizing functions. */
static void test_footprint(void) {
    size_t two   = process_group_bytes(2);
    size_t three = process_group_bytes(3);
    size_t fixed;

    CHECK(process_group_bytes(0) == 0 && process_group_bytes(-1) == 0,
          "footprint: a model with no terms costs nothing to report");
    CHECK(two > 0 && three > two, "footprint: another term costs more");

    /* fitter_storage() is private, so the check is growth beyond the fitter. */
    fixed = (size_t)(diag_storage(3) - diag_storage(2)) * sizeof(double);
    CHECK(three - two > fixed,
          "footprint: a term costs more than its residual-check block alone");
    CHECK(three - two > (size_t)(3 + 1) * sizeof(double),
          "footprint: and more than its coefficients alone");
    CHECK(two > (size_t)sizeof(void *) + GROUP_MAX,
          "footprint: the record around the arrays is counted too");

    /* Scoring is a different figure: its array is dimensioned at the ceiling. */
    CHECK(process_model_bytes() >= (size_t)(LOS_MAX_VARS + 2) * sizeof(double),
          "footprint: a loaded model carries the build's whole ceiling");
    CHECK(process_model_bytes() != process_group_bytes(LOS_MAX_VARS < REGRESS_MAX_VARS
                                                       ? LOS_MAX_VARS : REGRESS_MAX_VARS),
          "footprint: fitting and scoring are not the same figure");

    /* The documents' formulas as arithmetic, so prose cannot drift from the
     * allocation. QR is the larger, per-column vectors counted. */
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
    /* Half away from zero, not printf's half to even, which gives 2 and -2. */
    CHECK(NEAR(los_round(2.5, 0), 3.0), "round: half up");
    CHECK(NEAR(los_round(-2.5, 0), -3.0), "round: half away from zero when negative");
    CHECK(NEAR(los_round(15.63514, 4), 15.6351), "round: to four digits");
    CHECK(NEAR(los_round(46.5139, 1), 46.5), "round: to one digit");
}

static void test_los_schema(void) {
    char *names[3];
    char  header[LINEARR_MAX_OUTPUT];

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
    CHECK(los_nvars() == 2 && streq(los_var_name(0), "km"),
          "schema: a rejected header changes nothing");
    los_free();
}

static void test_los_tables(void) {
    const struct los_model *m;

    CHECK(los_load_both() == 0, "los: tables load");
    /* The header defines the model: 24 terms here, 2 in simple-train.csv. */
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

    /* Coefficients alone are a working model: addition 0, trim point equal to
     * the prediction. A table produced by -t has no trim file. */
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
    char line[LINEARR_MAX_INPUT];
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
    char line[LINEARR_MAX_INPUT], out[LINEARR_MAX_OUTPUT];
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
    char out[LINEARR_MAX_OUTPUT], header[LINEARR_MAX_OUTPUT];
    struct fit_info info;
    const struct los_model *m;

    /* train.csv came from the example coefficients: fitting gives them back. */
    CHECK(los_load(COEF) == 0, "train: reference table loads");
    m = los_model_get("001");
    CHECK(m != NULL, "train: reference group present");
    if (m) {
        struct los_model ref = *m;
        double got[LOS_MAX_VARS + 2];
        int k, nf, same = 1;
        ref.trim_addition = 0.0;
        CHECK(los_format_header(header, sizeof header) == 0, "train: format reference header");

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
    char out[LINEARR_MAX_OUTPUT];
    struct fit_info info;

    /* Two columns nobody wrote any code for, fitted by the binary that does the
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
    test_diag_center();
    test_diag_spread();
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
