#ifndef ATTACKS_H
#define ATTACKS_H

#include "bitboard.h"

extern Bitboard knight_attacks[64];
extern Bitboard king_attacks[64];
extern Bitboard pawn_attacks[2][64]; /* [color of the pawn][square] */

void attacks_init(void);

Bitboard bishop_attacks(int sq, Bitboard occupied);
Bitboard rook_attacks(int sq, Bitboard occupied);

static inline Bitboard queen_attacks(int sq, Bitboard occupied) {
    return bishop_attacks(sq, occupied) | rook_attacks(sq, occupied);
}

#endif
