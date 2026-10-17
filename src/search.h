#ifndef SEARCH_H
#define SEARCH_H

#include <stdatomic.h>
#include <stddef.h>

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

void search_limits_init(SearchLimits *limits);

/*
 * Iterative deepening from the game's current position until a limit is
 * hit. Always returns a legal move if there is one. With print_info it
 * writes UCI "info" lines to stdout.
 */
SearchResult search_run(Search *s, const Game *game, const SearchLimits *limits, bool print_info);

/* Moves to mate for a mate score: positive if the side to move mates, else 0. */
int search_mate_moves(int score);

#endif
