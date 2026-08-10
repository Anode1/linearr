/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* main.c, the CLI front end: parse options, get the input (arguments, or
 * lines from stdin so it works as a filter), run process(), print the result.
 * The scaffolding stays; the model lives in process.c, los.c and regress.c.
 * This is the only file that exits: the modules return -1 and let it decide. */
#define _POSIX_C_SOURCE 200809L  /* isatty */

#include "common.h"
#include "regress.h"
#include "los.h"
#include "process.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#ifndef UNIT_TEST   /* the test build (make ut) supplies main() from tests.c */

static void usage(FILE *out, const char *prog) {
    (void)fprintf(out,
        "usage: %s [-d] [-h] GROUP [TERM=VALUE ...]     score one case\n"
        "       %s [-d] < cases.csv                     score a stream\n"
        "       %s -t TRAIN.CSV [-g GROUP]              fit the coefficients\n"
        "       %s --terms                              list the model's terms\n"
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
        "  --stats F   with -t, one row per GROUP to F: rows, df, R2, resid SD,\n"
        "         cond and how many terms were pinned. The summary reports the\n"
        "         worst of each; this says which group they came from\n"
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

/* "1 terms and 1 groups" reads as unfinished work, and this program asks to be
 * trusted with numbers. */
static const char *s_(long n) { return (n == 1) ? "" : "s"; }

/* Score one line; report a bad one and keep going, so a bad row in a batch does
 * not throw away the rest of the file. */
static int score(const char *input) {
    char out[MAX_OUTPUT];
    debug("scoring '%s'", input);
    if (process(input, out, sizeof out) != 0) {
        (void)fprintf(stderr, "cannot score '%s': %s\n", input, process_error());
        return -1;
    }
    (void)printf("%s\n", out);
    return 0;
}

static int score_named(const char *group, char *const *assign, int n) {
    char out[MAX_OUTPUT];
    if (process_named(group, assign, n, out, sizeof out) != 0) {
        (void)fprintf(stderr, "cannot score group '%s': %s\n", group, process_error());
        return -1;
    }
    (void)printf("%s\n", out);
    return 0;
}

/* --terms: the answer to "what am I supposed to type?". Without it the only way
 * to learn the model's column names was to open the CSV and count. */
/* What a run will hold. Asked often enough to be worth answering without a
 * trial run, and the only figure in this program that depends on the problem
 * rather than on the model alone. */
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

static void print_terms(void) {
    int i, n = process_nterms();

    (void)printf("%d term%s and %lld group%s in %s\n", n, s_(n), process_ngroups(),
           s_(process_ngroups()), process_coef_path());
    /* What the model predicts, when the file says so. A table of coefficients
     * with no response named cannot be identified a week later, and -t writes
     * the name in for exactly that reason. */
    if (los_response_name()[0] != '\0')
        (void)printf("it predicts: %s\n", los_response_name());
    for (i = 0; i < n; i++)
        (void)printf("  %3d  %s\n", i + 1, process_term_name(i));
    (void)printf("\nname them: GROUP %s=1 ...\n", n > 0 ? process_term_name(0) : "TERM");
}

/* Every group in one pass. This is the default for -t now, because "one fitted
 * line per group" is what the model IS: the old default pooled every row into a
 * single line labelled '*', and getting a real table meant one invocation and
 * one full re-read of the training file per group. */
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
        stats = fopen(stats_file, "w");
        if (!stats) die("cannot write %s: %s", stats_file, strerror(errno));
    }
    if (process_train_residuals(path, only, stdout, resid, stats, &sum) != 0) {
        if (stats) (void)fclose(stats);
        if (resid) (void)fclose(resid);
        (void)fprintf(stderr, "cannot fit: %s\n", process_error());
        return -1;
    }
    /* A residual file truncated by a full disk is worse than none: it looks
     * like a model that fits. This was unchecked, so `--residuals /dev/full`
     * wrote nothing and exited 0. That is the defect fixed for stdout a few
     * commits earlier, in a comment congratulating itself; the test that caught
     * it for stdout tested stdout, and the residual file went unexamined. */
    if (resid) {
        if (fflush(resid) != 0 || ferror(resid) || fclose(resid) != 0)
            die("cannot write %s: %s", resid_file, strerror(errno));
        (void)fprintf(stderr, "residuals: %s\n", resid_file);
    }
    if (stats) {
        if (fflush(stats) != 0 || ferror(stats) || fclose(stats) != 0)
            die("cannot write %s: %s", stats_file, strerror(errno));
        (void)fprintf(stderr, "per-group statistics: %s\n", stats_file);
    }
    /* What the file was read AS, before what came of it. The layout is fixed
     * -- column 1 the group, column 2 the response, the rest terms -- and a
     * file written in another order fits perfectly well and answers a question
     * nobody asked. Nothing in the data can say which column is the response,
     * so the program says which one it took, every time, where a reader will
     * see it without being told to look. */
    if (los_response_name()[0] != '\0')
        (void)fprintf(stderr, "reading: column 1 is the group, '%s' is the "
                      "value being predicted, and the other %d %s%s\n",
                      los_response_name(), process_nterms(),
                      process_nterms() == 1 ? "column is a term"
                                            : "columns are terms",
                      process_response_named() ? ""
                                               : ". Use -y NAME if that is the wrong column");
    (void)fprintf(stderr, "fit: %lld group%s, %lld row%s", sum.groups, s_(sum.groups),
            sum.rows, s_(sum.rows));
    if (sum.pinned > 0)
        fprintf(stderr, ", %d term-slot%s pinned to 0", sum.pinned, s_(sum.pinned));
    (void)fprintf(stderr, ", least df=%lld", sum.min_df);
    if (sum.max_sigma >= 0.0)
        fprintf(stderr, ", worst resid SD%s%.4g",
                sum.sigma_is_bound ? "<" : "=", sum.max_sigma);
    if (sum.max_condition > 1.0)
        fprintf(stderr, ", worst cond=%.3g (%s)", sum.max_condition, process_solver());
    (void)fprintf(stderr, "\n");
    if (sum.min_df <= 0)
        (void)fprintf(stderr, "warning: at least one group has no residual degrees of "
                        "freedom; its line passes through every row by "
                        "construction. Fit those groups on more rows.\n");
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
    if (sum.spread_t != 0.0)
        (void)fprintf(stderr, "warning: the size of the residual moves with the "
                      "prediction (t=%.1f), so the error is not the same "
                      "everywhere and the residual SD above is not a typical "
                      "error at either end of the range.\n", sum.spread_t);
    if (sum.max_condition > 1e8)
        (void)fprintf(stderr, "warning: at least one group is ill-conditioned "
                        "(cond=%.3g); the trailing digits of its coefficients "
                        "are noise.%s\n", sum.max_condition,
                        strcmp(process_solver(), "QR") == 0 ? ""
                        : " Try --qr, which does not square the condition number.");
    return 0;
}

static int train(const char *path, const char *group) {
    char out[MAX_OUTPUT];
    struct fit_info info;

    if (process_train(path, group, out, sizeof out, &info) != 0) {
        (void)fprintf(stderr, "cannot fit: %s\n", process_error());
        return -1;
    }
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
    /* R2 cannot see this failure: an ill-conditioned design fits its own sample
     * beautifully and predicts nothing. Normal equations square the condition
     * number, so this is the diagnostic that has to be said out loud. */
    if (info.condition > 1e8)
        (void)fprintf(stderr, "warning: the design is ill-conditioned (cond=%.3g). "
                        "The trailing digits of these coefficients are noise; "
                        "%srescale your columns, or drop a near-duplicate "
                        "one.\n", info.condition,
                        strcmp(process_solver(), "QR") == 0 ? "" : "try --qr, ");
    if (info.r2 < 0.0)
        (void)fprintf(stderr, "warning: R2 is not reportable here: the response "
                        "does not vary, or the fit consumed all of its "
                        "variance.\n");
    /* Said plainly, because an R2 of 1 from a saturated fit reads like success
     * and is the easiest way to publish a model that knows nothing. */
    if (info.df <= 0)
        (void)fprintf(stderr, "warning: no residual degrees of freedom; this line "
                        "passes through every row by construction, and its R2 "
                        "means nothing. Fit it on more rows.\n");
    return 0;
}

/* Load the model, or stop with the reason. Scoring cannot proceed without it,
 * and a table that will not open is one fatal condition, not a complaint to
 * repeat per row. */
static void need_model(void) {
    char err[512];
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
    char line[MAX_INPUT];
    int c, bad = 0, want_terms = 0;
    int  footprint_terms = 0;
    long footprint_groups = 1;

    g_prog = argv[0];               /* resolve.c finds our files from this */

    while ((c = getopt_long(argc, argv, "dhc:t:g:y:", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': g_debug = 1; break;
            case 't': train_file = optarg; break;
            case 'y': process_use_response(optarg); break;
            case 'g': group = optarg; break;
            case 'T': want_terms = 1; break;
            case 'F': {   /* atoi cannot tell "abc" from 0, and 0 fell through
                           * every branch to the stdin path, which then died
                           * about a missing coefficient table. */
                          char *end;
                          long v = strtol(optarg, &end, 10);
                          if (*end != '\0' || v < 1 || v > REGRESS_MAX_VARS)
                              die("--footprint takes a term count of 1 to %d",
                                  REGRESS_MAX_VARS);
                          footprint_terms = (int)v;
                      } break;
            case 'S': if (process_set_scale(atoi(optarg)) != 0)
                          die("--scale takes 0 to 9 decimal places");
                      break;
            case 'Z': if (process_set_trim_scale(atoi(optarg)) != 0)
                          die("--trim-scale takes 0 to 9 decimal places");
                      break;
            case 'c': process_use_coef(optarg); break;
            case 'R': process_use_trim(optarg); break;
            case 'N': process_use_trim(NULL); break;
            case 'E': resid_file = optarg; break;
            case 'G': stats_file = optarg; break;
            case 'Q': process_use_qr(1); break;
            case 'V': printf("linearr %s\nBSD 2-Clause; no warranty.\n",
                             LINEARR_VERSION); return 0;
            case 'h': usage(stdout, argv[0]); return 0;
            default:  usage(stderr, argv[0]); return 2;
        }
    }

    /* Absent or unreadable, the defaults in process.c apply; that is not an
     * error, so the program runs from anywhere with the tables beside it. */

    /* Silently ignoring an option is how a user comes to believe something
     * happened. Each of these used to be accepted and dropped. */
    if (group && !train_file)
        die("-g names a group to fit, so it needs -t TRAIN.CSV");
    if (stats_file && !train_file)
        die("--stats writes one row per GROUP fitted, so it needs -t");
    if (resid_file && !train_file)
        die("--residuals writes one row per TRAINING row, so it needs -t");
    if (want_terms && train_file)
        die("--terms lists the loaded model; it cannot be combined with -t");
    if (footprint_terms > 0) {
        /* An optional group count follows, so the common question ("how much
         * for 400,000 groups of 24 terms?") is one command and no arithmetic. */
        if (optind < argc) {
            char *end;
            footprint_groups = strtoll(argv[optind], &end, 10);
            if (*end != '\0' || footprint_groups < 1)
                die("--footprint's group count must be a whole number, 1 or more");
        }
    }

    if (want_terms) {
        need_model();
        print_terms();
    } else if (footprint_terms > 0) {
        print_footprint(footprint_terms, footprint_groups);
    } else if (train_file) {
        /* -g with --residuals used to be refused, because the single-group
         * path had never been wired for the second pass. The refusal was the
         * easier fix and the wrong one: the residuals of ONE group are exactly
         * what you want when a summary line has told you which group is
         * wrong. */
        bad = ((group && !resid_file && !stats_file) ? train(train_file, group)
                     : train_all(train_file, group, resid_file, stats_file)) != 0;
    } else if (optind < argc) {
        need_model();
        /* A comma in the first argument means the row form, and then every
         * argument is a row. Otherwise it is a group, and what follows are its
         * term=value assignments. */
        if (strchr(argv[optind], ',')) {
            int i;
            for (i = optind; i < argc; i++)
                if (score(argv[i]) != 0) bad = 1;
        } else {
            bad = score_named(argv[optind], argv + optind + 1,
                              argc - optind - 1) != 0;
        }
    } else if (isatty(STDIN_FILENO)) {
        /* Nothing to read and a terminal on stdin: the user typed the bare
         * command and wants to know what it does. Reading stdin here made the
         * program sit there silently looking hung, and only showed the usage
         * after a Ctrl-C: the worst possible first impression. A filter still
         * gets its filter behaviour below, because a pipe is not a terminal. */
        usage(stdout, argv[0]);
    } else {
        need_model();
        while (fgets(line, sizeof line, stdin)) {
            size_t n;
            /* Excel's "CSV UTF-8" puts three invisible bytes at the start of
             * the file. Left in place they join the first group name, and the
             * program then reports that a group is missing from a table it is
             * plainly in. */
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF)
                memmove(line, line + 3, strlen(line + 3) + 1);
            n = strcspn(line, "\r\n");

            /* No line ending and not at end of file means the line did not fit.
             * Reading on would score the REMAINDER as a case of its own: one
             * physical line produced two confident predictions on stdout, with
             * only the first half reported as an error. csv_next has always
             * guarded this for files; stdin did not. Drain to the newline and
             * refuse the whole line. */
            if (line[n] == '\0' && !feof(stdin)) {
                int ch;
                while ((ch = fgetc(stdin)) != EOF && ch != '\n')
                    ;
                (void)fprintf(stderr, "cannot score: a line longer than %d bytes\n",
                        MAX_INPUT - 1);
                bad = 1;
                continue;
            }
            line[n] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            if (score(line) != 0) bad = 1;
        }
        if (ferror(stdin)) die("cannot read stdin: %s", strerror(errno));
        /* An empty pipe is not an error: `grep ... | linearr` matching nothing
         * is an ordinary outcome, and a filter that lectures about it is noise. */
    }

    process_free();

    /* Every printf above was unchecked, so `linearr -t train.csv > model.csv` on
     * a full disk or over quota wrote nothing, said nothing, and exited 0,
     * installing an empty coefficient table while reporting success. stdout is
     * an output the caller is relying on; a failure to produce it is a failure. */
    if (fflush(stdout) != 0 || ferror(stdout))
        die("cannot write output: %s", strerror(errno));

    return bad ? 1 : 0;
}

#endif /* !UNIT_TEST */
