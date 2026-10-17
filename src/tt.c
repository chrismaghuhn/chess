#include "tt.h"

#include <stdlib.h>
#include <string.h>

bool tt_init(TT *tt, size_t mb) {
    size_t count = 1;
    while (count * 2 * sizeof(TTEntry) <= mb * 1024 * 1024)
        count *= 2;

    TTEntry *entries = calloc(count, sizeof(TTEntry));
    if (!entries)
        return false;
    tt->entries = entries;
    tt->mask = count - 1;
    return true;
}

void tt_free(TT *tt) {
    free(tt->entries);
    tt->entries = NULL;
    tt->mask = 0;
}

void tt_clear(TT *tt) {
    memset(tt->entries, 0, (tt->mask + 1) * sizeof(TTEntry));
}

const TTEntry *tt_probe(const TT *tt, uint64_t key) {
    const TTEntry *e = &tt->entries[key & tt->mask];
    return e->key == key && e->bound != BOUND_NONE ? e : NULL;
}

void tt_store(TT *tt, uint64_t key, Move move, int score, int depth, int bound) {
    TTEntry *e = &tt->entries[key & tt->mask];
    if (move || e->key != key)
        e->move = move;
    e->key = key;
    e->score = (int16_t)score;
    e->depth = (int8_t)depth;
    e->bound = (uint8_t)bound;
}

int tt_hashfull(const TT *tt) {
    size_t sample = tt->mask + 1 < 1000 ? tt->mask + 1 : 1000;
    int used = 0;
    for (size_t i = 0; i < sample; i++)
        used += tt->entries[i].bound != BOUND_NONE;
    return (int)(used * 1000 / sample);
}
