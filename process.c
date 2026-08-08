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
#include "common.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int tables_loaded;

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
    const char *trim;

    if (tables_loaded) return 0;
    if (los_load(param_or("coef.file", DEFAULT_COEF_FILE)) != 0) return -1;

    /* The trim table is optional in three distinguishable ways, and they are
     * not the same thing: named in the config and unreadable is a failure the
     * user asked for and must hear about; set to empty means deliberately none;
     * absent from the config means the built-in path, which is only a default
     * and may simply not be there. Treating all three as fatal made a
     * coefficient table produced by -t impossible to score against. */
    trim = params_get("trim.file");
    if (trim && trim[0] == '\0') {
        debug("process: trim.file is empty; the trim point is the prediction");
    } else if (los_load_trims(trim ? trim : DEFAULT_TRIM_FILE) != 0) {
        if (trim) { los_free(); return -1; }
        debug("process: no %s; the trim point is the prediction", DEFAULT_TRIM_FILE);
    }

    tables_loaded = 1;
    return 0;
}

int process(const char *input, char *out, size_t outsz) {
    struct los_case c;
    const struct los_model *m;
    double prediction, trim;
    int pscale, tscale, w;

    if (ensure_tables() != 0) return -1;

    if (los_parse_case(input, &c) != 0) {
        debug("process: not a case (want GROUP and %d terms)", los_nvars());
        return -1;
    }
    m = los_model_get(c.group);
    if (!m) {
        debug("process: no coefficients for group %s", c.group);
        return -1;
    }

    pscale = param_int("predict.scale", DEFAULT_PREDICT_SCALE);
    tscale = param_int("trim.scale", DEFAULT_TRIM_SCALE);

    /* The trim point is built on the ROUNDED prediction, not the raw one: the
     * published figure is what the next step is entitled to use. */
    prediction = los_round(los_predict(m, &c), pscale);
    trim       = los_round(los_trim_point(m, prediction), tscale);

    w = snprintf(out, outsz, "%s prediction=%.*f trim=%.*f",
                 c.group, pscale, prediction, tscale, trim);
    if (w < 0 || (size_t)w >= outsz) return -1;
    return 0;
}

int process_train(const char *csv_path, const char *group,
                  char *out, size_t outsz, struct fit_info *info) {
    struct regress   r;
    struct los_model fitted;
    struct los_case  c;
    double beta[REGRESS_MAX_TERMS];
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp;
    size_t used;
    int    rc = -1, n, i, pinned, nvars;
    int    pool = (!group || strcmp(group, "*") == 0);
    long   rows = 0, seen = 0;

    fp = fopen(csv_path, "r");
    if (!fp) { debug("train: cannot open %s", csv_path); return -1; }

    /* The training file's header defines the polynomial: GROUP, the observed
     * value, then one column per term. Add a column to the file and the fit has
     * that term -- nothing here counts the terms for itself. */
    if (csv_next(fp, line, sizeof line) != 1) {
        debug("train: %s has no header", csv_path);
        goto cleanup;
    }
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        debug("train: %s wants GROUP, the observed value, and at least one term",
              csv_path);
        goto cleanup;
    }
    if (los_schema_set(field + 2, n - 2) != 0) goto cleanup;
    nvars = los_nvars();
    if (regress_init(&r, nvars) != 0) {
        debug("train: %s has %d terms, more than the fitter's %d",
              csv_path, nvars, REGRESS_MAX_VARS);
        goto cleanup;
    }
    /* One row at a time: read it, add it to the cross-products, forget it. The
     * file may be any size; the fitter's footprint is the same either way. */
    while ((n = csv_next(fp, line, sizeof line)) == 1) {
        double los;
        seen++;
        if (los_parse_training(line, &c, &los) != 0) {
            debug("train: %s row %ld is malformed", csv_path, seen);
            goto cleanup;
        }
        if (!pool && strcmp(c.group, group) != 0) continue;
        regress_add(&r, c.x, los);
        rows++;
    }
    if (n < 0) { debug("train: %s has an over-long line", csv_path); goto cleanup; }

    if (rows == 0) {
        debug("train: no rows for group %s in %s", pool ? "*" : group, csv_path);
        goto cleanup;
    }

    pinned = regress_solve(&r, beta);
    if (pinned < 0) {
        debug("train: nothing to fit for group %s", pool ? "*" : group);
        goto cleanup;
    }

    fitted.intercept = beta[0];
    for (i = 0; i < nvars; i++) fitted.b[i] = beta[i + 1];
    fitted.trim_addition = 0.0;      /* the trim table is a separate input */

    /* Header, newline, row: a coefficient file, not a fragment of one. */
    if (los_format_header(out, outsz) != 0) goto cleanup;
    used = strlen(out);
    if (used + 2 > outsz) goto cleanup;
    out[used++] = '\n';
    out[used] = '\0';
    if (los_format_model(pool ? "*" : group, &fitted, out + used, outsz - used) != 0) {
        debug("train: the fitted table does not fit in %lu bytes",
              (unsigned long)outsz);
        goto cleanup;
    }

    if (info) {
        info->rows   = rows;
        info->pinned = pinned;
        info->df     = rows - (nvars + 1 - pinned);
        info->r2     = regress_r2(&r, beta);
    }
    rc = 0;
cleanup:
    fclose(fp);
    return rc;
}

void process_free(void) {
    los_free();
    tables_loaded = 0;
}
