/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* main.c -- the CLI front end: parse options, get the input (arguments, or
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

#ifndef UNIT_TEST   /* the test build (make ut) supplies main() from tests.c */

static void usage(FILE *out, const char *prog) {
    fprintf(out,
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
        "  -g G   fit only group G (default '*': every row pooled)\n"
        "  --terms  what the loaded coefficient file expects, in order\n"
        "  -d     debug tracing to stderr\n"
        "  -h     this help\n"
        "\n"
        "Files (coef.file, trim.file, and -t's argument) are looked for in the\n"
        "current directory first, then beside the program.\n",
        prog, prog, prog, prog, prog);
}

/* Score one line; report a bad one and keep going, so a bad row in a batch does
 * not throw away the rest of the file. */
static int score(const char *input) {
    char out[MAX_OUTPUT];
    debug("scoring '%s'", input);
    if (process(input, out, sizeof out) != 0) {
        fprintf(stderr, "cannot score '%s': %s\n", input, process_error());
        return -1;
    }
    printf("%s\n", out);
    return 0;
}

static int score_named(const char *group, char *const *assign, int n) {
    char out[MAX_OUTPUT];
    if (process_named(group, assign, n, out, sizeof out) != 0) {
        fprintf(stderr, "cannot score group '%s': %s\n", group, process_error());
        return -1;
    }
    printf("%s\n", out);
    return 0;
}

/* --terms: the answer to "what am I supposed to type?". Without it the only way
 * to learn the model's column names was to open the CSV and count. */
static void print_terms(void) {
    int i, n = process_nterms();

    printf("%d terms and %ld groups in %s\n", n, process_ngroups(),
           process_coef_path());
    for (i = 0; i < n; i++)
        printf("  %3d  %s\n", i + 1, process_term_name(i));
    printf("\nname them: GROUP %s=1 ...\n", n > 0 ? process_term_name(0) : "TERM");
}

static int train(const char *path, const char *group) {
    char out[MAX_OUTPUT];
    struct fit_info info;

    if (process_train(path, group, out, sizeof out, &info) != 0) {
        fprintf(stderr, "cannot fit: %s\n", process_error());
        return -1;
    }
    printf("%s\n", out);
    fprintf(stderr, "fit: %ld rows", info.rows);
    if (info.r2 >= 0.0) fprintf(stderr, ", R2=%.4f", info.r2);
    if (info.pinned > 0)
        fprintf(stderr, ", %d term%s unidentified and set to 0",
                info.pinned, info.pinned == 1 ? "" : "s");
    fprintf(stderr, ", df=%ld\n", info.df);
    /* Said plainly, because an R2 of 1 from a saturated fit reads like success
     * and is the easiest way to publish a model that knows nothing. */
    if (info.df <= 0)
        fprintf(stderr, "warning: no residual degrees of freedom -- this line "
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
        { "terms", no_argument, NULL, 'T' },
        { "help",  no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *train_file = NULL;
    const char *group = NULL;
    char line[MAX_INPUT];
    int c, bad = 0, want_terms = 0;

    g_prog = argv[0];               /* resolve.c finds our files from this */

    while ((c = getopt_long(argc, argv, "dht:g:", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': g_debug = 1; break;
            case 't': train_file = optarg; break;
            case 'g': group = optarg; break;
            case 'T': want_terms = 1; break;
            case 'h': usage(stdout, argv[0]); return 0;
            default:  usage(stderr, argv[0]); return 2;
        }
    }

    /* Absent or unreadable, the defaults in process.c apply; that is not an
     * error, so the program runs from anywhere with the tables beside it. */
    if (params_load("system.properties") != 0)
        debug("no system.properties; using built-in defaults");

    if (want_terms) {
        need_model();
        print_terms();
    } else if (train_file) {
        bad = train(train_file, group) != 0;
    } else if (optind < argc) {
        need_model();
        /* A comma in the first argument means the row form -- and then every
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
         * after a Ctrl-C -- the worst possible first impression. A filter still
         * gets its filter behaviour below, because a pipe is not a terminal. */
        usage(stdout, argv[0]);
    } else {
        need_model();
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            if (score(line) != 0) bad = 1;
        }
        /* An empty pipe is not an error: `grep ... | linearr` matching nothing
         * is an ordinary outcome, and a filter that lectures about it is noise. */
    }

    process_free();
    params_free();
    return bad ? 1 : 0;
}

#endif /* !UNIT_TEST */
