/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* constants.h: tunable sizes. Every buffer takes its size from here, so the peak
 * footprint is readable off this file and the term ceiling in regress.h. All are
 * functions of how wide one ROW is, none of how much data arrives. */
#ifndef CONSTANTS_H
#define CONSTANTS_H

/* The term ceiling the line buffers are sized from. los.h defines the same
 * default under the same guard, so the buffers are derived, not guessed, and
 * -DLOS_MAX_VARS=32 reaches both. */
#ifndef LOS_MAX_VARS
#define LOS_MAX_VARS 256
#endif

/* The widest one field can be: a column name (LOS_NAME_MAX, 64) or a double in
 * shortest round-trip form (24), plus its comma. Names win. process.c asserts
 * at compile time that this covers LOS_NAME_MAX. */
#define CSV_FIELD_MAX     65

/* A row is a group, maybe a response, one field per term, plus slack. These
 * buffers are the whole stack requirement and derive from the term ceiling,
 * which is the only thing to set on a small target. */
#define CSV_LINE_MAX   ((LOS_MAX_VARS + 2) * CSV_FIELD_MAX + 1024)
/* LINEARR_-prefixed because MAX_INPUT is POSIX's: <limits.h> defines it as 255
 * under _XOPEN_SOURCE or _POSIX_C_SOURCE, and resolve.c defines
 * _XOPEN_SOURCE 700. */
#define LINEARR_MAX_INPUT   CSV_LINE_MAX  /* longest line we read from stdin  */
#define LINEARR_MAX_OUTPUT  (2 * CSV_LINE_MAX)
                               /* longest result process() may produce; a fitted
                                  header plus its coefficient row has to fit    */
#define CSV_MAX_FIELDS   512   /* most fields csv_split() will hand back        */
#define GROUP_MAX         32   /* longest group code                           */
#define RESOLVE_PATH_MAX 4096  /* longest path we will build looking for a file */

#endif /* CONSTANTS_H */
