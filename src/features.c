#include "features.h"

int features_extract(const Board *b, Feature out[MAX_ACTIVE_FEATURES]) {
    int count = 0;
    for (int color = 0; color < 2; color++) {
        int8_t sign = color == b->side ? 1 : -1;
        int mirror = color == WHITE ? 0 : 56; /* flips the rank */
        for (int type = 0; type < 6; type++) {
            for (Bitboard bb = b->pieces[color][type]; bb;) {
                int sq = pop_lsb(&bb) ^ mirror;
                out[count].index = (uint16_t)(type * 64 + sq);
                out[count].sign = sign;
                count++;
            }
        }
    }
    return count;
}
