#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <stdio.h>

#include "bitboard.h"

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
#define FEN_MAX 100

typedef struct {
    Bitboard pieces[2][6]; /* [color][type] */
    Bitboard occupied[2];  /* [color] */
    Bitboard all;
    uint64_t hash;
    uint8_t squares[64];   /* piece on each square, or NO_PIECE */
    uint8_t side;          /* side to move */
    uint8_t castling;      /* WHITE_OO | WHITE_OOO | BLACK_OO | BLACK_OOO */
    uint8_t ep;            /* en passant target square, or SQ_NONE */
    uint16_t halfmove;     /* plies since the last capture or pawn move */
    uint16_t fullmove;
} Board;

/* Must be called once before anything else. */
void chess_init(void);

/*
 * Parses a FEN; the move counters are optional. Rejects malformed and
 * illegal positions. Castling rights without king and rook on their home
 * squares are dropped, and so is an en passant square no pawn can capture on.
 */
bool board_from_fen(Board *b, const char *fen);

/* Builds a board from its parts with the same checks and normalization as board_from_fen. */
bool board_setup(Board *b, const uint8_t squares[64], int side, int castling, int ep,
                 int halfmove, int fullmove);
void board_to_fen(const Board *b, char *buf, size_t size);

/*
 * Plays a pseudo-legal move on a copy of b. Returns false if the move
 * leaves the mover's king in check; out is then garbage. The move must come
 * from movegen_generate for this position: moves from outside (UCI) have to
 * be matched against the generated list first.
 */
bool board_make_move(const Board *b, Move m, Board *out);

/* Own pieces that are the only blocker between the own king and an enemy slider. */
Bitboard board_pinned(const Board *b);

/*
 * board_make_move that skips the king test where a move cannot expose the
 * king: no check, not a king move, not a pinned piece, not en passant.
 * pinned and in_check must describe b (board_pinned, board_in_check).
 */
bool board_make_move_fast(const Board *b, Move m, Board *out, Bitboard pinned, bool in_check);

bool board_square_attacked(const Board *b, int sq, int by_color);

static inline int board_king_square(const Board *b, int color) {
    return lsb(b->pieces[color][KING]);
}

static inline bool board_in_check(const Board *b) {
    return board_square_attacked(b, board_king_square(b, b->side), b->side ^ 1);
}

/* No pawns, rooks or queens and at most one minor piece: nobody can mate. */
bool board_insufficient_material(const Board *b);

uint64_t board_compute_hash(const Board *b);

/* Checks that bitboards, mailbox and hash agree. For debug builds and tests. */
bool board_is_consistent(const Board *b);

void board_print(const Board *b, FILE *out);

#endif
