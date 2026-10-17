#include "movegen.h"

#include "attacks.h"

static inline void add(MoveList *list, int from, int to, int flags) {
    list->moves[list->count++] = move_encode(from, to, flags);
}

static void add_targets(MoveList *list, int from, Bitboard targets, Bitboard enemy) {
    while (targets) {
        int to = pop_lsb(&targets);
        add(list, from, to, (enemy & bit(to)) ? MF_CAPTURE : MF_QUIET);
    }
}

static void add_promotions(MoveList *list, int from, int to, int capture) {
    for (int piece = 3; piece >= 0; piece--) /* queen first */
        add(list, from, to, MF_PROMO | capture | piece);
}

static void generate_pawn_moves(const Board *b, MoveList *list) {
    int us = b->side;
    Bitboard enemy = b->occupied[us ^ 1], empty = ~b->all;
    int push = us == WHITE ? 8 : -8;
    int start_rank = us == WHITE ? 1 : 6;
    int last_rank_before_promo = us == WHITE ? 6 : 1;

    for (Bitboard pawns = b->pieces[us][PAWN]; pawns;) {
        int from = pop_lsb(&pawns);
        int to = from + push;
        bool promotes = rank_of(from) == last_rank_before_promo;

        if (empty & bit(to)) {
            if (promotes) {
                add_promotions(list, from, to, 0);
            } else {
                add(list, from, to, MF_QUIET);
                if (rank_of(from) == start_rank && (empty & bit(to + push)))
                    add(list, from, to + push, MF_DOUBLE_PUSH);
            }
        }

        for (Bitboard captures = pawn_attacks[us][from] & enemy; captures;) {
            int target = pop_lsb(&captures);
            if (promotes)
                add_promotions(list, from, target, MF_CAPTURE);
            else
                add(list, from, target, MF_CAPTURE);
        }

        if (b->ep != SQ_NONE && (pawn_attacks[us][from] & bit(b->ep)))
            add(list, from, b->ep, MF_EP_CAPTURE);
    }
}

static void generate_castling(const Board *b, MoveList *list) {
    int us = b->side, them = us ^ 1;
    int base = us == WHITE ? 0 : 56; /* shifts first-rank squares to the 8th rank */
    int king_side = us == WHITE ? WHITE_OO : BLACK_OO;
    int queen_side = us == WHITE ? WHITE_OOO : BLACK_OOO;

    if (!(b->castling & (king_side | queen_side)))
        return;
    if (board_square_attacked(b, E1 + base, them))
        return;

    if ((b->castling & king_side)
        && !(b->all & (bit(F1 + base) | bit(G1 + base)))
        && !board_square_attacked(b, F1 + base, them))
        add(list, E1 + base, G1 + base, MF_KING_CASTLE);

    if ((b->castling & queen_side)
        && !(b->all & (bit(B1 + base) | bit(C1 + base) | bit(D1 + base)))
        && !board_square_attacked(b, D1 + base, them))
        add(list, E1 + base, C1 + base, MF_QUEEN_CASTLE);
}

void movegen_generate(const Board *b, MoveList *list) {
    int us = b->side;
    Bitboard enemy = b->occupied[us ^ 1];
    Bitboard targets = ~b->occupied[us];
    const Bitboard *own = b->pieces[us];

    list->count = 0;

    generate_pawn_moves(b, list);

    for (Bitboard bb = own[KNIGHT]; bb;) {
        int from = pop_lsb(&bb);
        add_targets(list, from, knight_attacks[from] & targets, enemy);
    }
    for (Bitboard bb = own[BISHOP] | own[QUEEN]; bb;) {
        int from = pop_lsb(&bb);
        add_targets(list, from, bishop_attacks(from, b->all) & targets, enemy);
    }
    for (Bitboard bb = own[ROOK] | own[QUEEN]; bb;) {
        int from = pop_lsb(&bb);
        add_targets(list, from, rook_attacks(from, b->all) & targets, enemy);
    }

    int king = board_king_square(b, us);
    add_targets(list, king, king_attacks[king] & targets, enemy);

    generate_castling(b, list);
}

void move_to_uci(Move m, char out[6]) {
    int from = move_from(m), to = move_to(m);
    out[0] = (char)('a' + file_of(from));
    out[1] = (char)('1' + rank_of(from));
    out[2] = (char)('a' + file_of(to));
    out[3] = (char)('1' + rank_of(to));
    out[4] = move_is_promo(m) ? "nbrq"[move_promo_type(m) - KNIGHT] : '\0';
    out[5] = '\0';
}
