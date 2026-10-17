#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"

/* No legal chess position has more than 218 moves. */
typedef struct {
    Move moves[256];
    int count;
} MoveList;

/*
 * Generates pseudo-legal moves: everything except moves that leave the own
 * king in check, which board_make_move rejects. Castling is fully checked
 * here apart from the king's destination square.
 */
void movegen_generate(const Board *b, MoveList *list);

/* Writes the move in UCI notation, e.g. "e2e4" or "e7e8q". */
void move_to_uci(Move m, char out[6]);

#endif
