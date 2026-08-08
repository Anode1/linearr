/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* resolve.h -- find a data file the program needs.
 *
 * A tool that only works from its own source directory is not installed, it is
 * merely built. `linearr` used to fail from anywhere else, because it opened
 * "conf/coefficients.csv" relative to the current directory and nowhere else.
 *
 * So a name is looked for in two places, in this order:
 *   1. relative to the current directory -- your files win, always;
 *   2. beside the program itself -- the defaults that shipped with it.
 * An absolute path is used as given. */
#ifndef RESOLVE_H
#define RESOLVE_H

#include <stddef.h>

/* Fill out[outsz] with a readable path for `name`, and return 0. Returns -1 if
 * neither location has it, in which case out gets a human-readable account of
 * where it looked -- the caller can put that straight in the error. */
int resolve_file(const char *name, char *out, size_t outsz);

/* The directory the running program sits in, or NULL if it cannot be worked
 * out. Derived from g_prog (argv[0]): a path if it has a '/', otherwise the
 * first match on PATH, which is the case for an installed binary. */
const char *resolve_program_dir(void);

#endif /* RESOLVE_H */
