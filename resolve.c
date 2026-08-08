/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* resolve.c -- see resolve.h. Everything here is a fixed stack buffer; the
 * result is copied into the caller's. */
#define _POSIX_C_SOURCE 200809L  /* access */

#include "resolve.h"
#include "common.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* Readable AND a regular file. Without the second half, naming a FIFO as
 * coef.file or as -t's argument made the program block forever on open with no
 * output and no diagnostic -- indistinguishable from a hang. Directories,
 * /dev/zero and unreadable files were already handled; the FIFO was the one
 * input that could take the process away and not give it back. */
static int readable(const char *path) {
    struct stat st;
    if (access(path, R_OK) != 0) return 0;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) != 0;
}

/* Where the program itself lives. Worked out once and remembered, because it
 * involves walking PATH and nothing about it changes during a run. */
const char *resolve_program_dir(void) {
    static char dir[RESOLVE_PATH_MAX];
    static int  tried;
    const char *slash, *path;
    size_t n;

    if (tried) return dir[0] ? dir : NULL;
    tried = 1;
    dir[0] = '\0';

    if (!g_prog || !g_prog[0]) return NULL;

    slash = strrchr(g_prog, '/');
    if (slash) {                                  /* invoked by a path */
        n = (size_t)(slash - g_prog);
        if (n == 0) n = 1;                        /* "/prog" -> "/" */
        if (n >= sizeof dir) return NULL;
        memcpy(dir, g_prog, n);
        dir[n] = '\0';
        return dir;
    }

    /* Invoked by bare name: the installed case. Walk PATH for the first entry
     * that holds an executable of that name -- the same one the shell ran. */
    path = getenv("PATH");
    if (!path) return NULL;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char cand[RESOLVE_PATH_MAX];

        if (len == 0) { len = 1; path = "."; }    /* an empty entry means "." */
        if (len < sizeof dir) {
            int w = snprintf(cand, sizeof cand, "%.*s/%s", (int)len, path, g_prog);
            if (w > 0 && (size_t)w < sizeof cand && access(cand, X_OK) == 0) {
                memcpy(dir, path, len);
                dir[len] = '\0';
                return dir;
            }
        }
        if (!end) break;
        path = end + 1;
    }
    return NULL;
}

int resolve_file(const char *name, char *out, size_t outsz) {
    const char *dir;
    int w;

    if (name[0] == '/') {                         /* absolute: as given */
        w = snprintf(out, outsz, "%s", name);
        if (w < 0 || (size_t)w >= outsz) return -1;
        return readable(out) ? 0 : -1;
    }

    w = snprintf(out, outsz, "%s", name);         /* 1. the current directory */
    if (w < 0 || (size_t)w >= outsz) return -1;
    if (readable(out)) { debug("resolve: %s in the current directory", name); return 0; }

    dir = resolve_program_dir();                  /* 2. beside the program */
    if (dir) {
        char cand[RESOLVE_PATH_MAX];
        w = snprintf(cand, sizeof cand, "%s/%s", dir, name);
        if (w > 0 && (size_t)w < sizeof cand && readable(cand)) {
            w = snprintf(out, outsz, "%s", cand);
            if (w < 0 || (size_t)w >= outsz) return -1;
            debug("resolve: %s beside the program", name);
            return 0;
        }
    }

    /* Not found: hand back where we looked, so the caller's error can say it
     * instead of leaving the user to guess. */
    if (dir) snprintf(out, outsz, "'%s' (looked in the current directory and in %s)",
                      name, dir);
    else     snprintf(out, outsz, "'%s' (looked in the current directory)", name);
    return -1;
}
