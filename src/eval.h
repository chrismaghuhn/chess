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
 * The search keeps the piece-square part incrementally: the middlegame and
 * endgame sums from white's view, unclamped. eval_psq_delta gives their
 * change for a move (from the board before the move). eval_static adds the
 * rest (mobility, pawns, bishop pair), mixes the phases and clamps:
 * eval_static(w, b, eval_psq(w, b)) == eval_evaluate(w, b).
 */
typedef struct {
    int mg, eg;
} Psq;

Psq eval_psq(const Weights *weights, const Board *b);
Psq eval_psq_delta(const Weights *weights, const Board *before, Move m);
int eval_static(const Weights *weights, const Board *b, Psq psq);

/* Mixes white's-view phase sums by the game phase; side to move's view, clamped. */
int eval_mix(const Board *b, int mg, int eg);

/*
 * Whether any non-piece-square weight is nonzero. Without, eval_static equals
 * eval_mix(b, psq.mg, psq.eg), which the search uses to skip the expensive part.
 */
bool weights_have_structure(const Weights *weights);

static inline Psq psq_add(Psq a, Psq b) {
    Psq sum = { a.mg + b.mg, a.eg + b.eg };
    return sum;
}

static inline bool psq_equal(Psq a, Psq b) {
    return a.mg == b.mg && a.eg == b.eg;
}

/*
 * Weight files: "SCHW", version (uint32), feature count (uint32), then one
 * float32 per feature, all little-endian. The engine rounds to integers;
 * the tuner keeps floats. Files with LEGACY_FEATURE_COUNT weights (piece-square
 * only, from before the phase features) load into both phases, which gives
 * exactly their old evaluation.
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
