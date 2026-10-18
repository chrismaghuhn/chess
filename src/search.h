#ifndef SEARCH_H
#define SEARCH_H

#include <stdatomic.h>
#include <stddef.h>

#include "eval.h"
#include "game.h"

#define MAX_PLY 128
#define SCORE_INF 32000
#define SCORE_MATE 31000
#define SCORE_MATE_BOUND (SCORE_MATE - MAX_PLY) /* beyond this a score is a mate */

typedef struct {
    int depth;         /* 0 = no limit */
    uint64_t nodes;    /* 0 = no limit */
    int movetime;      /* ms, 0 = none */
    int time[2];       /* ms left per color, -1 = not given */
    int inc[2];        /* ms per move per color */
    int movestogo;     /* 0 = not given */
    atomic_bool *stop; /* optional; setting it to true aborts the search */
    uint64_t seed;     /* nonzero: pick at random among equally scored root moves */
} SearchLimits;

typedef struct {
    Move best;      /* 0 if the side to move has no legal move */
    int score;      /* side to move's view */
    int depth;      /* last fully searched depth */
    uint64_t nodes;
} SearchResult;

/* One searcher per thread: it owns its hash table and history. */
typedef struct Search Search;

Search *search_new(size_t hash_mb);
void search_free(Search *s);
bool search_resize(Search *s, size_t hash_mb);

/* Forgets everything learned during earlier searches (new game). */
void search_clear(Search *s);

/*
 * Optional search techniques: principal variation search, null-move
 * pruning, late move reductions (NMP and LMR need PVS; "all" is these three),
 * and "exact": with a seed, find exactly tied root moves and draw one at
 * random even with learned weights (otherwise only with all weights 0) –
 * costs depth, but self-play training learned more per game with it.
 */
enum { SEARCH_PVS = 1, SEARCH_NMP = 2, SEARCH_LMR = 4, SEARCH_ALL = 7, SEARCH_EXACT_TIES = 8 };

/* What the engine uses unless told otherwise (UCI, self-play, matches, bench). */
#define SEARCH_DEFAULT ((unsigned)SEARCH_ALL)

void search_set_options(Search *s, unsigned options);

/*
 * Parses "none" or a comma list of pvs, nmp, lmr, all (= the three) and exact,
 * e.g. "all,exact"; false if empty, unknown, or NMP/LMR without PVS.
 */
bool search_parse_options(const char *text, unsigned *options);

/*
 * Evaluate with this network instead of the linear weights (experiment); the
 * weights still provide the learned piece values for move ordering. NULL
 * switches back. The caller keeps it alive. The search uses an integer copy
 * (nnue_quantize); false if the network cannot be stored that way or memory
 * runs out, and the search then evaluates linearly.
 */
/*
 * Playing style (for the Lichess bot; both 0 by default, as in training):
 * contempt makes a draw count that much against the searching side, so it
 * avoids draws; knight_love adds that much per knight it has more than the
 * opponent. Changing the style clears the hash table.
 */
void search_set_style(Search *s, int contempt, int knight_love);

typedef struct Network Network;
bool search_set_network(Search *s, const Network *net);

/* Evaluation weights to use; NULL means all zero. The caller keeps them alive. */
void search_set_weights(Search *s, const Weights *weights);

void search_limits_init(SearchLimits *limits);

/*
 * Iterative deepening from the game's current position until a limit is
 * hit. Always returns a legal move if there is one. With print_info it
 * writes UCI "info" lines to stdout.
 */
SearchResult search_run(Search *s, const Game *game, const SearchLimits *limits, bool print_info);

/*
 * Runs only the quiescence search (captures and promotions, or evasions in
 * check) and follows its best line to the end: the quiet position whose
 * static evaluation the score comes from. The tuner trains on these, so it
 * does not learn from positions where a piece is about to be taken.
 * score is from b's side to move; leaf may have the other side to move.
 * Gives up after max_nodes (0 = no limit) and returns false with leaf = b.
 */
bool search_quiet(Search *s, const Board *b, uint64_t max_nodes, Board *leaf, int *score,
                  uint64_t *nodes);

/* Moves to mate for a mate score: positive if the side to move mates, else 0. */
int search_mate_moves(int score);

#endif
