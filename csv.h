/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* csv.h -- the minimum CSV a training file needs: read a line, split it on
 * commas. Splitting is in place and allocates nothing, so a caller streams a
 * file of any size through one stack buffer. No quoting and no embedded commas:
 * the files this reads are numeric tables, and a reader that pretends to handle
 * RFC 4180 while not handling it is worse than one that says it does not. */
#ifndef CSV_H
#define CSV_H

#include <stdio.h>

/* Read the next content line into buf (capacity bufsz): blank lines and lines
 * beginning with '#' are skipped, and the trailing CR/LF is stripped. Returns 1
 * on a line, 0 at end of file, -1 if a line did not fit in buf. */
int csv_next(FILE *fp, char *buf, size_t bufsz);

/* Split line in place on ',' into at most maxf pointers in field[]. Surrounding
 * spaces are trimmed. Returns the field count, or -1 if the line holds more
 * than maxf fields. An empty field yields an empty string, never NULL. */
int csv_split(char *line, char **field, int maxf);

#endif /* CSV_H */
