/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* csv.h -- the minimum CSV a training file needs: read a line, split it on
 * commas. Splitting is in place and allocates nothing, so a caller streams a
 * file of any size through one stack buffer. No quoting and no embedded commas:
 * the files this reads are numeric tables, and a reader that pretends to handle
 * RFC 4180 while not handling it is worse than one that says it does not. */
#ifndef CSV_H
#define CSV_H

#include <stdio.h>

/* Read the next line into buf (capacity bufsz), stripping the trailing CR/LF.
 * Blank lines are skipped -- they are never data. A line beginning with '#' is
 * NOT skipped; it is returned with a 2, so the caller can look at it.
 *
 * That distinction matters: silently swallowing '#' lines meant a group code
 * beginning with '#' vanished from a table without a word, and `--terms` then
 * reported one group for a two-group file. A reader that cannot see a comment
 * cannot tell a comment from a row it has misread.
 *
 * Returns 1 on a data line, 2 on a comment line, 0 at end of file, -1 if a line
 * did not fit in buf (or held a NUL byte, which looks the same from here). */
int csv_next(FILE *fp, char *buf, size_t bufsz);

/* Split line in place on ',' into at most maxf pointers in field[]. Surrounding
 * spaces are trimmed. Returns the field count, or -1 if the line holds more
 * than maxf fields. An empty field yields an empty string, never NULL. */
int csv_split(char *line, char **field, int maxf);

#endif /* CSV_H */
