/* Copyright (C) 2001 Vasili Gavrilov. GNU GPL v2 or later. Hardened 2026. */
/* params.c -- see params.h. Parses "key = value" lines: blank lines and lines
 * starting with '#' are ignored; the key is split at the first '='; surrounding
 * spaces are trimmed. Values are heap copies owned by the table. */
#include "params.h"
#include "hash.h"
#include "utils.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARAMS_LINE_MAX 1024

static struct hash *params;

int params_load(const char *path) {
    char line[PARAMS_LINE_MAX];
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    if (!params) params = hash_create(100);

    while (fgets(line, sizeof line, fp)) {
        char *eq, *key, *val;
        line[strcspn(line, "\r\n")] = '\0';        /* drop the newline */
        if (line[0] == '#' || line[0] == '\0') continue;

        eq = strchr(line, '=');
        if (!eq) continue;                          /* not key=value */
        *eq = '\0';
        key = line;
        val = eq + 1;

        rtrim(key, ' '); ltrim(key, ' ');
        rtrim(val, ' '); ltrim(val, ' ');
        if (key[0] == '\0') continue;

        free(hash_put(params, key, xstrdup(val))); /* table copies the key;
                                                      free any value displaced
                                                      by a duplicate key */
    }
    fclose(fp);
    return 0;
}

const char *params_get(const char *key) {
    return params ? (const char *)hash_get(params, key) : NULL;
}

void params_free(void) {
    if (!params) return;
    hash_call(params, free);                        /* free the value copies */
    hash_delete(params);                            /* frees keys + table    */
    params = NULL;
}