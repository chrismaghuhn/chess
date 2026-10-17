#include "attacks.h"

#include <stdio.h>
#include <stdlib.h>

#include "rng.h"

Bitboard knight_attacks[64];
Bitboard king_attacks[64];
Bitboard pawn_attacks[2][64];
Magic bishop_magics[64];
Magic rook_magics[64];
Bitboard between_bb[64][64];
Bitboard forward_file_bb[2][64];
Bitboard passed_span_bb[2][64];
Bitboard adjacent_files_bb[8];

static Bitboard bishop_table[5248];
static Bitboard rook_table[102400];

static void init_magics(void);

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

    init_magics();

    /* Pawn geometry: squares ahead on the same file, and ahead on the same and adjacent files. */
    for (int f = 0; f < 8; f++) {
        Bitboard file_a = 0x0101010101010101ULL;
        adjacent_files_bb[f] = (f > 0 ? file_a << (f - 1) : 0) | (f < 7 ? file_a << (f + 1) : 0);
    }
    for (int sq = 0; sq < 64; sq++) {
        for (int color = 0; color < 2; color++) {
            Bitboard ahead = 0;
            for (int r = rank_of(sq) + (color == WHITE ? 1 : -1); r >= 0 && r < 8; r += color == WHITE ? 1 : -1)
                ahead |= 0xFFULL << (8 * r);
            Bitboard file = 0x0101010101010101ULL << file_of(sq);
            forward_file_bb[color][sq] = ahead & file;
            passed_span_bb[color][sq] = ahead & (file | adjacent_files_bb[file_of(sq)]);
        }
    }

    /* Squares strictly between two squares on a common line. */
    for (int a = 0; a < 64; a++) {
        for (int b = 0; b < 64; b++) {
            between_bb[a][b] = 0;
            if (a == b)
                continue;
            if (rook_attacks_slow(a, 0) & bit(b))
                between_bb[a][b] = rook_attacks_slow(a, bit(b)) & rook_attacks_slow(b, bit(a));
            else if (bishop_attacks_slow(a, 0) & bit(b))
                between_bb[a][b] = bishop_attacks_slow(a, bit(b)) & bishop_attacks_slow(b, bit(a));
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

Bitboard bishop_attacks_slow(int sq, Bitboard occupied) {
    return ray_attacks(NORTH_EAST, sq, occupied) | ray_attacks(SOUTH_EAST, sq, occupied)
         | ray_attacks(SOUTH_WEST, sq, occupied) | ray_attacks(NORTH_WEST, sq, occupied);
}

Bitboard rook_attacks_slow(int sq, Bitboard occupied) {
    return ray_attacks(NORTH, sq, occupied) | ray_attacks(EAST, sq, occupied)
         | ray_attacks(SOUTH, sq, occupied) | ray_attacks(WEST, sq, occupied);
}

/*
 * Magic bitboards: the blockers that matter for a slider on sq (its rays
 * without the board edge) are multiplied by a magic number, and the top
 * bits of the product index a table of precomputed attacks. The magics are
 * found at startup by trying sparse random numbers until no two blocker
 * sets with different attacks collide; the fixed seed makes this the same
 * every run.
 */
static Bitboard relevant_mask(int sq, Bitboard (*slow)(int, Bitboard)) {
    Bitboard rank1 = 0xFFULL, rank8 = rank1 << 56;
    Bitboard file_a = 0x0101010101010101ULL, file_h = file_a << 7;
    Bitboard own_rank = rank1 << (8 * rank_of(sq)), own_file = file_a << file_of(sq);
    Bitboard edges = ((rank1 | rank8) & ~own_rank) | ((file_a | file_h) & ~own_file);
    return slow(sq, 0) & ~edges;
}

static void find_magics(Magic *magics, Bitboard *table, Bitboard (*slow)(int, Bitboard), uint64_t *rng) {
    static Bitboard occupancy[4096], reference[4096];
    static int epoch[4096], current;

    Bitboard *next = table;
    for (int sq = 0; sq < 64; sq++) {
        Magic *m = &magics[sq];
        m->mask = relevant_mask(sq, slow);
        int bits = popcount(m->mask);
        m->shift = 64 - (unsigned)bits;
        m->attacks = next;

        /* Every subset of the mask (carry-rippler enumeration). */
        int size = 0;
        Bitboard subset = 0;
        do {
            occupancy[size] = subset;
            reference[size] = slow(sq, subset);
            size++;
            subset = (subset - m->mask) & m->mask;
        } while (subset);

        for (;;) {
            uint64_t magic = rng_next(rng) & rng_next(rng) & rng_next(rng);
            if (popcount((m->mask * magic) >> 56) < 6)
                continue;
            current++;
            bool ok = true;
            for (int i = 0; i < size && ok; i++) {
                unsigned index = (unsigned)((occupancy[i] * magic) >> m->shift);
                if (epoch[index] != current) {
                    epoch[index] = current;
                    next[index] = reference[i];
                } else if (next[index] != reference[i]) {
                    ok = false;
                }
            }
            if (ok) {
                m->magic = magic;
                break;
            }
        }
        next += (size_t)1 << bits;
    }
}

static void init_magics(void) {
    uint64_t rng = 0x4D41474943ULL; /* "MAGIC" */
    find_magics(bishop_magics, bishop_table, bishop_attacks_slow, &rng);
    find_magics(rook_magics, rook_table, rook_attacks_slow, &rng);
}
