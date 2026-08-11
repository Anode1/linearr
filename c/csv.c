/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* csv.c: see csv.h. */
#include "csv.h"
#include "utils.h"
#include "constants.h"

#include <string.h>

/* Excel's "CSV UTF-8" writes three invisible bytes at the start of the file.
 * They are not part of any field, and left in place they attach to the first
 * value: a case file scored from stdin then reported that group '001' was not
 * in a table where '001' plainly is, with nothing on screen to explain it. */
static void skip_bom(char *buf) {
    const unsigned char *u = (const unsigned char *)buf;
    if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
        memmove(buf, buf + 3, strlen(buf + 3) + 1);
}

int csv_next(FILE *fp, char *buf, size_t bufsz) {
    while (fgets(buf, (int)bufsz, fp)) {
        size_t n;
        skip_bom(buf);
        n = strcspn(buf, "\r\n");
        if (buf[n] == '\0' && !feof(fp))
            return -1;                      /* no line ending: it did not fit */
        buf[n] = '\0';
        rtrim(buf, ' ');
        ltrim(buf, ' ');
        if (buf[0] == '\0') continue;       /* blank lines are never data */
        if (buf[0] == '#') return 2;         /* the caller decides */
        return 1;
    }
    return 0;
}

/* The trimming is rtrim/ltrim's, done here against the end this loop already
 * knows: the comma it just replaced. Calling rtrim() meant a strlen() per
 * field to re-find that end, and ltrim() meant a memmove() to shift a field
 * left when advancing the pointer says the same thing. At 37 fields a row that
 * was 370 million strlen calls over a ten-million-row file. */
int csv_split(char *line, char **field, int maxf) {
    int n = 0;
    char *p = line;

    for (;;) {
        char *comma, *end;
        if (n >= maxf) return -1;
        comma = strchr(p, ',');
        if (comma) { *comma = '\0'; end = comma; }
        else       { end = p + strlen(p); }
        while (end > p && end[-1] == ' ') *--end = '\0';
        while (*p == ' ') p++;
        field[n++] = p;
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

int csv_comment_is_data_shaped(char *line, int want) {
    char *field[CSV_MAX_FIELDS];
    return csv_split(line, field, CSV_MAX_FIELDS) == want;
}
