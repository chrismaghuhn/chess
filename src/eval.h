#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "features.h"

/*
 * Linear evaluation: the sum of the weights of the active features, from the
 * side to move's view. Generation 0 has all weights at 0 and knows nothing
 * beyond the rules; later generations load weights learned from self-play.
 *
 * Units are whatever the tuner makes them; it maps scores to win
 * probability with a fixed scale, so they end up roughly comparable to
 * centipawns. EVAL_MAX keeps every evaluation clear of mate scores.
 *
 * The engine plays with weights rounded to integers (half to even) and
 * clamped to WEIGHT_MAX, and clamps the sum to EVAL_MAX. A tuner that works
 * with floats sees slightly different scores: at most 0.5 per active
 * feature from rounding, and none from clamping as long as its weights stay
 * within the limits. It should check that before writing a file.
 */
#define EVAL_MAX 20000
#define WEIGHT_MAX 10000

typedef struct {
    int32_t w[FEATURE_COUNT];
} Weights;

int eval_evaluate(const Weights *weights, const Board *b);

/*
 * The same sum kept incrementally by the search: "psq" is the unclamped
 * weight sum from white's view. eval_psq_delta gives its change for a move
 * (from the board before the move), eval_from_psq the clamped evaluation for
 * the side to move. eval_from_psq(eval_psq(w, b), b->side) == eval_evaluate(w, b).
 */
int eval_psq(const Weights *weights, const Board *b);
int eval_psq_delta(const Weights *weights, const Board *before, Move m);

static inline int eval_from_psq(int psq, int side) {
    int score = side == WHITE ? psq : -psq;
    return score > EVAL_MAX ? EVAL_MAX : score < -EVAL_MAX ? -EVAL_MAX : score;
}

/*
 * Weight files: "SCHW", version (uint32), feature count (uint32), then one
 * float32 per feature, all little-endian. The engine rounds to integers;
 * the tuner keeps floats.
 */
#define WEIGHTS_VERSION 1

void weights_zero(Weights *weights);

/* Reads a weight file. On failure returns false and writes the reason to err. */
bool weights_load_float(const char *path, float out[FEATURE_COUNT], char *err, size_t err_size);
bool weights_load(const char *path, Weights *weights, char *err, size_t err_size);

bool weights_save_float(const char *path, const float weights[FEATURE_COUNT]);

/* The engine's integer weights for float weights: rounded half to even, clamped to WEIGHT_MAX. */
void weights_from_float(const float in[FEATURE_COUNT], Weights *out);

#endif
