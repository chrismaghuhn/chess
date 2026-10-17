#include "zobrist.h"

uint64_t zobrist_piece[12][64];
uint64_t zobrist_castling[16];
uint64_t zobrist_ep[8];
uint64_t zobrist_side;

/* splitmix64 with a fixed seed, so hashes are the same on every run. */
static uint64_t next_random(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void zobrist_init(void) {
    uint64_t state = 0x5C4AC4E55C4AC4E5ULL;

    for (int piece = 0; piece < 12; piece++)
        for (int sq = 0; sq < 64; sq++)
            zobrist_piece[piece][sq] = next_random(&state);

    /* No castling rights hashes to 0 so the empty state adds nothing. */
    zobrist_castling[0] = 0;
    for (int i = 1; i < 16; i++)
        zobrist_castling[i] = next_random(&state);

    for (int f = 0; f < 8; f++)
        zobrist_ep[f] = next_random(&state);

    zobrist_side = next_random(&state);
}
