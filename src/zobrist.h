#ifndef ZOBRIST_H
#define ZOBRIST_H

#include <stdint.h>

extern uint64_t zobrist_piece[12][64];
extern uint64_t zobrist_castling[16];
extern uint64_t zobrist_ep[8]; /* by file */
extern uint64_t zobrist_side;  /* xored in when black is to move */

void zobrist_init(void);

#endif
