/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* main.c, the CLI front end: parse options, read the input (arguments, or stdin
 * as a filter), run process(), print the result. The model is in process.c,
 * los.c and regress.c. The only file that exits; the modules return -1. */
#define _POSIX_C_SOURCE 200809L  /* isatty */

#include "common.h"
#include "regress.h"
#include "los.h"
#include "process.h"
#include "csv.h"        /* the line reader the stdin path shares with files */
#include "constants.h"
#include "version.h"   /* generated: LINEARR_VERSION, from the git tag */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#ifndef UNIT_TEST   /* the test build (make ut) supplies main() from tests.c */

static void usage(FILE *out, const char *prog) {
    (void)fprintf(out,
        "usage: %s -c MODEL.CSV GROUP [TERM=VALUE ...]  score one case\n"
        "       %s -c MODEL.CSV < cases.csv             score a stream\n"
        "       %s -t TRAIN.CSV [-g GROUP]              fit the coefficients\n"
        "       %s -c MODEL.CSV --terms                 list the model's terms\n"
        "\n"
        "  GROUP TERM=VALUE ...   name only the terms that are not zero:\n"
        "                           %s 001 icu_indicator=1 age_60plus=1\n"
        "  GROUP,x1,...,xp        the same case as a row, every term in the\n"
        "                         coefficient file's column order; this is the\n"
        "                         form read from stdin, so a scored file round\n"
        "                         trips through a pipeline\n"
        "  -t F   fit from training file F, whose rows are group,value,x1..xp;\n"
        "         F may be - to read the training data from a pipe\n"
        "         and whose header names the terms. A complete coefficient file\n"
        "         goes to stdout, the fit summary to stderr\n"
        "  -y NAME, --response NAME   the column holding the value being\n"
        "         predicted, by its header name. Without it that is column 2,\n"
        "         and a file written in another order fits the wrong column\n"
        "  -g G   fit only group G, or '*' to pool every row into one line.\n"
        "         Without -g, every group in the file is fitted, one line each\n"
        "  -c F   the coefficient table to score against. Required for scoring:\n"
        "         fit one with -t first, or name one you have\n"
        "  --qr   solve by QR instead of normal equations: slower per row, and\n"
        "         it does not square the condition number. Use it when the fit\n"
        "         reports a large cond=\n"
        "  --stats F   with -t, one row per GROUP to F (- for stdout): rows, df,\n"
        "         R2, resid SD, cond and how many terms were pinned. The\n"
        "         summary reports the worst of each; this says which group\n"
        "         they came from\n"
        "  --residuals F   with -t, also write one row per training row to F:\n"
        "         GROUP,observed,predicted,residual. Where the model is wrong,\n"
        "         which no summary number can show you\n"
        "  --trim F   the trim table: group,trim_addition. Without it the trim\n"
        "         point is the prediction itself\n"
        "  --no-trim  says that explicitly. Only useful to override a --trim\n"
        "         earlier on the same command line\n"
        "  --scale N / --trim-scale N   decimal places in the prediction and\n"
        "         in the trim point, 0 to 9. Default 4 and 1\n"
        "  --terms  what the loaded coefficient file expects, in order\n"
        "  --footprint TERMS [GROUPS]   how much memory a run of that shape\n"
        "         holds, for fitting and for scoring. Neither figure depends\n"
        "         on the number of rows\n"
        "  -d     debug tracing to stderr\n"
        "  --version  which build this is\n"
        "  -h     this help\n"
        "\n"
        "Files named with -c, --trim and -t are looked for in the\n"
        "current directory first, then beside the program.\n",
        prog, prog, prog, prog, prog);
}

/* "1 terms and 1 groups" reads as unfinished work. */
static const char *s_(long n) { return (n == 1) ? "" : "s"; }

/* Score one line; a bad one is reported and the rest of the file goes on. */
static int score(const char *input) {
    char out[LINEARR_MAX_OUTPUT];
    debug("scoring '%s'", input);
    if (process(input, out, sizeof out) != 0) {
        (void)fprintf(stderr, "cannot score '%s': %s\n", input, process_error());
        return -1;
    }
    (void)printf("%s\n", out);
    return 0;
}

static int score_named(const char *group, char *const *assign, int n) {
    char out[LINEARR_MAX_OUTPUT];
    if (process_named(group, assign, n, out, sizeof out) != 0) {
        (void)fprintf(stderr, "cannot score group '%s': %s\n", group, process_error());
        return -1;
    }
    (void)printf("%s\n", out);
    return 0;
}

/* The only figure that depends on the problem rather than the model alone. */
static void print_bytes(const char *label, double b) {
    if (b < 1024.0)                     (void)printf("%s%.0f bytes\n", label, b);
    else if (b < 1024.0 * 1024)         (void)printf("%s%.1f KB\n", label, b / 1024.0);
    else if (b < 1024.0 * 1024 * 1024)  (void)printf("%s%.1f MB\n", label, b / (1024.0 * 1024));
    else                                (void)printf("%s%.2f GB\n", label, b / (1024.0 * 1024 * 1024));
}

static void print_footprint(int terms, long long groups) {
    size_t fit   = process_group_bytes(terms);
    size_t score = process_model_bytes();

    (void)printf("%d term%s, %lld group%s\n\n", terms, s_(terms), groups, s_(groups));
    (void)printf("fitting, -t, one accumulator per group\n");
    (void)printf("  per group   %zu bytes\n", fit);
    print_bytes("  in total    ", (double)fit * (double)groups);
    (void)printf("\nscoring, a loaded coefficient table\n");
    (void)printf("  per group   %zu bytes\n", score);
    print_bytes("  in total    ", (double)score * (double)groups);
    (void)printf("\nThe scoring figure does not move with the term count: the\n"
                 "coefficient array is sized at this build's ceiling of %d, so a\n"
                 "small model pays for a large one. The fitting figure does move.\n",
                 LOS_MAX_VARS);
    (void)printf("\nNeither depends on the number of ROWS, which is the point:\n"
                 "the same figures cover a thousand rows and a trillion.\n");
}

/* --terms: what the loaded file expects to be typed. */
static void print_terms(void) {
    int i, n = process_nterms();

    (void)printf("%d term%s and %lld group%s in %s\n", n, s_(n), process_ngroups(),
           s_(process_ngroups()), process_coef_path());
    if (los_response_name()[0] != '\0')
        (void)printf("it predicts: %s\n", los_response_name());
    for (i = 0; i < n; i++)
        (void)printf("  %3d  %s\n", i + 1, process_term_name(i));
    (void)printf("\nname them: GROUP %s=1 ...\n", n > 0 ? process_term_name(0) : "TERM");
}

/* What the file was read as, printed on every fit. Column 1 is the group,
 * column 2 the response, the rest terms. Nothing in the data says which is the
 * response, so a file in another order fits and answers another question. */
static void print_reading(void) {
    if (los_response_name()[0] == '\0') return;
    (void)fprintf(stderr, "reading: column 1 is the group, '%s' is the "
                  "value being predicted, and the other %d %s%s\n",
                  los_response_name(), process_nterms(),
                  process_nterms() == 1 ? "column is a term"
                                        : "columns are terms",
                  process_response_named() ? ""
                                           : ". Use -y NAME if that is the wrong column");
}

/* Every group in one pass: the default for -t. */
static int train_all(const char *path, const char *only,
                     const char *resid_file, const char *stats_file) {
    struct fit_summary sum;
    FILE *resid = NULL;
    FILE *stats = NULL;

    if (resid_file) {
        resid = fopen(resid_file, "w");
        if (!resid) die("cannot write %s: %s", resid_file, strerror(errno));
    }
    if (stats_file) {
        /* "-" is stdout, as it is stdin for -t. */
        stats = (strcmp(stats_file, "-") == 0) ? stdout : fopen(stats_file, "w");
        if (!stats) die("cannot write %s: %s", stats_file, strerror(errno));
    }
    if (process_train_residuals(path, only, stdout, resid, stats, &sum) != 0) {
        if (stats) (void)fclose(stats);
        if (resid) (void)fclose(resid);
        (void)fprintf(stderr, "cannot fit: %s\n", process_error());
        return -1;
    }
    /* A residual file truncated by a full disk looks like a model that fits. */
    if (resid) {
        if (fflush(resid) != 0 || ferror(resid) || fclose(resid) != 0)
            die("cannot write %s: %s", resid_file, strerror(errno));
        (void)fprintf(stderr, "residuals: %s\n", resid_file);
    }
    if (stats) {
        if (fflush(stats) != 0 || ferror(stats))
            die("cannot write %s: %s", stats_file, strerror(errno));
        if (stats != stdout && fclose(stats) != 0)
            die("cannot write %s: %s", stats_file, strerror(errno));
        if (stats != stdout)
            (void)fprintf(stderr, "per-group statistics: %s\n", stats_file);
    }
    print_reading();
    /* With one group the aggregates are its own figures, not "worst". */
    {
        int many = (sum.groups != 1);
        if (many) (void)fprintf(stderr, "fit: %lld groups, %lld row%s",
                                sum.groups, sum.rows, s_(sum.rows));
        else      (void)fprintf(stderr, "fit: %lld row%s", sum.rows, s_(sum.rows));
        if (sum.min_r2 >= 0.0)
            fprintf(stderr, ", %sR2=%.4f", many ? "worst " : "", sum.min_r2);
        if (sum.max_sigma >= 0.0)
            fprintf(stderr, ", %sresid SD%s%.4g", many ? "worst " : "",
                    sum.sigma_is_bound ? "<" : "=", sum.max_sigma);
        if (sum.pinned > 0)
            fprintf(stderr, ", %d term%s unidentified and set to 0",
                    sum.pinned, s_(sum.pinned));
        (void)fprintf(stderr, ", %sdf=%lld", many ? "least " : "", sum.min_df);
        if (sum.max_condition > 1.0)
            fprintf(stderr, ", %scond=%.3g (%s%s)", many ? "worst " : "",
                    sum.max_condition, process_solver(),
                    sum.pinned > 0 ? ", over the terms kept" : "");
    }
    (void)fprintf(stderr, "\n");
    if (sum.min_df <= 0)
        (void)fprintf(stderr, "warning: at least one group has no residual degrees of "
                        "freedom; its line passes through every row by "
                        "construction. Fit those groups on more rows.\n");
    /* The worst-of aggregation above skips a group with no R2; counted here. */
    if (sum.groups_flat_y > 0)
        (void)fprintf(stderr, "warning: %lld group%s no R2, because the response "
                        "does not vary there: R2 is 0/0, which is undefined and "
                        "not zero. The line and its residual SD are still "
                        "reported.\n", sum.groups_flat_y,
                        sum.groups_flat_y == 1 ? " has" : "s have");
    if (sum.groups_r2_lost > 0)
        (void)fprintf(stderr, "warning: %lld group%s no R2 that this solver can "
                        "report: the residual came out below zero by more than "
                        "rounding, so the subtraction has no digits left. "
                        "Rescale the response%s.\n", sum.groups_r2_lost,
                        sum.groups_r2_lost == 1 ? " has" : "s have",
                        strcmp(process_solver(), "QR") == 0 ? ""
                        : ", or use --qr, which does not form that difference");
    if (sum.curved_term >= 0)
        (void)fprintf(stderr, "warning: in group %s the residuals still depend on "
                      "%s after the line is subtracted (t=%.1f). A straight line "
                      "is probably the wrong shape in that term; consider adding "
                      "%s as a column.\n",
                      sum.worst_group, process_term_name(sum.curved_term),
                      sum.curved_t, sum.curved_pow == 3 ? "its cube" : "its square");
    if (sum.fitted_t != 0.0 && sum.curved_term < 0)
        (void)fprintf(stderr, "warning: the residuals still depend on the "
                      "prediction itself (t=%.1f), so the model has the wrong "
                      "SHAPE: an interaction between two terms, or a curve "
                      "that no single term shows. (Not a missing column: one "
                      "of those leaves a residual this cannot see.)\n",
                      sum.fitted_t);
    /* Not an unconditional claim of heteroskedasticity: a missing interaction
     * leaves residuals whose size tracks the fitted value, so this probe fires
     * on constant variance -- 167 times in 200 on one design. Fix the mean. */
    if (sum.spread_t != 0.0)
        (void)fprintf(stderr, "warning: the size of the residual moves with the "
                      "prediction (t=%.1f), so the residual SD above is not a "
                      "typical error at either end of the range.%s\n",
                      sum.spread_t,
                      (sum.curved_term >= 0 || sum.fitted_t != 0.0)
                      ? " The shape is wrong too, and a wrong shape produces "
                        "this on its own: fix that first, then read this again."
                      : " The error is not the same everywhere.");
    if (sum.max_condition > 1e8)
        (void)fprintf(stderr, "warning: at least one group is ill-conditioned "
                        "(cond=%.3g); the trailing digits of its coefficients "
                        "are noise.%s\n", sum.max_condition,
                        strcmp(process_solver(), "QR") == 0 ? ""
                        : " Try --qr, which does not square the condition number.");
    return 0;
}

static int train(const char *path, const char *group) {
    char out[LINEARR_MAX_OUTPUT];
    struct fit_info info;

    if (process_train(path, group, out, sizeof out, &info) != 0) {
        (void)fprintf(stderr, "cannot fit: %s\n", process_error());
        return -1;
    }
    print_reading();
    (void)printf("%s\n", out);
    (void)fprintf(stderr, "fit: %lld row%s", info.rows, s_(info.rows));
    if (info.r2 >= 0.0) fprintf(stderr, ", R2=%.4f", info.r2);
    if (info.sigma >= 0.0)
        fprintf(stderr, ", resid SD%s%.4g",
                info.sigma_is_bound ? "<" : "=", info.sigma);
    if (info.pinned > 0)
        (void)fprintf(stderr, ", %d term%s unidentified and set to 0",
                info.pinned, s_(info.pinned));
    (void)fprintf(stderr, ", df=%lld", info.df);
    if (info.condition > 1.0)
        fprintf(stderr, ", cond=%.3g (%s%s)", info.condition, process_solver(),
                info.pinned > 0 ? ", over the terms kept" : "");
    (void)fprintf(stderr, "\n");
    /* Neither R2 nor the residual file sees this: an ill-conditioned design
     * fits its own sample well. What it gets wrong is the coefficients. */
    if (info.condition > 1e8)
        (void)fprintf(stderr, "warning: the design is ill-conditioned (cond=%.3g). "
                        "The trailing digits of these coefficients are noise; "
                        "%srescale your columns, or drop a near-duplicate "
                        "one.\n", info.condition,
                        strcmp(process_solver(), "QR") == 0 ? "" : "try --qr, ");
    /* Which of the two, not both: different events, different remedies. */
    if (info.r2 == REGRESS_R2_FLAT_Y)
        (void)fprintf(stderr, "warning: no R2 here, because the response does not "
                        "vary: R2 is 0/0, which is undefined and not zero. The "
                        "line and its residual SD are still reported.\n");
    else if (info.r2 < 0.0)
        (void)fprintf(stderr, "warning: no R2 that this solver can report: the "
                        "residual came out below zero by more than rounding, so "
                        "the subtraction has no digits left. Rescale the "
                        "response%s.\n",
                        strcmp(process_solver(), "QR") == 0 ? ""
                        : ", or use --qr, which does not form that difference");
    /* An R2 of 1 from a saturated fit reads like success. */
    if (info.df <= 0)
        (void)fprintf(stderr, "warning: no residual degrees of freedom; this line "
                        "passes through every row by construction, and its R2 "
                        "means nothing. Fit it on more rows.\n");
    return 0;
}

/* A scale, read as --footprint reads its term count: atoi cannot tell "abc"
 * from 0. The trim point is built on the rounded prediction. */
static int scale_arg(const char *s, const char *opt) {
    char *end;
    long v;

    if (s[0] == '\0') die("%s takes 0 to 9 decimal places", opt);
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 9)
        die("%s takes 0 to 9 decimal places", opt);
    return (int)v;
}

/* Load the model, or stop: one fatal condition, not a complaint per row. */
static void need_model(void) {
    /* As wide as process_error()'s buffer: resolve_file lists directories. */
    char err[RESOLVE_PATH_MAX + 512];
    if (process_init(err, sizeof err) != 0) die("%s", err);
}

int main(int argc, char **argv) {
    static struct option longopts[] = {
        { "terms",   no_argument,       NULL, 'T' },
        { "footprint", required_argument, NULL, 'F' },
        { "response",   required_argument, NULL, 'y' },
        { "scale",      required_argument, NULL, 'S' },
        { "trim-scale", required_argument, NULL, 'Z' },
        { "version", no_argument,       NULL, 'V' },
        { "coef",    required_argument, NULL, 'c' },
        { "trim",    required_argument, NULL, 'R' },
        { "no-trim",   no_argument,       NULL, 'N' },
        { "residuals", required_argument, NULL, 'E' },
        { "stats",     required_argument, NULL, 'G' },
        { "qr",        no_argument,       NULL, 'Q' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *train_file = NULL;
    const char *group = NULL;
    const char *resid_file = NULL;
    const char *stats_file = NULL;
    char line[LINEARR_MAX_INPUT];
    int c, bad = 0, want_terms = 0;
    int  footprint_terms = 0;
    int  response_named = 0, want_qr = 0;
    int  scoring_opt = 0;               /* --scale, --trim-scale, --trim, --no-trim */
    const char *scoring_opt_name = NULL;
    long long footprint_groups = 1;

    g_prog = argv[0];               /* resolve.c finds our files from this */

    while ((c = getopt_long(argc, argv, "dhc:t:g:y:", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': g_debug = 1; break;
            case 't': train_file = optarg; break;
            case 'y': response_named = 1; process_use_response(optarg); break;
            case 'g': group = optarg; break;
            case 'T': want_terms = 1; break;
            case 'F': {   /* atoi cannot tell "abc" from 0, and 0 falls through
                           * every branch to the stdin path. */
                          char *end;
                          long v = strtol(optarg, &end, 10);
                          if (*end != '\0' || v < 1 || v > REGRESS_MAX_VARS)
                              die("--footprint takes a term count of 1 to %d",
                                  REGRESS_MAX_VARS);
                          footprint_terms = (int)v;
                      } break;
            case 'S': scoring_opt = 1; scoring_opt_name = "--scale";
                      (void)process_set_scale(scale_arg(optarg, "--scale"));
                      break;
            case 'Z': scoring_opt = 1; scoring_opt_name = "--trim-scale";
                      (void)process_set_trim_scale(scale_arg(optarg, "--trim-scale"));
                      break;
            case 'c': process_use_coef(optarg); break;
            case 'R': scoring_opt = 1; scoring_opt_name = "--trim";
                      process_use_trim(optarg); break;
            case 'N': scoring_opt = 1; scoring_opt_name = "--no-trim";
                      process_use_trim(NULL); break;
            case 'E': resid_file = optarg; break;
            case 'G': stats_file = optarg; break;
            case 'Q': want_qr = 1; process_use_qr(1); break;
            case 'V': printf("linearr %s\nBSD 2-Clause; no warranty.\n",
                             LINEARR_VERSION); return 0;
            case 'h': usage(stdout, argv[0]); return 0;
            default:  usage(stderr, argv[0]); return 2;
        }
    }

    /* Silently ignoring an option makes a user believe something happened. */
    if (group && !train_file)
        die("-g names a group to fit, so it needs -t TRAIN.CSV");
    if (stats_file && !train_file)
        die("--stats writes one row per GROUP fitted, so it needs -t");
    if (resid_file && !train_file)
        die("--residuals writes one row per TRAINING row, so it needs -t");
    if (want_terms && train_file)
        die("--terms lists the loaded model; it cannot be combined with -t");
    /* Both reach only the fitter; scoring's schema comes from -c's table. */
    if (response_named && !train_file)
        die("-y names the column to predict when fitting, so it needs -t "
            "TRAIN.CSV. Scoring takes its columns from the table given to -c");
    if (want_qr && !train_file)
        die("--qr chooses the solver that does the fitting, so it needs -t "
            "TRAIN.CSV. Scoring only multiplies out coefficients already fitted");
    /* The reciprocals: --footprint reads no data, rounding is the scorer's. */
    if (footprint_terms > 0 && train_file)
        die("--footprint reports what a shape would hold and reads no data, so "
            "it cannot be combined with -t");
    if (footprint_terms > 0 && want_terms)
        die("--footprint reports what a shape would hold; --terms lists a loaded "
            "model. Ask for one or the other");
    if (scoring_opt && train_file)
        die("%s applies to a prediction, so it needs a case to score, not -t. "
            "Fitting writes unrounded coefficients: the rounding belongs to the "
            "figure they produce", scoring_opt_name);
    if (footprint_terms > 0) {
        /* An optional group count follows the term count. */
        if (optind < argc) {
            char *end;
            errno = 0;
            footprint_groups = strtoll(argv[optind], &end, 10);
            /* ERANGE, because strtoll saturates to LLONG_MAX. */
            if (*end != '\0' || errno == ERANGE || footprint_groups < 1)
                die("--footprint's group count must be a whole number, 1 or more");
            optind++;
        }
    }
    /* An operand these commands do not read is a mistyped option. */
    if ((train_file || want_terms || footprint_terms > 0) && optind < argc)
        die("'%s' is not used by this command", argv[optind]);

    if (want_terms) {
        need_model();
        print_terms();
    } else if (footprint_terms > 0) {
        print_footprint(footprint_terms, footprint_groups);
    } else if (train_file) {
        /* The two-pass path fits one named group as well as all of them. */
        bad = ((group && !resid_file && !stats_file) ? train(train_file, group)
                     : train_all(train_file, group, resid_file, stats_file)) != 0;
    } else if (optind < argc) {
        need_model();
        /* A comma in the first argument means the row form, else TERM=VALUE. */
        if (strchr(argv[optind], ',')) {
            int i;
            for (i = optind; i < argc; i++)
                if (score(argv[i]) != 0) bad = 1;
        } else {
            bad = score_named(argv[optind], argv + optind + 1,
                              argc - optind - 1) != 0;
        }
    } else if (isatty(STDIN_FILENO)) {
        /* A terminal on stdin and nothing to read: the bare command. A pipe is
         * not a terminal, so a filter still gets the loop below. */
        usage(stdout, argv[0]);
    } else {
        need_model();
        for (;;) {
            /* csv.c's reader, the one files use: it counts the bytes stored, so
             * a NUL and an over-long line differ, and it consumes the line. */
            int eof, rv = csv_read_line(stdin, line, sizeof line, &eof);

            if (eof || rv == CSV_ERR_IO) break;
            if (rv < 0) {
                (void)fprintf(stderr, "cannot score: the input %s\n",
                              csv_line_error(rv, sizeof line));
                bad = 1;
                continue;
            }
            /* Excel's "CSV UTF-8" writes three invisible bytes at the file
             * start; left in place they join the first group name. */
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF)
                memmove(line, line + 3, strlen(line + 3) + 1);
            if (line[0] == '\0' || line[0] == '#') continue;
            if (score(line) != 0) bad = 1;
        }
        if (ferror(stdin)) die("cannot read stdin: %s", strerror(errno));
        /* An empty pipe is not an error: `grep | linearr` may match none. */
    }

    process_free();

    /* A failure to write stdout is a failure: `-t train.csv > model.csv` on a
     * full disk must not install an empty coefficient table and exit 0. */
    if (fflush(stdout) != 0 || ferror(stdout))
        die("cannot write output: %s", strerror(errno));

    return bad ? 1 : 0;
}

#endif /* !UNIT_TEST */
