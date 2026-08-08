/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* constants.h -- tunable sizes, in one place. Every buffer in the program takes
 * its size from here, so the peak footprint is readable off this file. */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#define MAX_INPUT       4096   /* longest input line we read from stdin        */
#define MAX_OUTPUT      4096   /* longest result process() may produce; a fitted
                                  header plus its coefficient row has to fit,
                                  at LOS_MAX_VARS columns of LOS_NAME_MAX       */
#define CSV_LINE_MAX    4096   /* longest line csv_next() will read            */
#define CSV_MAX_FIELDS    64   /* most fields csv_split() will hand back       */
#define GROUP_MAX         16   /* longest group code ("001", "999")            */

#endif /* CONSTANTS_H */
