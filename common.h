/* Copyright (C) 2001 Vasili Gavrilov. GNU GPL v2 or later. Hardened 2026. */
/* common.h -- safe primitives shared by every module: fatal exit, gated debug,
 * and checked allocation. Functions, not macros, so they type-check and are
 * greppable. See README.md for the style these encode. */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

extern int g_debug;                 /* set by -d; gates debug() */

/* argv[0], set by main. resolve.c uses it to find the files that shipped
 * beside the program, so it must be set before anything opens one. */
extern const char *g_prog;

/* CLI-level fatal: print to stderr and exit non-zero. Use only at the top
 * level (main/CLI); modules return -1 and let the caller decide. */
void die(const char *fmt, ...);

/* Trace to stderr, only when -d is on. Safe to sprinkle; off by default. */
void debug(const char *fmt, ...);

/* Checked allocation: never returns NULL (die on out-of-memory). */
void *xmalloc(size_t n);
char *xstrdup(const char *s);

#endif /* COMMON_H */