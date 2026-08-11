/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* resolve.c: see resolve.h. Fixed stack buffers; the result is copied out. */
/* _XOPEN_SOURCE 700, not _POSIX_C_SOURCE 200809L: glibc guards realpath() with
 * __USE_XOPEN_EXTENDED, which only _XOPEN_SOURCE sets, and an implicitly
 * declared realpath returns int, truncating the pointer to 32 bits. 700 implies
 * POSIX.1-2008, so access() and stat() stay declared. */
#define _XOPEN_SOURCE 700

#include "resolve.h"
#include "common.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* The last directory separator. Windows argv[0] is C:\...\linearr.exe and
 * _fullpath returns backslashes, so '/' alone finds none and the whole path
 * reads as a bare file name. Checking both is safe on POSIX. */
static char *last_sep(const char *p) {
    char *a = strrchr(p, '/');
#ifdef _WIN32
    char *b = strrchr(p, '\\');
    if (!a || (b && b > a)) a = b;
#endif
    return a;
}

/* Windows has these under other names, and lacks realpath; _fullpath stands in. */
#ifdef _WIN32
#include <stdlib.h>
#define PATH_SEP ';'                 /* and a PATH entry contains a colon    */
#define X_OK_MODE 0                  /* _access has no execute mode; 0 is
                                        "does it exist", which is what the
                                        PATH walk is actually asking          */
static char *realpath(const char *path, char *out) {
    return _fullpath(out, path, RESOLVE_PATH_MAX);
}
#else
#define PATH_SEP ':'
#define X_OK_MODE X_OK
#endif

/* Readable AND regular: a FIFO given to -c or -t blocks forever on open. */
static int readable(const char *path) {
    struct stat st;
    if (access(path, R_OK) != 0) return 0;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) != 0;
}

/* Where the program lives. Worked out once: it walks PATH and cannot change. */
const char *resolve_program_dir(void) {
    static char dir[RESOLVE_PATH_MAX];
    static int  tried;
    const char *slash, *path;
    size_t n;

    if (tried) return dir[0] ? dir : NULL;
    tried = 1;
    dir[0] = '\0';

    if (!g_prog || !g_prog[0]) return NULL;

    slash = last_sep(g_prog);
    if (slash) {                                  /* invoked by a path */
        /* Through the symlink: a binary linked into a bin directory leaves
         * dirname(argv[0]) pointing where the data files are not. */
        char real[RESOLVE_PATH_MAX];
        const char *use = g_prog;
        if (realpath(g_prog, real) != NULL) use = real;

        slash = last_sep(use);
        if (!slash) return NULL;
        n = (size_t)(slash - use);
        if (n == 0) n = 1;                        /* "/prog" -> "/" */
        if (n >= sizeof dir) return NULL;
        memcpy(dir, use, n);
        dir[n] = '\0';
        return dir;
    }

    /* Bare name, the installed case: the first PATH entry holding an executable
     * of that name is the one the shell ran. */
    path = getenv("PATH");
    if (!path) return NULL;
    while (*path) {
        const char *end = strchr(path, PATH_SEP);
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char cand[RESOLVE_PATH_MAX];

        if (len == 0) { len = 1; path = "."; }    /* an empty entry means "." */
        if (len < sizeof dir) {
            int w = snprintf(cand, sizeof cand, "%.*s/%s", (int)len, path, g_prog);
            if (w > 0 && (size_t)w < sizeof cand && access(cand, X_OK_MODE) == 0) {
                char real[RESOLVE_PATH_MAX];
                const char *sl;
                if (realpath(cand, real) != NULL && (sl = last_sep(real)) != NULL) {
                    size_t rn = (size_t)(sl - real);
                    if (rn == 0) rn = 1;
                    if (rn < sizeof dir) { memcpy(dir, real, rn); dir[rn] = '\0'; return dir; }
                }
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

    /* An absolute path on Windows is "C:\..." as well as "/...". */
#ifdef _WIN32
    if (name[0] == '/' || name[0] == '\\' ||
        (name[0] != '\0' && name[1] == ':')) {
#else
    if (name[0] == '/') {                         /* absolute: as given */
#endif
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
        /* 3. the installed layout: <bindir>/../share/linearr/<name>, so
         * `make install` can put the binary on PATH and its data elsewhere. */
        w = snprintf(cand, sizeof cand, "%s/../share/linearr/%s", dir, name);
        if (w > 0 && (size_t)w < sizeof cand && readable(cand)) {
            w = snprintf(out, outsz, "%s", cand);
            if (w < 0 || (size_t)w >= outsz) return -1;
            debug("resolve: %s in the installed share directory", name);
            return 0;
        }
    }

    /* Not found: hand back where we looked, for the caller's error. */
    if (dir && strcmp(dir, ".") != 0)
        (void)snprintf(out, outsz, "'%s' (looked in the current directory, in %s, and "
                 "in %s/../share/linearr)", name, dir, dir);
    else
        (void)snprintf(out, outsz, "'%s' (looked in the current directory)", name);
    return -1;
}
