/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* csv.h, the minimum CSV a training file needs: read a line, split it on commas.
 * Splitting is in place and allocates nothing, so a caller streams a file of any
 * size through one stack buffer. No quoting, no embedded commas, not RFC 4180. */
#ifndef CSV_H
#define CSV_H

#include <stdio.h>

/* Why a line was refused. One set of codes for both readers below. */
#define CSV_ERR_TOO_LONG (-1)
#define CSV_ERR_NUL      (-2)
#define CSV_ERR_CR       (-3)
#define CSV_ERR_IO       (-4)

/* The refusal as a clause, to follow the file's name: "%s %s". bufsz is the
 * buffer the caller read into, its limit being part of the sentence. */
const char *csv_line_error(int rv, size_t bufsz);

/* Read one physical line into buf (capacity bufsz, at least 2), stripping the
 * trailing LF and a CR before it. Returns the stored length, or a CSV_ERR_*
 * code; at end of file it returns 0 with *eof set, which an empty line does not.
 * The line is always consumed to its end, so a caller may refuse one and read
 * the next. A byte loop, not fgets: a NUL ends the buffer's C string early, so
 * without the byte count, which fgets cannot report, a NUL in the LAST line of
 * an unterminated file looks like a short line. An interior CR is refused, not
 * cut at: a CR-only file (old Mac) is one line to every modern reader. */
int csv_read_line(FILE *fp, char *buf, size_t bufsz, int *eof);

/* Read the next line into buf (capacity bufsz), stripping the trailing CR/LF.
 * Blank lines are skipped, never being data. A line beginning with '#' is NOT
 * skipped but returned with a 2: a group code may begin with '#', and swallowing
 * such a line drops a row silently. Returns 1 on a data line, 2 on a comment, 0
 * at end of file, or a CSV_ERR_* code (all negative, so `> 0` means a line). */
int csv_next(FILE *fp, char *buf, size_t bufsz);

/* Split line in place on ',' into at most maxf pointers in field[], trimming
 * surrounding spaces. Returns the field count, or -1 past maxf fields. An empty
 * field yields an empty string, never NULL. */
int csv_split(char *line, char **field, int maxf);

/* A comment line (csv_next returned 2) splitting into exactly `want` fields has
 * the shape of a data row: data whose group code starts with '#'. Every reader
 * that skips comments must ask first and refuse on yes. Splits in place. */
int csv_comment_is_data_shaped(char *line, int want);

#endif /* CSV_H */
