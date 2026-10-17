#ifndef BITBOARD_H
#define BITBOARD_H

#include "types.h"

static inline Bitboard bit(int sq) { return 1ULL << sq; }
static inline int lsb(Bitboard b) { return __builtin_ctzll(b); }
static inline int msb(Bitboard b) { return 63 - __builtin_clzll(b); }
static inline int popcount(Bitboard b) { return __builtin_popcountll(b); }

/* Returns the lowest set square and clears it. */
static inline int pop_lsb(Bitboard *b) {
    int sq = lsb(*b);
    *b &= *b - 1;
    return sq;
}

#endif
