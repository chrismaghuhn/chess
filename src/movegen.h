#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"

/*
 * No legal chess position has more than 218 moves. Pseudo-legal moves can be
 * more; with the piece counts board_from_fen allows (at most 9 queens, 2 rooks,
 * 2 bishops, 2 knights) the bound is 9*27 + 2*14 + 2*13 + 2*8 + 8 + 2 = 323.
 */
#define MAX_MOVES 324

typedef struct {
    Move moves[MAX_MOVES];
    int count;
} MoveList;

/*
 * Generates pseudo-legal moves: everything except moves that leave the own
 * king in check, which board_make_move rejects. Castling is fully checked
 * here apart from the king's destination square.
 */
void movegen_generate(const Board *b, MoveList *list);

/* Only captures and promotions (for quiescence search). */
void movegen_generate_noisy(const Board *b, MoveList *list);

/* Only legal moves, in generation order. Slower; not for the search. */
void movegen_legal(const Board *b, MoveList *list);

/* Writes the move in UCI notation, e.g. "e2e4" or "e7e8q". */
void move_to_uci(Move m, char out[6]);

#endif
