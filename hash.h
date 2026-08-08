/* Copyright (C) 2001 Vasili Gavrilov. GNU GPL v2 or later. Hardened 2026. */
/* hash.h -- a generic string -> void* hash table (separate chaining). The table
 * owns a copy of each key; the caller owns the stored data. */
#ifndef HASH_H
#define HASH_H

struct hash;

struct hash *hash_create(long size);
void         hash_delete(struct hash *table);              /* frees keys + table */

/* Insert or replace. Returns the PREVIOUS data for key, or NULL if there was
 * none, so the caller can free what it displaced. The table owns keys, never
 * data: returning the new pointer instead silently dropped the old one on the
 * floor, and every project copying this template inherited the leak. */
void *hash_put(struct hash *table, const char *key, void *data);
void *hash_get(struct hash *table, const char *key);       /* NULL if absent */

/* Remove key; returns its data (caller frees) or NULL. */
void *hash_delete_entry(struct hash *table, const char *key);

/* Call func on each stored datum (e.g. free). */
void hash_call(struct hash *table, void (*func)(void *));

#endif /* HASH_H */