/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* csv.c: see csv.h. */

/* Before any header: glibc resolves <features.h> on the first standard header
 * it sees, and a feature macro defined after that does nothing. process.c had
 * exactly this and lost its monotonic clock for it. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L   /* getc_unlocked, flockfile */
#endif

#include "csv.h"
#include "utils.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>

/* One byte at a time is only affordable unlocked: getc takes and drops the
 * stream lock per character, and at 121 MB that is the fit. The lock is taken
 * once around the line instead. */
#if defined(_WIN32)
#define CSV_GETC(fp)    _getc_nolock(fp)
#define CSV_LOCK(fp)    ((void)0)
#define CSV_UNLOCK(fp)  ((void)0)
#else
#define CSV_GETC(fp)    getc_unlocked(fp)
#define CSV_LOCK(fp)    flockfile(fp)
#define CSV_UNLOCK(fp)  funlockfile(fp)
#endif

/* Excel's "CSV UTF-8" writes three invisible bytes at the start of the file.
 * They are not part of any field, and left in place they attach to the first
 * value: a case file scored from stdin then reported that group '001' was not
 * in a table where '001' plainly is, with nothing on screen to explain it. */
static void skip_bom(char *buf) {
    const unsigned char *u = (const unsigned char *)buf;
    if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
        memmove(buf, buf + 3, strlen(buf + 3) + 1);
}

const char *csv_line_error(int rv, size_t bufsz) {
    static char msg[160];
    switch (rv) {
    case CSV_ERR_NUL:
        return "has a line holding a NUL byte, which is not the text this reads";
    case CSV_ERR_CR:
        return "has a bare CR inside a line: this reads LF or CRLF endings, and "
               "cutting the line there would have thrown the rest of the file away";
    case CSV_ERR_IO:
        return "could not be read to the end";
    default:
        (void)snprintf(msg, sizeof msg, "has a line longer than %lu bytes",
                       (unsigned long)(bufsz - 1));
        return msg;
    }
}

int csv_read_line(FILE *fp, char *buf, size_t bufsz, int *eof) {
    size_t n = 0;
    int    c, nul = 0, over = 0;

    *eof = 0;
    CSV_LOCK(fp);
    /* This is the program's whole read path, and one byte at a time costs about
     * 20% against fgets on a 121 MB file: 0.55 s to 0.67 s at 2,000,000 rows by
     * 8 terms. What it buys is the byte COUNT, which fgets cannot report and
     * without which a NUL in the last line of an unterminated file is
     * indistinguishable from a short line. Hoisting this bounds test out of the
     * loop was measured and changed nothing; the cost is getc, so the way to
     * spend it back would be bulk reads and a reader that owns its own buffer. */
    while ((c = CSV_GETC(fp)) != EOF && c != '\n') {
        if (c == '\0') nul = 1;
        if (n + 1 < bufsz) buf[n++] = (char)c;
        else               over = 1;
    }
    CSV_UNLOCK(fp);

    if (c == EOF && n == 0 && !nul && !over) {
        buf[0] = '\0';
        if (ferror(fp)) return CSV_ERR_IO;
        *eof = 1;
        return 0;
    }
    if (n > 0 && buf[n - 1] == '\r') n--;    /* CRLF, the ordinary case */
    buf[n] = '\0';
    if (nul)  return CSV_ERR_NUL;
    if (over) return CSV_ERR_TOO_LONG;
    if (memchr(buf, '\r', n) != NULL) return CSV_ERR_CR;
    return (int)n;
}

int csv_next(FILE *fp, char *buf, size_t bufsz) {
    for (;;) {
        int eof, rv = csv_read_line(fp, buf, bufsz, &eof);
        if (eof) return 0;
        if (rv < 0) return rv;
        skip_bom(buf);
        rtrim(buf, ' ');
        ltrim(buf, ' ');
        if (buf[0] == '\0') continue;       /* blank lines are never data */
        if (buf[0] == '#') return 2;         /* the caller decides */
        return 1;
    }
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
