#ifndef ATTACKS_H
#define ATTACKS_H

#include "bitboard.h"

extern Bitboard knight_attacks[64];
extern Bitboard king_attacks[64];
extern Bitboard pawn_attacks[2][64]; /* [color of the pawn][square] */

typedef struct {
    Bitboard mask;            /* squares whose occupancy matters */
    uint64_t magic;
    const Bitboard *attacks;  /* table indexed by the magic product */
    unsigned shift;
} Magic;

extern Magic bishop_magics[64];
extern Magic rook_magics[64];
extern Bitboard between_bb[64][64]; /* squares strictly between two aligned squares, else 0 */

void attacks_init(void);

static inline Bitboard bishop_attacks(int sq, Bitboard occupied) {
    const Magic *m = &bishop_magics[sq];
    return m->attacks[((occupied & m->mask) * m->magic) >> m->shift];
}

static inline Bitboard rook_attacks(int sq, Bitboard occupied) {
    const Magic *m = &rook_magics[sq];
    return m->attacks[((occupied & m->mask) * m->magic) >> m->shift];
}

/* Ray walking, used to build the magic tables and to test them. */
Bitboard bishop_attacks_slow(int sq, Bitboard occupied);
Bitboard rook_attacks_slow(int sq, Bitboard occupied);

static inline Bitboard queen_attacks(int sq, Bitboard occupied) {
    return bishop_attacks(sq, occupied) | rook_attacks(sq, occupied);
}

#endif
