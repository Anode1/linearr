/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* main.c, the CLI front end: parse options, get the input (arguments, or
 * lines from stdin so it works as a filter), run process(), print the result.
 * The scaffolding stays; the model lives in process.c, los.c and regress.c.
 * This is the only file that exits: the modules return -1 and let it decide. */
#define _POSIX_C_SOURCE 200809L  /* isatty */

#include "common.h"
#include "process.h"
#include "params.h"
#include "constants.h"

#include <stdio.h>
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
        "  -t F   fit from training file F, whose rows are GROUP,VALUE,x1..xp\n"
        "         and whose header names the terms. A complete coefficient file\n"
        "         goes to stdout, the fit summary to stderr\n"
        "  -g G   fit only group G, or '*' to pool every row into one line.\n"
        "         Without -g, every group in the file is fitted, one line each\n"
        "  -c F   read the coefficient table from F instead of system.properties\n"
        "  --residuals F   with -t, also write one row per training row to F:\n"
        "         GROUP,observed,predicted,residual. Where the model is wrong,\n"
        "         which no summary number can show you\n"
        "  --trim F / --no-trim   the trim table, or none\n"
        "  --terms  what the loaded coefficient file expects, in order\n"
        "  -d     debug tracing to stderr\n"
        "  --version  which build this is\n"
        "  -h     this help\n"
        "\n"
        "Files (coef.file, trim.file, and -t's argument) are looked for in the\n"
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
static void print_terms(void) {
    int i, n = process_nterms();

    (void)printf("%d term%s and %ld group%s in %s\n", n, s_(n), process_ngroups(),
           s_(process_ngroups()), process_coef_path());
    for (i = 0; i < n; i++)
        (void)printf("  %3d  %s\n", i + 1, process_term_name(i));
    (void)printf("\nname them: GROUP %s=1 ...\n", n > 0 ? process_term_name(0) : "TERM");
}

/* Every group in one pass. This is the default for -t now, because "one fitted
 * line per group" is what the model IS: the old default pooled every row into a
 * single line labelled '*', and getting a real table meant one invocation and
 * one full re-read of the training file per group. */
static int train_all(const char *path, const char *resid_file) {
    struct fit_summary sum;
    FILE *resid = NULL;

    if (resid_file) {
        resid = fopen(resid_file, "w");
        if (!resid) die("cannot write %s: %s", resid_file, strerror(errno));
    }
    if (process_train_residuals(path, stdout, resid, &sum) != 0) {
        if (resid) fclose(resid);
        (void)fprintf(stderr, "cannot fit: %s\n", process_error());
        return -1;
    }
    (void)fprintf(stderr, "fit: %ld group%s, %ld row%s", sum.groups, s_(sum.groups),
            sum.rows, s_(sum.rows));
    if (sum.pinned > 0)
        fprintf(stderr, ", %d term-slot%s pinned to 0", sum.pinned, s_(sum.pinned));
    (void)fprintf(stderr, ", least df=%ld", sum.min_df);
    if (sum.max_sigma >= 0.0) fprintf(stderr, ", worst resid SD=%.4g", sum.max_sigma);
    if (sum.max_condition > 1.0) fprintf(stderr, ", worst cond=%.3g", sum.max_condition);
    (void)fprintf(stderr, "\n");
    if (sum.min_df <= 0)
        (void)fprintf(stderr, "warning: at least one group has no residual degrees of "
                        "freedom; its line passes through every row by "
                        "construction. Fit those groups on more rows.\n");
    if (sum.max_condition > 1e8)
        (void)fprintf(stderr, "warning: at least one group is ill-conditioned (cond=%.3g); "
                        "the trailing digits of its coefficients are noise.\n",
                sum.max_condition);
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
    (void)fprintf(stderr, "fit: %ld row%s", info.rows, s_(info.rows));
    if (info.r2 >= 0.0) fprintf(stderr, ", R2=%.4f", info.r2);
    if (info.sigma >= 0.0) fprintf(stderr, ", resid SD=%.4g", info.sigma);
    if (info.pinned > 0)
        (void)fprintf(stderr, ", %d term%s unidentified and set to 0",
                info.pinned, s_(info.pinned));
    (void)fprintf(stderr, ", df=%ld", info.df);
    if (info.condition > 1.0) fprintf(stderr, ", cond=%.3g", info.condition);
    (void)fprintf(stderr, "\n");
    /* R2 cannot see this failure: an ill-conditioned design fits its own sample
     * beautifully and predicts nothing. Normal equations square the condition
     * number, so this is the diagnostic that has to be said out loud. */
    if (info.condition > 1e8)
        (void)fprintf(stderr, "warning: the design is ill-conditioned (cond=%.3g). The "
                        "trailing digits of these coefficients are noise; rescale "
                        "your columns or drop a near-duplicate one.\n", info.condition);
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
        { "version", no_argument,       NULL, 'V' },
        { "coef",    required_argument, NULL, 'c' },
        { "trim",    required_argument, NULL, 'R' },
        { "no-trim",   no_argument,       NULL, 'N' },
        { "residuals", required_argument, NULL, 'E' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *train_file = NULL;
    const char *group = NULL;
    const char *resid_file = NULL;
    char line[MAX_INPUT];
    int c, bad = 0, want_terms = 0;

    g_prog = argv[0];               /* resolve.c finds our files from this */

    while ((c = getopt_long(argc, argv, "dhc:t:g:", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': g_debug = 1; break;
            case 't': train_file = optarg; break;
            case 'g': group = optarg; break;
            case 'T': want_terms = 1; break;
            case 'c': process_use_coef(optarg); break;
            case 'R': process_use_trim(optarg); break;
            case 'N': process_use_trim(NULL); break;
            case 'E': resid_file = optarg; break;
            case 'V': printf("linearr %s\nGNU GPL v2 or later; no warranty.\n",
                             LINEARR_VERSION); return 0;
            case 'h': usage(stdout, argv[0]); return 0;
            default:  usage(stderr, argv[0]); return 2;
        }
    }

    /* Absent or unreadable, the defaults in process.c apply; that is not an
     * error, so the program runs from anywhere with the tables beside it. */
    if (params_load("system.properties") != 0)
        debug("no system.properties; using built-in defaults");

    /* Silently ignoring an option is how a user comes to believe something
     * happened. Each of these used to be accepted and dropped. */
    if (group && !train_file)
        die("-g names a group to fit, so it needs -t TRAIN.CSV");
    if (resid_file && !train_file)
        die("--residuals writes one row per TRAINING row, so it needs -t");
    if (resid_file && group)
        die("--residuals covers every group; use it without -g");
    if (want_terms && train_file)
        die("--terms lists the loaded model; it cannot be combined with -t");

    if (want_terms) {
        need_model();
        print_terms();
    } else if (train_file) {
        bad = (group ? train(train_file, group)
                     : train_all(train_file, resid_file)) != 0;
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
            size_t n = strcspn(line, "\r\n");

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
    params_free();

    /* Every printf above was unchecked, so `linearr -t train.csv > model.csv` on
     * a full disk or over quota wrote nothing, said nothing, and exited 0,
     * installing an empty coefficient table while reporting success. stdout is
     * an output the caller is relying on; a failure to produce it is a failure. */
    if (fflush(stdout) != 0 || ferror(stdout))
        die("cannot write output: %s", strerror(errno));

    return bad ? 1 : 0;
}

#endif /* !UNIT_TEST */
