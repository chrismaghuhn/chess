#ifndef DATAFMT_H
#define DATAFMT_H

#include <stdio.h>

#include "board.h"
#include "game.h"

/*
 * Training data: a 16-byte header ("SCHD", version, record size, reserved;
 * uint32 little-endian each) followed by 32-byte position records:
 *
 *   0..7    occupancy bitboard
 *   8..23   piece codes (0..11) of the occupied squares, low square first,
 *           two per byte, low nibble first
 *   24      side to move
 *   25      castling rights
 *   26      en passant square (64 = none)
 *   27      halfmove clock, capped at 255
 *   28      game result for the side to move: 0 loss, 1 draw, 2 win
 *   29      why the game ended (GameEnd from game.h)
 *   30..31  ply of the position in its game
 *
 * Records of one game are stored in order, so a new game starts wherever
 * the ply does not go up by one. Unused nibbles and the header's reserved
 * word must be zero.
 */
#define DATA_VERSION 1
#define DATA_HEADER_SIZE 16
#define DATA_RECORD_SIZE 32

enum { RESULT_LOSS, RESULT_DRAW, RESULT_WIN };

typedef struct {
    Board board;
    int result; /* RESULT_* for the side to move */
    GameEnd end;
    int ply;
} DataPosition;

void data_pack(const Board *b, int result, int ply, uint8_t out[DATA_RECORD_SIZE]);

/* Fills in how the game ended: the result seen from white (RESULT_*) and why. */
void data_set_outcome(uint8_t record[DATA_RECORD_SIZE], int white_result, GameEnd end);

/* Returns false for records that do not describe a legal position. */
bool data_unpack(const uint8_t in[DATA_RECORD_SIZE], DataPosition *out);

/* Creates a data file and writes the header. */
FILE *data_create(const char *path);

/* Opens a data file and checks its header; count receives the number of records. */
FILE *data_open(const char *path, uint64_t *count, char *err, size_t err_size);

#endif
