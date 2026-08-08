/* Copyright (C) 2001 Vasili Gavrilov. GNU GPL v2 or later. Hardened 2026. */
/* params.h -- a tiny key=value config store, backed by the hash table. Load a
 * .properties file once, then look up keys. This is where a project adds its
 * configurable knobs: put them in the file, read them with params_get. */
#ifndef PARAMS_H
#define PARAMS_H

int         params_load(const char *path);   /* 0 ok, -1 if the file won't open */
const char *params_get(const char *key);     /* NULL if absent */
void        params_free(void);

#endif /* PARAMS_H */