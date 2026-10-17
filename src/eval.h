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
 */
#define EVAL_MAX 20000
#define WEIGHT_MAX 10000

typedef struct {
    int32_t w[FEATURE_COUNT];
} Weights;

int eval_evaluate(const Weights *weights, const Board *b);

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

#endif
