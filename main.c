/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* main.c -- the CLI front end: parse options, get the input (an argument, or
 * lines from stdin so it works as a filter), run process(), print the result.
 * The scaffolding stays; the model lives in process.c, los.c and regress.c.
 * This is the only file that exits: the modules return -1 and let it decide. */
#include "common.h"
#include "process.h"
#include "params.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <getopt.h>

#ifndef UNIT_TEST   /* the test build (make ut) supplies main() from tests.c */

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "usage: %s [-d] [-h] [-t TRAIN.CSV [-g GROUP]] [CASE]\n"
        "  CASE  \"GROUP,x1,...,xp\" -- a group and one value per term, as many\n"
        "        as the coefficient file's header names; prints the prediction\n"
        "        and the trim point\n"
        "  without CASE: score every line read from stdin (filter mode)\n"
        "  -t F  fit the coefficients from training file F instead of scoring;\n"
        "        its rows are \"GROUP,VALUE,x1,...,xp\" and its header names the\n"
        "        terms. Prints a complete coefficient file on stdout and the\n"
        "        fit summary on stderr\n"
        "  -g G  fit only group G (default '*': every row pooled)\n"
        "  -d    debug tracing to stderr\n"
        "  -h    this help\n", prog);
}

/* Score one line; report a bad one and keep going, so a bad row in a batch does
 * not throw away the rest of the file. Returns 0 if it scored. */
static int score(const char *input) {
    char out[MAX_OUTPUT];
    debug("scoring '%s'", input);
    if (process(input, out, sizeof out) != 0) {
        fprintf(stderr, "cannot score: %s\n", input);
        return -1;
    }
    printf("%s\n", out);
    return 0;
}

static int train(const char *path, const char *group) {
    char out[MAX_OUTPUT];
    struct fit_info info;

    if (process_train(path, group, out, sizeof out, &info) != 0) {
        fprintf(stderr, "cannot fit %s from %s (run with -d for the reason)\n",
                group ? group : "*", path);
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

int main(int argc, char **argv) {
    const char *train_file = NULL;
    const char *group = NULL;
    char line[MAX_INPUT];
    int c, bad = 0;

    while ((c = getopt(argc, argv, "dht:g:")) != -1) {
        switch (c) {
            case 'd': g_debug = 1; break;
            case 't': train_file = optarg; break;
            case 'g': group = optarg; break;
            case 'h': usage(stdout, argv[0]); return 0;
            default:  usage(stderr, argv[0]); return 2;
        }
    }

    /* Absent or unreadable, the defaults in process.c apply; that is not an
     * error, so the program runs from anywhere with the tables beside it. */
    if (params_load("system.properties") != 0)
        debug("no system.properties; using built-in defaults");

    if (train_file) {
        bad = train(train_file, group) != 0;
    } else if (optind < argc) {
        int i;
        for (i = optind; i < argc; i++)
            if (score(argv[i]) != 0) bad = 1;
    } else {
        int any = 0;
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            any = 1;
            if (score(line) != 0) bad = 1;
        }
        if (!any) { usage(stderr, argv[0]); bad = 1; }
    }

    process_free();
    params_free();
    return bad ? 1 : 0;
}

#endif /* !UNIT_TEST */
