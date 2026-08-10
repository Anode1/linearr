/* Copyright (c) 2001-2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* utils.h: small bounded string helpers. */
#ifndef UTILS_H
#define UTILS_H

void rtrim(char *s, char ch);   /* strip a trailing run of ch, in place */
void ltrim(char *s, char ch);   /* strip a leading run of ch, in place  */

#endif /* UTILS_H */