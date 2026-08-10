/* Copyright (c) 2001-2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* utils.c: see utils.h. Both are O(n) and bounded by the string length. */
#include "utils.h"
#include <string.h>

void rtrim(char *s, char ch) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ch) s[--n] = '\0';
}

void ltrim(char *s, char ch) {
    char *p = s;
    while (*p == ch) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}