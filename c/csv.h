/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* csv.h, the minimum CSV a training file needs: read a line, split it on
 * commas. Splitting is in place and allocates nothing, so a caller streams a
 * file of any size through one stack buffer. No quoting and no embedded commas:
 * the files this reads are numeric tables, and a reader that pretends to handle
 * RFC 4180 while not handling it is worse than one that says it does not. */
#ifndef CSV_H
#define CSV_H

#include <stdio.h>

/* Why a line was refused. One set of codes for both readers below, so the same
 * condition cannot be described two ways in two files: it was, and the two
 * sentences disagreed about the byte limit. */
#define CSV_ERR_TOO_LONG (-1)
#define CSV_ERR_NUL      (-2)
#define CSV_ERR_CR       (-3)
#define CSV_ERR_IO       (-4)

/* The refusal as a clause, to follow the file's name: "%s %s". bufsz is the
 * buffer the caller read into, because the byte limit belongs in the sentence. */
const char *csv_line_error(int rv, size_t bufsz);

/* Read one physical line into buf (capacity bufsz, at least 2), stripping the
 * trailing LF and a CR before it. Returns the stored length, or a CSV_ERR_*
 * code; at end of file it returns 0 with *eof set, which an empty line does not.
 *
 * A byte loop rather than fgets, because the byte COUNT is the whole answer and
 * fgets cannot report it. A NUL in a line makes the buffer's C string end early,
 * so a NUL in the LAST line of a file with no trailing newline was
 * indistinguishable from a short line: the row was truncated at the NUL and
 * fitted, exit 0. The line is always consumed to its end, so a caller that
 * refuses one line can go on to the next.
 *
 * An interior CR is refused rather than cut at: a CR-only file (old Mac) is one
 * line to every reader on a modern system, and cutting there quietly read the
 * first record and threw the rest of the file away. */
int csv_read_line(FILE *fp, char *buf, size_t bufsz, int *eof);

/* Read the next line into buf (capacity bufsz), stripping the trailing CR/LF.
 * Blank lines are skipped; they are never data. A line beginning with '#' is
 * NOT skipped; it is returned with a 2, so the caller can look at it.
 *
 * That distinction matters: silently swallowing '#' lines meant a group code
 * beginning with '#' vanished from a table without a word, and `--terms` then
 * reported one group for a two-group file. A reader that cannot see a comment
 * cannot tell a comment from a row it has misread.
 *
 * Returns 1 on a data line, 2 on a comment line, 0 at end of file, or one of
 * the CSV_ERR_* codes above (all negative, so `> 0` still means "a line"). */
int csv_next(FILE *fp, char *buf, size_t bufsz);

/* Split line in place on ',' into at most maxf pointers in field[]. Surrounding
 * spaces are trimmed. Returns the field count, or -1 if the line holds more
 * than maxf fields. An empty field yields an empty string, never NULL. */
int csv_split(char *line, char **field, int maxf);

/* A comment line (csv_next returned 2) that splits into exactly `want` fields
 * has the shape of a data row: almost certainly data whose group code starts
 * with '#', not a comment. Every reader that skips comments must ask this
 * first and refuse on yes, for the reason csv_next's contract gives: a
 * swallowed row is a group short with nothing on screen. Splits in place, so
 * the line is consumed by the check. */
int csv_comment_is_data_shaped(char *line, int want);

#endif /* CSV_H */
