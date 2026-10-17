#ifndef GAME_H
#define GAME_H

#include "board.h"

#define GAME_MAX_HISTORY 1024

/*
 * A position plus the hashes needed for repetition checks: every position
 * since the last capture or pawn move, the current one last.
 */
typedef struct {
    Board board;
    uint64_t hashes[GAME_MAX_HISTORY];
    int count;
} Game;

typedef enum { GAME_ONGOING, GAME_WHITE_WINS, GAME_BLACK_WINS, GAME_DRAW } GameResult;

void game_init(Game *g, const Board *b);

/* Finds the legal move with this UCI name ("e2e4", "e7e8q"). */
bool game_find_move(const Board *b, const char *uci, Move *out);

/*
 * Plays a move from movegen_generate for the current position. Returns false
 * (and changes nothing) if it leaves the own king in check.
 */
bool game_play(Game *g, Move m);

/* Mate, stalemate, threefold repetition, 50-move rule, insufficient material. */
GameResult game_result(const Game *g);

#endif
