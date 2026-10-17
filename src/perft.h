#ifndef PERFT_H
#define PERFT_H

#include "board.h"

/* Counts the leaf nodes of the legal move tree to the given depth. */
uint64_t perft(const Board *b, int depth);

/* Like perft, but prints the count below each root move ("e2e4: 20"). */
uint64_t perft_divide(const Board *b, int depth, FILE *out);

#endif
