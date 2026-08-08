/* Copyright (C) 2001 Vasili Gavrilov. GNU GPL v2 or later. Hardened 2026. */
/* utils.h -- small bounded string helpers. */
#ifndef UTILS_H
#define UTILS_H

void rtrim(char *s, char ch);   /* strip a trailing run of ch, in place */
void ltrim(char *s, char ch);   /* strip a leading run of ch, in place  */

#endif /* UTILS_H */