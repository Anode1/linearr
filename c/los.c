/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* los.c: see los.h.
 *
 * One of this program's three heap users is here: the coefficient table, one
 * struct los_model per group, held in the hash table for the life of the run.
 * It is bounded by the number of GROUPS in the table, never by the number of
 * cases scored, and it is freed on every path by los_free(). The schema is a
 * fixed array. Cases themselves are stack objects, processed one at a time and
 * forgotten: scoring ten cases and scoring ten million cost the same memory.
 *
 * The other two are params.c's config table and, while `-t` fits every group,
 * one accumulator per group in process.c. This comment used to say "nothing
 * else allocates", which was false the day it was written: params.c was already
 * there. A count is a claim like any other. */
#include "los.h"
#include "resolve.h"
#include "csv.h"
#include "hash.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>
#include <float.h>

#define GROUP_BUCKETS 1024

static char reason[RESOLVE_PATH_MAX + 512] = "";

/* Why the last load failed. The caller used to print one sentence ("X is not
 * a coefficient table") for about ten distinct causes, with the real one
 * visible only under -d. */
const char *los_error(void) { return reason; }

static int refuse(const char *fmt, ...) LINEARR_PRINTF(1, 2);
static int refuse(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(reason, sizeof reason, fmt, ap);
    va_end(ap);
    debug("los: %s", reason);
    return -1;
}

static struct hash *models;
static char         var_name[LOS_MAX_VARS][LOS_NAME_MAX];

/* Where each column of a TRAINING row lives. Positionally the response is
 * field 1 and the terms follow it, which is what these hold unless --response
 * names a column somewhere else. Nothing in the data can say which column is
 * the response, so a file written in another order fits perfectly well and
 * answers a different question; naming it is the only way to be sure. */
static int          resp_field = 1;
static int          term_field[LOS_MAX_VARS];
static int          nvars;
static long long ngroups;
static int          have_trims;
static char         response[LOS_NAME_MAX];

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        /* unsigned char, so a byte >= 0x80 is not implementation-defined here
         * (MISRA 10.3); both operands are widened the same way either way, but
         * "works by symmetry" is not a thing to leave in a header comparison. */
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

int los_schema_set(const char *const *names, int n) {
    int i;

    if (n < 1 || n > LOS_MAX_VARS) {
        debug("los: %d terms is outside 1..%d", n, LOS_MAX_VARS);
        return -1;
    }
    for (i = 0; i < n; i++) {
        int j;
        if (names[i][0] == '\0' || strlen(names[i]) >= LOS_NAME_MAX) {
            debug("los: column %d has an empty or over-long name", i + 1);
            return -1;
        }
        /* Two columns a user cannot tell apart are refused rather than ranked.
         * Lookup by name is case-insensitive, so "A" and "a" collide: with both
         * present, `linearr G a=1` silently set column "A" and there was no way
         * to address the other one at all. */
        for (j = 0; j < i; j++)
            if (ci_equal(names[i], names[j])) {
                debug("los: columns %d and %d are both '%s' (names are matched "
                      "without regard to case)", j + 1, i + 1, names[i]);
                return -1;
            }
    }
    /* Only once every name is known good, so a rejected header leaves the
     * previous schema intact rather than half-replaced. */
    for (i = 0; i < n; i++)
        strcpy(var_name[i], names[i]);          /* length checked above */
    nvars = n;
    for (i = 0; i < n; i++) term_field[i] = i + 2;   /* the positional layout */
    resp_field = 1;
    debug("los: schema of %d terms", n);
    return 0;
}

/* The same, from a WHOLE training header, with the response named rather than
 * assumed to be column 2. header[0] is the group; the named column is the
 * response; every other column is a term, in the order it appears.
 *
 * Returns 0, -1 for the reasons los_schema_set gives, -2 if the name is not in
 * the header, and -3 if it names the group column. The caller distinguishes
 * them because "no such column" and "that is the group" want different advice.
 */
int los_schema_set_response(const char *const *header, int n,
                            const char *want) {
    const char *names[LOS_MAX_VARS];
    int field[LOS_MAX_VARS];
    int i, r = -1, k = 0;

    if (!want) return -1;
    if (n < 3 || n - 1 > LOS_MAX_VARS + 1) return -1;

    for (i = 0; i < n; i++)
        if (ci_equal(header[i], want)) { r = i; break; }
    if (r < 0) return -2;
    if (r == 0) return -3;

    for (i = 1; i < n; i++) {
        if (i == r) continue;
        names[k] = header[i];
        field[k] = i;
        k++;
    }
    if (los_schema_set(names, k) != 0) return -1;
    for (i = 0; i < k; i++) term_field[i] = field[i];
    resp_field = r;
    debug("los: response is column %d, %d terms", r + 1, k);
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

long long los_ngroups(void) { return ngroups; }
int  los_has_trims(void) { return have_trims; }
const char *los_response_name(void) { return response; }

void los_set_response_name(const char *name) {
    if (!name || strlen(name) >= sizeof response) { response[0] = '\0'; return; }
    strcpy(response, name);                 /* checked on the line above */
}

/* Every power of ten up to 1e22 is exactly representable as a double. Past
 * that they are not, which is where the fast path below stops. */
static const double pow10_exact[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};
#define POW10_MAX ((int)(sizeof pow10_exact / sizeof pow10_exact[0]) - 1)

/* A plain integer or short decimal, read without strtod, or -1 to say "not
 * mine" so the general parser gets it. Reading a training file costs more than
 * fitting it, and strtod is why: it is correctly rounded and fully general,
 * handling exponents, hexadecimal, inf, nan and the locale's decimal point,
 * and it gets called once per field to read what is usually the single
 * character '0' or '1'.
 *
 * This is exact, not approximate, which is the only version of it worth
 * having: a mantissa under 10^16 is below 2^53 and converts to double with no
 * rounding at all, and 10^frac is exact for frac <= 22. IEEE division of two
 * exactly represented values is correctly rounded by definition, and the
 * correctly rounded quotient IS what strtod returns for the same digits. So
 * where this path answers, it answers with strtod's bits. Anything else --
 * exponents, hex, over-long mantissas, leading spaces, trailing junk; it
 * declines, and nothing about those cases changes. */
static int fast_num(const char *s, double *out) {
    const char        *p = s;
    unsigned long long m = 0;
    int digits = 0, frac = 0, dot = 0, neg = 0;
    double v;

    if (*p == '-') { neg = 1; p++; } else if (*p == '+') { p++; }

    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            if (++digits > 15) return -1;       /* keep the mantissa under 2^53 */
            m = m * 10u + (unsigned)(*p - '0');
            if (dot) frac++;
        } else if (*p == '.' && !dot) {
            dot = 1;
        } else {
            break;
        }
    }
    if (digits == 0 || frac > POW10_MAX) return -1;
    while (*p == ' ') p++;
    if (*p != '\0') return -1;

    v = (double)m;
    if (frac > 0) v /= pow10_exact[frac];
    *out = neg ? -v : v;
    return 0;
}

/* strtod that refuses what atof would have accepted silently: an empty field,
 * trailing text, and the three the earlier version of this comment claimed to
 * catch and did not. strtod happily returns nan for "nan", inf for "inf" and
 * for 1e400 (with ERANGE), and reads "0x10" as 16. Each of those loaded into a
 * coefficient table without complaint, scored "prediction=nan", and exited 0.
 * A number that is not finite is not a number we can publish. */
static int parse_num(const char *s, double *out) {
    char *end;
    double v;

    if (s[0] == '\0') return -1;
    if (fast_num(s, out) == 0) return 0;
    /* Hexadecimal, which strtod accepts and this function's own comment lists
     * among the things it exists to refuse. C99 added 0x10 and the binary
     * exponent form 0X1p4 to strtod, so a field reading 0x10 loaded quietly as
     * 16. Nothing writes a CSV that way on purpose: it is a mis-export, or a
     * hash, or an identifier that landed in a numeric column, and reading it
     * as a number is how one becomes a coefficient. Decimal exponents (1e3)
     * stay: those are ordinary in exported data. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return -1;
    if ((s[0] == '-' || s[0] == '+') && s[1] == '0'
        && (s[2] == 'x' || s[2] == 'X')) return -1;
    errno = 0;
    v = strtod(s, &end);
    if (errno == ERANGE) return -1;             /* 1e400, and denormal underflow */
    while (*end == ' ') end++;
    if (*end != '\0') return -1;
    if (!isfinite(v)) return -1;                /* nan, inf, -inf */
    *out = v;
    return 0;
}

static int copy_group(char *dst, size_t dstsz, const char *src) {
    if (src[0] == '\0' || strlen(src) >= dstsz) return -1;
    strcpy(dst, src);                           /* checked on the line above */
    return 0;
}

/* A line that begins with '#' but has exactly the shape of a data row is almost
 * certainly data whose group code starts with '#', not a comment. Saying so
 * beats dropping the row and reporting one group fewer than the file has. */
static int comment_is_data_shaped(char *line, int want) {
    char *field[CSV_MAX_FIELDS];
    return csv_split(line, field, CSV_MAX_FIELDS) == want;
}

static int load_coefficients(const char *path) {
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp = fopen(path, "r");
    int    rc = -1, n, i;
    long long rows = 0;

    if (!fp) return refuse("cannot open %s", path);

    /* Leading comments precede a header, and one of them is not decoration:
     * `# response: NAME` is what the file predicts, written by -t from the
     * training header. Without it a coefficient file says group,intercept,km,
     * stops and nothing in it says the answer is in minutes. */
    response[0] = '\0';
    while ((n = csv_next(fp, line, sizeof line)) == 2) {
        const char *p = line;
        while (*p == '#' || *p == ' ' || *p == '\t') p++;
        if (strncmp(p, "response:", 9) == 0) {
            p += 9;
            while (*p == ' ' || *p == '\t') p++;
            los_set_response_name(p);
        }
    }
    if (n < 0) {
        refuse("%s has a line over %d bytes, or one holding a NUL byte",
               path, CSV_LINE_MAX - 2);
        goto cleanup;
    }
    if (n != 1) {
        refuse("%s has no header line", path);
        goto cleanup;
    }
    n = csv_split(line, field, CSV_MAX_FIELDS);
    if (n < 3) {
        /* One field usually means the file is not comma-separated at all.
         * Excel writes semicolons wherever the comma is the decimal mark, and
         * blaming the header sends the reader to inspect a header that is
         * visibly correct. */
        if (n == 1 && (strchr(field[0], ';') || strchr(field[0], '\t')))
            refuse("%s has no commas in its header, but does have %s. It looks "
                   "%s-separated; this program reads commas only", path,
                   strchr(field[0], ';') ? "semicolons" : "tabs",
                   strchr(field[0], ';') ? "semicolon" : "tab");
        else
            refuse("%s needs a header of group, intercept and at least one term; "
                   "this one has %d column%s", path, n, n == 1 ? "" : "s");
        goto cleanup;
    }
    /* A "header" whose every field is a number is not a header. It used to be
     * adopted as the schema, so the terms were named "6.4832" and "0", and the
     * error a user finally saw complained about a missing TERM. */
    {   int numeric = 1, k;
        double tmp;
        for (k = 1; k < n; k++)
            if (parse_num(field[k], &tmp) != 0) { numeric = 0; break; }
        if (numeric) {
            refuse("%s starts with a row of numbers where its header should be: "
                   "a coefficient file needs GROUP,Intercept,<term names>", path);
            goto cleanup;
        }
    }
    /* The header IS the model's column order: whatever it names, in that order,
     * is what x[] and b[] mean from here on. */
    if (los_schema_set((const char *const *)(field + 2), n - 2) != 0) {
        refuse("%s does not name %d usable terms: they must be non-empty, under "
               "%d characters, and distinct ignoring case", path, n - 2, LOS_NAME_MAX);
        goto cleanup;
    }

    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        struct los_model *m;
        char   group[GROUP_MAX];
        double v;

        if (n == 2) {
            if (comment_is_data_shaped(line, nvars + 2)) {
                refuse("%s has a line beginning with '#' that has the shape of a "
                       "data row: a group code cannot start with '#', because "
                       "the line reads as a comment", path);
                goto cleanup;
            }
            continue;
        }

        if (csv_split(line, field, CSV_MAX_FIELDS) != nvars + 2) {
            refuse("%s row %lld does not have %d columns", path, rows + 1, nvars + 2);
            goto cleanup;
        }
        if (copy_group(group, sizeof group, field[0]) != 0) {
            refuse("%s row %lld has an empty group, or one over %d characters",
                   path, rows + 1, GROUP_MAX - 1);
            goto cleanup;
        }
        /* Two rows for one group is a table its author did not mean to write.
         * Last-one-wins scored the second silently, so the file and the answer
         * disagreed and nothing said so. */
        if (hash_get(models, group) != NULL) {
            refuse("%s names group '%s' twice", path, group);
            goto cleanup;
        }

        m = xmalloc(sizeof *m);
        m->trim_addition = 0.0;
        if (parse_num(field[1], &m->intercept) != 0) {
            free(m);
            refuse("%s group %s: the intercept '%s' is not a finite number",
                   path, group, field[1]);
            goto cleanup;
        }
        for (i = 0; i < nvars; i++) {
            if (parse_num(field[i + 2], &v) != 0) {
                free(m);
                refuse("%s group %s: %s = '%s' is not a finite number",
                       path, group, var_name[i], field[i + 2]);
                goto cleanup;
            }
            m->b[i] = v;
        }
        ngroups++;
        free(hash_put(models, group, m));
        rows++;
    }
    if (n < 0) {
        refuse("%s has a line over %d bytes, or one holding a NUL byte",
               path, CSV_LINE_MAX - 2);
        goto cleanup;
    }

    debug("los: %lld groups from %s", rows, path);
    rc = 0;
cleanup:
    (void)fclose(fp);
    return rc;
}

int los_load_trims(const char *path) {
    char   line[CSV_LINE_MAX];
    char  *field[CSV_MAX_FIELDS];
    FILE  *fp;
    double first;
    int    rc = -1, n;

    if (!models)
        return refuse("there is no coefficient table to attach %s to", path);

    fp = fopen(path, "r");
    if (!fp) return refuse("cannot open %s", path);

    /* Read the first line, and only DISCARD it if it is a header. It used to be
     * eaten unconditionally, so a headerless trim table silently lost its first
     * group's trim addition: a wrong number, quietly, for one group only. */
    while ((n = csv_next(fp, line, sizeof line)) == 2)
        ;
    if (n < 0) {
        refuse("%s has a line over %d bytes, or one holding a NUL byte",
               path, CSV_LINE_MAX - 2);
        goto cleanup;
    }
    if (n != 1) {
        refuse("%s is empty", path);
        goto cleanup;
    }
    if (csv_split(line, field, CSV_MAX_FIELDS) == 2 && parse_num(field[1], &first) == 0) {
        struct los_model *m0 = hash_get(models, field[0]);
        if (m0) { m0->trim_addition = first; have_trims = 1; }  /* data, not a header */
        debug("los: %s has no header line; treating the first line as data", path);
    }

    while ((n = csv_next(fp, line, sizeof line)) > 0) {
        struct los_model *m;
        double v;

        if (n == 2) continue;
        if (csv_split(line, field, CSV_MAX_FIELDS) != 2) {
            refuse("%s wants exactly GROUP,trim_addition on every line", path);
            goto cleanup;
        }
        if (parse_num(field[1], &v) != 0) {
            refuse("%s group %s: the trim addition '%s' is not a finite number",
                   path, field[0], field[1]);
            goto cleanup;
        }
        /* A trim for a group with no coefficients is not an error: the trim
         * table may be the wider of the two. It has nothing to attach to. */
        m = hash_get(models, field[0]);
        if (m) { m->trim_addition = v; have_trims = 1; }
    }
    if (n < 0) {
        refuse("%s has a line over %d bytes, or one holding a NUL byte",
               path, CSV_LINE_MAX - 2);
        goto cleanup;
    }
    rc = 0;
cleanup:
    (void)fclose(fp);
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
    have_trims = 0;
    response[0] = '\0';
    if (!models) return;
    hash_call(models, free);                    /* the struct los_model per group */
    hash_delete(models);                        /* keys + table                   */
    models = NULL;
}

/* Both parsers split a private copy: csv_split works in place, and the caller's
 * line is const. Bounded by CSV_LINE_MAX, on the stack, allocating nothing. */
/* Why the last parse_row() refused a line. One sentence, already naming the
 * column and what was in it.
 *
 * Every failure below used to return a bare -1, and the caller printed the same
 * sentence for all of them: "expected a group, a value, and N terms". An empty
 * field, a row one column short, the word NA, a category name, a quoted number
 * and a semicolon-separated row all produced that, on a file with millions of
 * rows and no indication of which column was at fault. A reviewer read it as
 * the program crashing. It does not crash; it refused, and it refused without
 * saying what it wanted. */
static char parse_why[512];

const char *los_parse_error(void) {
    return parse_why[0] ? parse_why : "the row could not be read";
}

/* A field's name, for the message: the group, the response, or a term. Copied
 * into the caller's bounded buffer rather than returned as a pointer, so the
 * message that uses it has a length the compiler can see. */
static void field_label(char *out, size_t outsz, int idx, int xoff) {
    const char *name = "a column";
    if (idx == 0) name = "the group column";
    else if (xoff == 2 && idx == resp_field)
        name = response[0] ? response : "the value column";
    else {
        int t = -1, j;
        if (xoff == 2) { for (j = 0; j < nvars; j++) if (term_field[j] == idx) t = j; }
        else           { t = idx - xoff; }
        if (t >= 0 && t < nvars) name = var_name[t];
    }
    (void)snprintf(out, outsz, "%.*s", (int)outsz - 1, name);
}

/* Enough of a field to recognise it, with the rest elided. A message that
 * quotes a whole 400-character line is not a message. */
static void show_field(char *out, size_t outsz, const char *s) {
    size_t n = strlen(s);
    if (n == 0) { (void)snprintf(out, outsz, "empty"); return; }
    if (n <= 24) { (void)snprintf(out, outsz, "'%.24s'", s); return; }
    (void)snprintf(out, outsz, "'%.24s...'", s);
}

static int parse_row(const char *line, struct los_case *c, double *los,
                     int xoff) {
    char  buf[CSV_LINE_MAX];
    char *field[CSV_MAX_FIELDS];
    char  shown[40];
    char  lbl[LOS_NAME_MAX + 24];
    int   i, n;

    parse_why[0] = '\0';
    if (nvars < 1) {
        (void)snprintf(parse_why, sizeof parse_why,
                       "there is no schema yet: no header has been read");
        return -1;
    }
    if (strlen(line) >= sizeof buf) {
        (void)snprintf(parse_why, sizeof parse_why,
                       "the line is longer than this build reads (%d bytes)",
                       CSV_LINE_MAX - 1);
        return -1;
    }
    strcpy(buf, line);                          /* checked on the line above */

    n = csv_split(buf, field, CSV_MAX_FIELDS);
    if (n != nvars + xoff) {
        /* A wrong count is usually a separator, not a missing column, and the
         * header check already says this for the header. It said nothing for
         * the rows, so a file with a comma header and semicolon data got the
         * generic sentence. */
        if (n == 1 && (strchr(field[0], ';') || strchr(field[0], '\t')))
            (void)snprintf(parse_why, sizeof parse_why,
                           "it has no commas, but does have %s. The header is "
                           "comma-separated and this row is not; this program "
                           "reads commas only",
                           strchr(field[0], ';') ? "semicolons" : "tabs");
        else if (n < 0)
            (void)snprintf(parse_why, sizeof parse_why,
                           "it has more than the %d fields this build splits",
                           CSV_MAX_FIELDS);
        else if (strchr(line, '"') || strchr(line, '\''))
            /* A quoted field holding a comma splits into two, and a quoted
             * field holding a line break leaves the rest on the next line.
             * Both arrive here as a wrong field count, and reporting only the
             * count sends the reader to look for a missing column that is
             * not missing. */
            (void)snprintf(parse_why, sizeof parse_why,
                           "it has %d field%s and the header names %d, and the "
                           "row contains a quote. A quoted field holding a "
                           "comma splits in two here, and one holding a line "
                           "break runs onto the next line: this reads plain "
                           "comma-separated fields, with no quoting",
                           n, n == 1 ? "" : "s", nvars + xoff);
        else
            (void)snprintf(parse_why, sizeof parse_why,
                           "it has %d field%s and the header names %d: a group, "
                           "%s, and %d term%s", n, n == 1 ? "" : "s",
                           nvars + xoff, xoff == 2 ? "the value" : "no value",
                           nvars, nvars == 1 ? "" : "s");
        return -1;
    }

    /* Quoting. csv.c splits on commas and nothing else, so a quoted field
     * arrives with its quotes still on it. A quoted NUMBER then fails to parse,
     * with a message about the number; a quoted GROUP NAME did not fail at all,
     * and "A" became a group distinct from A with nothing said. A silent wrong
     * answer is the worst of the three outcomes, so quoting is refused here
     * rather than half-handled. */
    for (i = 0; i < n; i++) {
        size_t len = strlen(field[i]);
        /* First OR last, because a field that merely ENDS in a quote is the
         * same fault: A" and A became two groups, silently, which is the
         * outcome this guard was written to prevent and it only checked byte
         * zero. An interior apostrophe is left alone, since O'Brien is a name
         * and not a quoting attempt. */
        if (field[i][0] == '"' || field[i][0] == '\'' ||
            (len > 0 && field[i][len - 1] == '"')) {
            show_field(shown, sizeof shown, field[i]);
            field_label(lbl, sizeof lbl, i, xoff);
            (void)snprintf(parse_why, sizeof parse_why,
                           "%s is quoted (%s). This reads plain comma-separated "
                           "fields: quotes are not stripped, so a quoted name "
                           "would become a different name and a quoted number "
                           "would not be a number. Export without quoting",
                           lbl, shown);
            return -1;
        }
    }

    if (copy_group(c->group, sizeof c->group, field[0]) != 0) {
        show_field(shown, sizeof shown, field[0]);
        (void)snprintf(parse_why, sizeof parse_why,
                       "the group column is %s, which is empty or longer than "
                       "the %d characters a group name may have",
                       shown, GROUP_MAX - 1);
        return -1;
    }
    if (los && parse_num(field[resp_field], los) != 0) {
        show_field(shown, sizeof shown, field[resp_field]);
        field_label(lbl, sizeof lbl, resp_field, xoff);
        (void)snprintf(parse_why, sizeof parse_why,
                       "%s is %s, which is not a number. This fits numbers "
                       "only: there is no imputation for an empty field and no "
                       "encoding for a category name", lbl, shown);
        return -1;
    }

    for (i = 0; i < nvars; i++) {
        int at = (xoff == 2) ? term_field[i] : i + xoff;
        if (parse_num(field[at], &c->x[i]) != 0) {
            show_field(shown, sizeof shown, field[at]);
            field_label(lbl, sizeof lbl, at, xoff);
            (void)snprintf(parse_why, sizeof parse_why,
                           "term %s is %s, which is not a number. This fits "
                           "numbers only: there is no imputation for an empty "
                           "field and no encoding for a category name",
                           lbl, shown);
            return -1;
        }
    }
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

/* Coefficients are written to 12 significant digits.
 *
 * Not fixed decimal places: at four decimals every coefficient below 5e-5 was
 * written as 0.0000, so a fit that reported R2=1.0000 wrote a constant model to
 * disk and the round trip of fit, redirect, score produced a different model
 * from the one that was fitted.
 *
 * Not the full 17 digits either, which is what a double needs to be reproduced
 * exactly. That printed 5 as 4.999999999999999 and 1.5 as 1.4999999999999998,
 * which is the binary representation showing through and not a measurement:
 * the twelfth significant digit of a coefficient is already far below the
 * residual standard deviation of any fit that produced it. A table of exact
 * values should read as exact values.
 *
 * Twelve digits prints 5 as 5, 2.5 as 2.5, and 1.5e-06 as 1.5e-06, and the
 * value read back differs from the fitted double by at most one part in 1e12.
 * --scale governs the PREDICTION, where the rounding is part of the
 * published answer; it has no business here. */
static int append_num(char *out, size_t outsz, size_t *used, double v) {
    int w = snprintf(out + *used, outsz - *used, ",%.12g", v);
    if (w < 0 || (size_t)w >= outsz - *used) return -1;
    *used += (size_t)w;
    return 0;
}

int los_format_header(char *out, size_t outsz) {
    size_t used = 0;
    int    i;

    if (nvars < 1 || outsz == 0) return -1;
    if (append_str(out, outsz, &used, "group,intercept") != 0) return -1;
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

    if (!isfinite(v)) return v;
    for (i = 0; i < scale; i++) p *= 10.0;

    /* v*p overflowed to inf for a perfectly finite v (1.8e304 at scale 4),
     * and the infinity was then printed as a prediction. Nothing useful is lost
     * by declining to round a number with no fractional part left to round. */
    if (fabs(v) > DBL_MAX / p) return v;

    /* round() is round-half-away-from-zero and correctly rounded. The old
     * floor(v*p + 0.5) form did the rounding twice: the addition itself rounds,
     * so 0.49999999999999994, the largest double below one half, became
     * exactly 1.0 before floor() ever saw it, and rounded up. */
    return round(v * p) / p;
}
