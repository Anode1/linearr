/* Copyright (c) 2001-2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* hash.c: see hash.h. Djb/Berkeley string hash (chosen for low collisions on
 * dictionary words), separate chaining. Allocation is checked via xmalloc. */
#include "hash.h"
#include "common.h"

#include <string.h>
#include <limits.h>
#include <stdlib.h>

typedef struct hash_bucket {
    char               *key;
    void               *data;
    struct hash_bucket *next;
} hash_bucket;

struct hash {
    long          size;
    long          n;               /* entries */
    hash_bucket **table;
};

static unsigned long calc_hash(const char *str) {
    unsigned long h = 0;
    int c;
    while ((c = (unsigned char)*str++))
        h = (unsigned long)c + (h << 6) + (h << 16) - h;
    return h;
}

struct hash *hash_create(long size) {
    long i;
    struct hash *table;
    if (size <= 0) return NULL;
    table = xmalloc(sizeof *table);
    table->size  = size;
    table->n     = 0;
    table->table = xmalloc(sizeof(hash_bucket *) * (size_t)size);
    for (i = 0; i < size; i++) table->table[i] = NULL;
    return table;
}

void hash_delete(struct hash *table) {
    long i;
    if (!table) return;
    for (i = 0; i < table->size; i++) {
        hash_bucket *b = table->table[i];
        while (b) {
            hash_bucket *next = b->next;
            free(b->key);
            free(b);
            b = next;
        }
    }
    free(table->table);
    free(table);
}

/* Double the table when it is full, so N insertions cost O(N) and not O(N^2).
 * With a fixed 1024 buckets, loading 400,000 groups took 26 seconds: clean
 * 4x time for every 2x groups, which is the signature of chains that never stop
 * growing. Keys are moved, not recomputed, and nothing is reallocated. */
static void hash_grow(struct hash *table) {
    long i, newsize;
    hash_bucket **nt;

    /* Tested BEFORE the multiply: computing it and then looking is signed
     * overflow, which is undefined, and is the shape MISRA and the Power of
     * Ten both forbid. Unreachable at any real table size; still wrong. */
    if (table->size > LONG_MAX / 2) return;
    newsize = table->size * 2;
    nt = malloc(sizeof(hash_bucket *) * (size_t)newsize);
    if (!nt) return;                               /* growing is an optimisation */
    for (i = 0; i < newsize; i++) nt[i] = NULL;

    for (i = 0; i < table->size; i++) {
        hash_bucket *b = table->table[i];
        while (b) {
            hash_bucket *next = b->next;
            unsigned long v = calc_hash(b->key) % (unsigned long)newsize;
            b->next = nt[v];
            nt[v] = b;
            b = next;
        }
    }
    free(table->table);
    table->table = nt;
    table->size  = newsize;
}

void *hash_put(struct hash *table, const char *key, void *data) {
    unsigned long val;
    hash_bucket *b;

    if (table->n >= table->size) {                 /* average chain would exceed 1 */
        hash_grow(table);
    }
    val = calc_hash(key) % (unsigned long)table->size;

    for (b = table->table[val]; b; b = b->next)
        if (strcmp(key, b->key) == 0) {           /* replace */
            void *old = b->data;                  /* hand it back; caller owns it */
            b->data = data;
            return old;
        }


    b = xmalloc(sizeof *b);                        /* prepend new */
    b->key  = xstrdup(key);
    b->data = data;
    b->next = table->table[val];
    table->table[val] = b;
    table->n++;
    return NULL;                                   /* nothing was displaced */
}

void *hash_get(const struct hash *table, const char *key) {
    unsigned long val = calc_hash(key) % (unsigned long)table->size;
    hash_bucket *b;
    for (b = table->table[val]; b; b = b->next)
        if (strcmp(key, b->key) == 0) return b->data;
    return NULL;
}

void hash_call(struct hash *table, void (*func)(void *)) {
    long i;
    hash_bucket *b;
    for (i = 0; i < table->size; i++)
        for (b = table->table[i]; b; b = b->next)
            func(b->data);
}