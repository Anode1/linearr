/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* resolve.h: find a data file the program needs. A name is looked for in three
 * places, in order:
 *   1. relative to the current directory: your files win, always;
 *   2. beside the program itself: a build tree, or an unpacked release;
 *   3. <bindir>/../share/linearr: where `make install` puts them.
 * An absolute path is used as given. A symlinked binary is resolved first,
 * linking one into a bin directory being how people install one. */
#ifndef RESOLVE_H
#define RESOLVE_H

#include <stddef.h>

/* Fill out[outsz] with a readable path for `name` and return 0. Returns -1 if
 * no location has it, leaving in out a human-readable account of where it
 * looked, fit to go straight into the caller's error. */
int resolve_file(const char *name, char *out, size_t outsz);

/* The directory the running program sits in, or NULL if it cannot be worked
 * out. From g_prog (argv[0]): its own path if it has a '/', otherwise the first
 * match on PATH. */
const char *resolve_program_dir(void);

#endif /* RESOLVE_H */
