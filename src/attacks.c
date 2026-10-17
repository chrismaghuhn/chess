#include "attacks.h"

Bitboard knight_attacks[64];
Bitboard king_attacks[64];
Bitboard pawn_attacks[2][64];

/*
 * Sliding pieces use plain rays for now: walk to the first blocker and cut
 * the ray behind it. Magic bitboards can replace this once perft is correct.
 */
enum { NORTH, NORTH_EAST, EAST, SOUTH_EAST, SOUTH, SOUTH_WEST, WEST, NORTH_WEST };

static const int dir_file[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int dir_rank[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };

/* All squares from sq in one direction, without sq itself. */
static Bitboard rays[8][64];

static Bitboard bit_if_on_board(int file, int rank) {
    if (file < 0 || file > 7 || rank < 0 || rank > 7)
        return 0;
    return bit(make_square(file, rank));
}

void attacks_init(void) {
    static const int knight_df[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
    static const int knight_dr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };

    for (int sq = 0; sq < 64; sq++) {
        int f = file_of(sq), r = rank_of(sq);

        knight_attacks[sq] = 0;
        king_attacks[sq] = 0;
        for (int i = 0; i < 8; i++) {
            knight_attacks[sq] |= bit_if_on_board(f + knight_df[i], r + knight_dr[i]);
            king_attacks[sq] |= bit_if_on_board(f + dir_file[i], r + dir_rank[i]);
        }

        pawn_attacks[WHITE][sq] = bit_if_on_board(f - 1, r + 1) | bit_if_on_board(f + 1, r + 1);
        pawn_attacks[BLACK][sq] = bit_if_on_board(f - 1, r - 1) | bit_if_on_board(f + 1, r - 1);

        for (int d = 0; d < 8; d++) {
            rays[d][sq] = 0;
            for (int rf = f + dir_file[d], rr = r + dir_rank[d];
                 rf >= 0 && rf < 8 && rr >= 0 && rr < 8;
                 rf += dir_file[d], rr += dir_rank[d])
                rays[d][sq] |= bit(make_square(rf, rr));
        }
    }
}

static inline Bitboard ray_attacks(int dir, int sq, Bitboard occupied) {
    Bitboard attacks = rays[dir][sq];
    Bitboard blockers = attacks & occupied;
    if (blockers) {
        /* These four directions walk towards higher square numbers. */
        bool upwards = dir == NORTH || dir == NORTH_EAST || dir == EAST || dir == NORTH_WEST;
        int first = upwards ? lsb(blockers) : msb(blockers);
        attacks ^= rays[dir][first];
    }
    return attacks;
}

Bitboard bishop_attacks(int sq, Bitboard occupied) {
    return ray_attacks(NORTH_EAST, sq, occupied) | ray_attacks(SOUTH_EAST, sq, occupied)
         | ray_attacks(SOUTH_WEST, sq, occupied) | ray_attacks(NORTH_WEST, sq, occupied);
}

Bitboard rook_attacks(int sq, Bitboard occupied) {
    return ray_attacks(NORTH, sq, occupied) | ray_attacks(EAST, sq, occupied)
         | ray_attacks(SOUTH, sq, occupied) | ray_attacks(WEST, sq, occupied);
}
