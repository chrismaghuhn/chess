#include "board.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "attacks.h"
#include "zobrist.h"

static const char piece_chars[] = "PNBRQKpnbrqk";

/* Castling rights that survive a move from or to this square. */
static uint8_t castle_mask[64];

void chess_init(void) {
    attacks_init();
    zobrist_init();

    for (int sq = 0; sq < 64; sq++)
        castle_mask[sq] = WHITE_OO | WHITE_OOO | BLACK_OO | BLACK_OOO;
    castle_mask[E1] &= ~(WHITE_OO | WHITE_OOO);
    castle_mask[H1] &= ~WHITE_OO;
    castle_mask[A1] &= ~WHITE_OOO;
    castle_mask[E8] &= ~(BLACK_OO | BLACK_OOO);
    castle_mask[H8] &= ~BLACK_OO;
    castle_mask[A8] &= ~BLACK_OOO;
}

static void put_piece(Board *b, int piece, int sq) {
    b->pieces[piece_color(piece)][piece_type(piece)] |= bit(sq);
    b->occupied[piece_color(piece)] |= bit(sq);
    b->all |= bit(sq);
    b->squares[sq] = (uint8_t)piece;
    b->hash ^= zobrist_piece[piece][sq];
}

static void remove_piece(Board *b, int sq) {
    int piece = b->squares[sq];
    b->pieces[piece_color(piece)][piece_type(piece)] &= ~bit(sq);
    b->occupied[piece_color(piece)] &= ~bit(sq);
    b->all &= ~bit(sq);
    b->squares[sq] = NO_PIECE;
    b->hash ^= zobrist_piece[piece][sq];
}

static void move_piece(Board *b, int from, int to) {
    int piece = b->squares[from];
    remove_piece(b, from);
    put_piece(b, piece, to);
}

bool board_square_attacked(const Board *b, int sq, int by_color) {
    const Bitboard *p = b->pieces[by_color];
    return (pawn_attacks[by_color ^ 1][sq] & p[PAWN])
        || (knight_attacks[sq] & p[KNIGHT])
        || (king_attacks[sq] & p[KING])
        || (bishop_attacks(sq, b->all) & (p[BISHOP] | p[QUEEN]))
        || (rook_attacks(sq, b->all) & (p[ROOK] | p[QUEEN]));
}

bool board_insufficient_material(const Board *b) {
    const Bitboard *w = b->pieces[WHITE], *k = b->pieces[BLACK];
    if (w[PAWN] | k[PAWN] | w[ROOK] | k[ROOK] | w[QUEEN] | k[QUEEN])
        return false;
    return popcount(w[KNIGHT] | k[KNIGHT] | w[BISHOP] | k[BISHOP]) <= 1;
}

uint64_t board_compute_hash(const Board *b) {
    uint64_t h = 0;
    for (int sq = 0; sq < 64; sq++)
        if (b->squares[sq] != NO_PIECE)
            h ^= zobrist_piece[b->squares[sq]][sq];
    h ^= zobrist_castling[b->castling];
    if (b->ep != SQ_NONE)
        h ^= zobrist_ep[file_of(b->ep)];
    if (b->side == BLACK)
        h ^= zobrist_side;
    return h;
}

static void apply_move(const Board *b, Move m, Board *out) {
    Board *n = out;
    *n = *b;

    int us = b->side, them = us ^ 1;
    int from = move_from(m), to = move_to(m), flags = move_flags(m);
    assert(b->squares[from] != NO_PIECE && piece_color(b->squares[from]) == us);
    int type = piece_type(b->squares[from]);

    if (n->ep != SQ_NONE) {
        n->hash ^= zobrist_ep[file_of(n->ep)];
        n->ep = SQ_NONE;
    }

    if (flags == MF_EP_CAPTURE)
        remove_piece(n, to ^ 8); /* the captured pawn is one rank behind the target */
    else if (flags & MF_CAPTURE)
        remove_piece(n, to);
    move_piece(n, from, to);

    if (type == PAWN || (flags & MF_CAPTURE))
        n->halfmove = 0;
    else
        n->halfmove++;

    if (flags & MF_PROMO) {
        remove_piece(n, to);
        put_piece(n, make_piece(us, move_promo_type(m)), to);
    } else if (flags == MF_DOUBLE_PUSH) {
        /*
         * Only record the en passant square if an enemy pawn could take
         * there, so that repetition checks see equal positions as equal.
         * A pinned capturer still counts; that rare case is accepted.
         */
        int ep = (from + to) / 2;
        if (pawn_attacks[us][ep] & n->pieces[them][PAWN]) {
            n->ep = (uint8_t)ep;
            n->hash ^= zobrist_ep[file_of(ep)];
        }
    } else if (flags == MF_KING_CASTLE) {
        move_piece(n, to + 1, to - 1);
    } else if (flags == MF_QUEEN_CASTLE) {
        move_piece(n, to - 2, to + 1);
    }

    int rights = n->castling & castle_mask[from] & castle_mask[to];
    if (rights != n->castling) {
        n->hash ^= zobrist_castling[n->castling] ^ zobrist_castling[rights];
        n->castling = (uint8_t)rights;
    }

    n->side = (uint8_t)them;
    n->hash ^= zobrist_side;
    if (us == BLACK)
        n->fullmove++;
}

bool board_make_move(const Board *b, Move m, Board *out) {
    apply_move(b, m, out);
    return !board_square_attacked(out, board_king_square(out, b->side), b->side ^ 1);
}

void board_make_null(const Board *b, Board *out) {
    *out = *b;
    if (out->ep != SQ_NONE) {
        out->hash ^= zobrist_ep[file_of(out->ep)];
        out->ep = SQ_NONE;
    }
    out->side ^= 1;
    out->hash ^= zobrist_side;
    out->halfmove = 0; /* nothing before a pass can repeat across it */
}

Bitboard board_pinned(const Board *b) {
    int us = b->side, them = us ^ 1, king = board_king_square(b, us);
    const Bitboard *p = b->pieces[them];
    Bitboard snipers = (rook_attacks(king, 0) & (p[ROOK] | p[QUEEN]))
                     | (bishop_attacks(king, 0) & (p[BISHOP] | p[QUEEN]));
    Bitboard pinned = 0;
    while (snipers) {
        Bitboard between = between_bb[king][pop_lsb(&snipers)] & b->all;
        if (between && !(between & (between - 1)) && (between & b->occupied[us]))
            pinned |= between;
    }
    return pinned;
}

bool board_make_move_fast(const Board *b, Move m, Board *out, Bitboard pinned, bool in_check) {
    int from = move_from(m);
    if (in_check || (pinned & bit(from)) || piece_type(b->squares[from]) == KING
        || move_flags(m) == MF_EP_CAPTURE)
        return board_make_move(b, m, out);
    apply_move(b, m, out);
    return true;
}

static int excess(int count, int initial) {
    return count > initial ? count - initial : 0;
}

/*
 * Every piece beyond the starting set needs a promoted pawn. This also
 * bounds the pseudo-legal move count below MAX_MOVES.
 */
static bool piece_counts_possible(const Bitboard *pieces) {
    int promoted = excess(popcount(pieces[KNIGHT]), 2) + excess(popcount(pieces[BISHOP]), 2)
                 + excess(popcount(pieces[ROOK]), 2) + excess(popcount(pieces[QUEEN]), 1);
    return popcount(pieces[PAWN]) + promoted <= 8;
}

bool board_setup(Board *b, const uint8_t squares[64], int side, int castling, int ep,
                 int halfmove, int fullmove) {
    if ((side != WHITE && side != BLACK) || castling < 0 || castling > 15
        || ep < 0 || ep > SQ_NONE || halfmove < 0 || halfmove > 10000
        || fullmove < 1 || fullmove > 65535)
        return false;

    memset(b, 0, sizeof *b);
    memset(b->squares, NO_PIECE, sizeof b->squares);
    for (int sq = 0; sq < 64; sq++) {
        if (squares[sq] > NO_PIECE)
            return false;
        if (squares[sq] != NO_PIECE)
            put_piece(b, squares[sq], sq);
    }
    b->side = (uint8_t)side;
    b->castling = (uint8_t)castling;
    b->ep = (uint8_t)ep;
    b->halfmove = (uint16_t)halfmove;
    b->fullmove = (uint16_t)fullmove;

    /* Legality checks. */
    int us = b->side, them = us ^ 1;
    if (popcount(b->pieces[WHITE][KING]) != 1 || popcount(b->pieces[BLACK][KING]) != 1)
        return false;
    if (!piece_counts_possible(b->pieces[WHITE]) || !piece_counts_possible(b->pieces[BLACK]))
        return false;
    if ((b->pieces[WHITE][PAWN] | b->pieces[BLACK][PAWN]) & 0xFF000000000000FFULL)
        return false;
    if (board_square_attacked(b, board_king_square(b, them), us))
        return false;

    if (b->squares[E1] != make_piece(WHITE, KING))
        b->castling &= ~(WHITE_OO | WHITE_OOO);
    if (b->squares[H1] != make_piece(WHITE, ROOK))
        b->castling &= ~WHITE_OO;
    if (b->squares[A1] != make_piece(WHITE, ROOK))
        b->castling &= ~WHITE_OOO;
    if (b->squares[E8] != make_piece(BLACK, KING))
        b->castling &= ~(BLACK_OO | BLACK_OOO);
    if (b->squares[H8] != make_piece(BLACK, ROOK))
        b->castling &= ~BLACK_OO;
    if (b->squares[A8] != make_piece(BLACK, ROOK))
        b->castling &= ~BLACK_OOO;

    if (ep != SQ_NONE) {
        if (rank_of(ep) != (us == WHITE ? 5 : 2))
            return false;
        int origin = us == WHITE ? ep + 8 : ep - 8; /* where the pawn came from */
        if (b->squares[ep ^ 8] != make_piece(them, PAWN)
            || b->squares[ep] != NO_PIECE || b->squares[origin] != NO_PIECE)
            return false;
        if (!(pawn_attacks[them][ep] & b->pieces[us][PAWN]))
            b->ep = SQ_NONE;
    }

    b->hash = board_compute_hash(b);
    return true;
}

bool board_from_fen(Board *b, const char *fen) {
    uint8_t squares[64];
    memset(squares, NO_PIECE, sizeof squares);
    int side, castling = 0, ep = SQ_NONE, halfmove = 0, fullmove = 1;

    const char *p = fen;

    /* Piece placement, from rank 8 down to rank 1. */
    int rank = 7, file = 0;
    for (; *p && *p != ' '; p++) {
        if (*p == '/') {
            if (file != 8 || rank == 0)
                return false;
            rank--;
            file = 0;
        } else if (*p >= '1' && *p <= '8') {
            file += *p - '0';
            if (file > 8)
                return false;
        } else {
            const char *pc = strchr(piece_chars, *p);
            if (!pc || file > 7)
                return false;
            squares[make_square(file, rank)] = (uint8_t)(pc - piece_chars);
            file++;
        }
    }
    if (rank != 0 || file != 8)
        return false;

    if (*p++ != ' ')
        return false;
    if (*p == 'w')
        side = WHITE;
    else if (*p == 'b')
        side = BLACK;
    else
        return false;
    p++;

    if (*p++ != ' ')
        return false;
    if (*p == '-') {
        p++;
    } else {
        for (; *p && *p != ' '; p++) {
            switch (*p) {
            case 'K': castling |= WHITE_OO; break;
            case 'Q': castling |= WHITE_OOO; break;
            case 'k': castling |= BLACK_OO; break;
            case 'q': castling |= BLACK_OOO; break;
            default: return false;
            }
        }
    }

    if (*p++ != ' ')
        return false;
    if (*p == '-') {
        p++;
    } else {
        if (p[0] < 'a' || p[0] > 'h' || (p[1] != '3' && p[1] != '6'))
            return false;
        ep = make_square(p[0] - 'a', p[1] - '1');
        p += 2;
    }
    if (*p && *p != ' ' && *p != '\n' && *p != '\r')
        return false;

    /* Optional move counters. Some tools write fullmove 0; that becomes 1. */
    while (*p == ' ')
        p++;
    if (*p >= '0' && *p <= '9') {
        char *end;
        long half = strtol(p, &end, 10);
        if (end == p || half > 10000)
            return false;
        p = end;
        long full = strtol(p, &end, 10);
        if (end == p || full < 0 || full > 65535)
            return false;
        p = end;
        halfmove = (int)half;
        fullmove = full ? (int)full : 1;
    }
    while (*p == ' ' || *p == '\n' || *p == '\r')
        p++;
    if (*p)
        return false;

    return board_setup(b, squares, side, castling, ep, halfmove, fullmove);
}

void board_to_fen(const Board *b, char *buf, size_t size) {
    char fen[FEN_MAX];
    int len = 0;

    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int piece = b->squares[make_square(file, rank)];
            if (piece == NO_PIECE) {
                empty++;
                continue;
            }
            if (empty)
                fen[len++] = (char)('0' + empty);
            empty = 0;
            fen[len++] = piece_chars[piece];
        }
        if (empty)
            fen[len++] = (char)('0' + empty);
        if (rank)
            fen[len++] = '/';
    }

    fen[len++] = ' ';
    fen[len++] = b->side == WHITE ? 'w' : 'b';
    fen[len++] = ' ';
    if (!b->castling)
        fen[len++] = '-';
    if (b->castling & WHITE_OO) fen[len++] = 'K';
    if (b->castling & WHITE_OOO) fen[len++] = 'Q';
    if (b->castling & BLACK_OO) fen[len++] = 'k';
    if (b->castling & BLACK_OOO) fen[len++] = 'q';
    fen[len++] = ' ';
    if (b->ep == SQ_NONE) {
        fen[len++] = '-';
    } else {
        fen[len++] = (char)('a' + file_of(b->ep));
        fen[len++] = (char)('1' + rank_of(b->ep));
    }
    snprintf(fen + len, sizeof fen - (size_t)len, " %d %d", b->halfmove, b->fullmove);

    snprintf(buf, size, "%s", fen);
}

bool board_is_consistent(const Board *b) {
    Bitboard seen = 0, occupied[2] = { 0, 0 };

    for (int color = 0; color < 2; color++) {
        for (int type = 0; type < 6; type++) {
            Bitboard bb = b->pieces[color][type];
            if (bb & seen)
                return false;
            seen |= bb;
            occupied[color] |= bb;
            while (bb)
                if (b->squares[pop_lsb(&bb)] != make_piece(color, type))
                    return false;
        }
    }
    if (occupied[WHITE] != b->occupied[WHITE] || occupied[BLACK] != b->occupied[BLACK]
        || seen != b->all)
        return false;

    int filled = 0;
    for (int sq = 0; sq < 64; sq++)
        filled += b->squares[sq] != NO_PIECE;
    if (filled != popcount(seen))
        return false;

    if (popcount(b->pieces[WHITE][KING]) != 1 || popcount(b->pieces[BLACK][KING]) != 1)
        return false;

    return b->hash == board_compute_hash(b);
}

void board_print(const Board *b, FILE *out) {
    for (int rank = 7; rank >= 0; rank--) {
        fprintf(out, "%d ", rank + 1);
        for (int file = 0; file < 8; file++) {
            int piece = b->squares[make_square(file, rank)];
            fprintf(out, " %c", piece == NO_PIECE ? '.' : piece_chars[piece]);
        }
        fputc('\n', out);
    }
    fprintf(out, "   a b c d e f g h\n");

    char fen[FEN_MAX];
    board_to_fen(b, fen, sizeof fen);
    fprintf(out, "FEN:  %s\nHash: %016llx\n", fen, (unsigned long long)b->hash);
}
