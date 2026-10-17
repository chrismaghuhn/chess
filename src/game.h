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

/* Why a game ended. END_PLY_LIMIT is not a chess rule; self-play uses it. */
typedef enum {
    END_NONE, END_MATE, END_STALEMATE, END_REPETITION, END_FIFTY_MOVES, END_MATERIAL, END_PLY_LIMIT,
    END_COUNT
} GameEnd;

extern const char *const game_end_names[END_COUNT];

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

/* Like game_result, also says why the game ended (END_NONE while ongoing). */
GameResult game_result_detail(const Game *g, GameEnd *end);

#endif
