/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* csv.c: see csv.h. */

/* Before any header: glibc resolves <features.h> on the first standard header
 * it sees, and a feature macro defined after that does nothing. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L   /* getc_unlocked, flockfile */
#endif

#include "csv.h"
#include "utils.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>

/* One byte at a time is only affordable unlocked: getc takes and drops the
 * stream lock per character. The lock is taken once around the line instead. */
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
 * Left in place they attach to the first value. */
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
    /* The whole read path. A byte at a time costs about 20% against fgets on a
     * 121 MB file: 0.55 s to 0.67 s at 2,000,000 rows by 8 terms. It buys the
     * byte COUNT, without which a NUL in the last line of an unterminated file
     * looks like a short line. */
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

/* rtrim/ltrim's trimming against the end this loop knows, the comma it just
 * replaced: rtrim() would cost a strlen() per field to re-find it, ltrim() a
 * memmove() where advancing the pointer says the same. */
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
