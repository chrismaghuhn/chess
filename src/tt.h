#ifndef TT_H
#define TT_H

#include <stddef.h>

#include "types.h"

enum { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };

typedef struct {
    uint64_t key;
    Move move;
    int16_t score;
    int8_t depth;
    uint8_t bound;
} TTEntry;

typedef struct {
    TTEntry *entries;
    size_t mask; /* entry count - 1, a power of two */
} TT;

/* Allocates the largest power-of-two table that fits in mb megabytes. */
bool tt_init(TT *tt, size_t mb);
void tt_free(TT *tt);
void tt_clear(TT *tt);

/* The entry for this key, or NULL if the slot holds another position. */
const TTEntry *tt_probe(const TT *tt, uint64_t key);

/* Always replaces; keeps the old move if the new one is unknown. */
void tt_store(TT *tt, uint64_t key, Move move, int score, int depth, int bound);

/* Used slots per thousand, sampled from the first 1000 entries. */
int tt_hashfull(const TT *tt);

#endif
