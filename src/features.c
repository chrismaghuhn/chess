#include "features.h"

#include "attacks.h"

typedef struct {
    Feature *out;
    int count;
    int mg, eg; /* shares of the side being added, signed */
} Collector;

/* One feature: its middlegame and endgame twins with their shares. */
static void add(Collector *c, int index) {
    if (c->mg) {
        c->out[c->count].index = (uint16_t)index;
        c->out[c->count].value = (int8_t)c->mg;
        c->count++;
    }
    if (c->eg) {
        c->out[c->count].index = (uint16_t)(FEATURE_EG + index);
        c->out[c->count].value = (int8_t)c->eg;
        c->count++;
    }
}

int features_extract(const Board *b, Feature out[MAX_ACTIVE_FEATURES]) {
    int phase = board_phase(b);
    Collector c = { out, 0, 0, 0 };

    for (int color = 0; color < 2; color++) {
        int sign = color == b->side ? 1 : -1;
        int mirror = color == WHITE ? 0 : 56; /* flips the rank */
        const Bitboard *own = b->pieces[color];
        Bitboard own_all = b->occupied[color];
        Bitboard enemy_pawns = b->pieces[color ^ 1][PAWN];
        c.mg = sign * phase;
        c.eg = sign * (PHASE_MAX - phase);

        for (int type = 0; type < 6; type++) {
            for (Bitboard bb = own[type]; bb;) {
                int sq = pop_lsb(&bb);
                add(&c, F_PSQ + type * 64 + (sq ^ mirror));

                Bitboard reach;
                switch (type) {
                case KNIGHT: reach = knight_attacks[sq]; break;
                case BISHOP: reach = bishop_attacks(sq, b->all); break;
                case ROOK: reach = rook_attacks(sq, b->all); break;
                case QUEEN: reach = queen_attacks(sq, b->all); break;
                default: reach = 0;
                }
                if (type >= KNIGHT && type <= QUEEN)
                    add(&c, F_MOBILITY + mobility_offset(type) + popcount(reach & ~own_all));

                if (type == PAWN) {
                    int rank = rank_of(sq ^ mirror); /* 1..6 for a pawn */
                    if (!(passed_span_bb[color][sq] & enemy_pawns))
                        add(&c, F_PASSED + rank - 1);
                    if (forward_file_bb[color][sq] & own[PAWN])
                        add(&c, F_DOUBLED);
                    if (!(adjacent_files_bb[file_of(sq)] & own[PAWN]))
                        add(&c, F_ISOLATED);
                }
            }
        }
        if (popcount(own[BISHOP]) >= 2)
            add(&c, F_BISHOP_PAIR);
    }
    return c.count;
}
