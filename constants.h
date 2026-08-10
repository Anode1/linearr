/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* constants.h: tunable sizes, in one place. Every buffer in the program takes
 * its size from here, so the peak footprint is readable off this file and the
 * term ceiling in regress.h. None of them is a function of how much data you
 * feed the program; they are all functions of how wide one ROW is. */
#ifndef CONSTANTS_H
#define CONSTANTS_H

/* The term ceiling the line buffers are sized from. los.h defines the same
 * default under the same guard and remains where it is documented to live;
 * repeating it here lets the buffers be DERIVED from it rather than guessed,
 * and -DLOS_MAX_VARS=32 still reaches both, so the two cannot disagree. */
#ifndef LOS_MAX_VARS
#define LOS_MAX_VARS 256
#endif

/* The widest one field can be: a column name (LOS_NAME_MAX, 64) or a double in
 * shortest round-trip form (24), plus its comma. Names win. process.c asserts
 * at compile time that this really does cover LOS_NAME_MAX, so raising that
 * constant cannot quietly outgrow the buffers. */
#define CSV_FIELD_MAX     65

/* A row is a group, maybe a response, and one field per term, plus slack for
 * whatever padding a file carries. These were a flat 65536 each, which is four
 * times what the default ceiling can produce and twenty times what a 32-term
 * build can, and they are the whole of this program's stack requirement.
 * Derived, the ceiling is the only thing to set on a small target. */
#define CSV_LINE_MAX   ((LOS_MAX_VARS + 2) * CSV_FIELD_MAX + 1024)
#define MAX_INPUT      CSV_LINE_MAX   /* longest input line we read from stdin */
#define MAX_OUTPUT     (2 * CSV_LINE_MAX)
                               /* longest result process() may produce; a fitted
                                  header plus its coefficient row has to fit    */
#define CSV_MAX_FIELDS   512   /* most fields csv_split() will hand back        */
#define GROUP_MAX         32   /* longest group code                           */
#define RESOLVE_PATH_MAX 4096  /* longest path we will build looking for a file */

/* Bumped by hand at a release, and printed by --version, so a bug report can
 * name something. */
#define LINEARR_VERSION "0.3.0"

#endif /* CONSTANTS_H */
