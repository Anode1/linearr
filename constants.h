/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* constants.h -- tunable sizes, in one place. Every buffer in the program takes
 * its size from here, so the peak footprint is readable off this file and the
 * term ceiling in regress.h. None of them is a function of how much data you
 * feed the program; they are all functions of how wide one ROW is. */
#ifndef CONSTANTS_H
#define CONSTANTS_H

/* A row is a group, maybe a response, and one field per term. At the default
 * ceiling of 256 terms with names up to 64 characters, a header line is about
 * 16 KB, so these are sized for that with room over. Shrink them together with
 * REGRESS_MAX_VARS on a small target. */
#define CSV_LINE_MAX   65536   /* longest line csv_next() will read            */
#define MAX_INPUT      65536   /* longest input line we read from stdin        */
#define MAX_OUTPUT     65536   /* longest result process() may produce; a fitted
                                  header plus its coefficient row has to fit    */
#define CSV_MAX_FIELDS   512   /* most fields csv_split() will hand back        */
#define GROUP_MAX         32   /* longest group code                           */
#define RESOLVE_PATH_MAX 4096  /* longest path we will build looking for a file */

/* Bumped by hand at a release, and printed by --version, so a bug report can
 * name something. */
#define LINEARR_VERSION "0.1.0"

#endif /* CONSTANTS_H */
