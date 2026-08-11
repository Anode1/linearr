/* Copyright (c) 2001-2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* common.h, the primitives shared by every module: fatal exit, gated debug,
 * checked allocation. Functions, not macros: they type-check and grep. */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

extern int g_debug;                 /* set by -d; gates debug() */

/* argv[0], set by main. resolve.c finds the files that shipped beside the
 * program with it, so it must be set before anything opens one. */
extern const char *g_prog;

/* A variadic wrapper around vfprintf is invisible to -Wformat without this
 * attribute. A "%ld" taking a long long is harmless where long is 64-bit and is
 * not on Windows, which is LLP64 and which this builds for: a "%s" after the
 * mis-sized "%ld" finds the vararg cursor desynchronised and dereferences half
 * an integer. Empty on compilers that do not know the attribute. */
#if defined(__GNUC__) || defined(__clang__)
#define LINEARR_PRINTF(fmt_idx, first_arg) \
    __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define LINEARR_PRINTF(fmt_idx, first_arg)
#endif

/* CLI-level fatal: print to stderr and exit non-zero. Top level only; modules
 * return -1 and let the caller decide. */
void die(const char *fmt, ...) LINEARR_PRINTF(1, 2);

/* Trace to stderr, only when -d is on. Safe to sprinkle; off by default. */
void debug(const char *fmt, ...) LINEARR_PRINTF(1, 2);

/* Checked allocation: never returns NULL (die on out-of-memory). */
void *xmalloc(size_t n);
char *xstrdup(const char *s);

#endif /* COMMON_H */