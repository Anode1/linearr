/* Copyright (c) 2001-2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* hash.h: a generic string -> void* hash table (separate chaining). The table
 * owns a copy of each key; the caller owns the stored data. */
#ifndef HASH_H
#define HASH_H

struct hash;

/* size is the initial bucket count; the table doubles as it fills, so this is a
 * starting point rather than a limit. */
struct hash *hash_create(long size);
void         hash_delete(struct hash *table);              /* frees keys + table */

/* Insert or replace. Returns the PREVIOUS data for key, or NULL if there was
 * none, so the caller can free what it displaced. The table owns keys, never
 * data: returning the new pointer instead silently dropped the old one on the
 * floor, and every project copying this template inherited the leak. */
void *hash_put(struct hash *table, const char *key, void *data);
void *hash_get(const struct hash *table, const char *key); /* NULL if absent */

/* Call func on each stored datum (e.g. free). */
void hash_call(struct hash *table, void (*func)(void *));

#endif /* HASH_H */