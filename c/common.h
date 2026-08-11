/* Copyright (c) 2001-2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* common.h, the safe primitives shared by every module: fatal exit, gated
 * debug, and checked allocation. Functions, not macros, so they type-check
 * and are greppable. See README.md for the style these encode. */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

extern int g_debug;                 /* set by -d; gates debug() */

/* argv[0], set by main. resolve.c uses it to find the files that shipped
 * beside the program, so it must be set before anything opens one. */
extern const char *g_prog;

/* A variadic wrapper around vfprintf is invisible to -Wformat unless it says
 * so: without this attribute the compiler checks the format strings it sees
 * directly and none of the ones passed through here. Ten of them were wrong,
 * all "%ld" taking a long long, and neither -Wall -Wextra nor `make pedantic`
 * could see any of them. That is harmless where long is 64-bit and is not on
 * Windows, which is LLP64 and which this project builds for; two of the ten
 * put a "%s" after the mis-sized "%ld", so the vararg cursor desynchronises
 * and the "%s" dereferences half an integer.
 *
 * LINEARR_PRINTF is empty on compilers that do not know the attribute, so it
 * costs nothing and stays portable. */
#if defined(__GNUC__) || defined(__clang__)
#define LINEARR_PRINTF(fmt_idx, first_arg) \
    __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define LINEARR_PRINTF(fmt_idx, first_arg)
#endif

/* CLI-level fatal: print to stderr and exit non-zero. Use only at the top
 * level (main/CLI); modules return -1 and let the caller decide. */
void die(const char *fmt, ...) LINEARR_PRINTF(1, 2);

/* Trace to stderr, only when -d is on. Safe to sprinkle; off by default. */
void debug(const char *fmt, ...) LINEARR_PRINTF(1, 2);

/* Checked allocation: never returns NULL (die on out-of-memory). */
void *xmalloc(size_t n);
char *xstrdup(const char *s);

#endif /* COMMON_H */