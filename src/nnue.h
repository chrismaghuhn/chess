#ifndef NNUE_H
#define NNUE_H

#include <stddef.h>

#include "board.h"
#include "eval.h"

/*
 * A small neural evaluation (experiment, milestone 9). Its inputs are just
 * the board: for each of the two perspectives (side to move, opponent), one
 * input per piece type, square and "own or enemy", squares mirrored for
 * black's perspective (nnue_input). Evaluation, side to move's view:
 *
 *   eval = skip . x_stm  +  scale * (w2 . [clamp(acc_stm), clamp(acc_opp)] + b2)
 *   acc_p = b1 + sum of the w1 rows of p's active inputs (H values), clamp to [0, 1]
 *
 * The skip part is a linear piece-square evaluation (768 weights, own and
 * enemy pieces separately); started from a linear model, the network only
 * has to learn what that misses. hidden = 0 leaves just the linear part.
 * The architecture is chosen by hand, all values are learned. This version
 * computes in float from scratch.
 */
#define NNUE_INPUTS 768
#define NNUE_HIDDEN_MAX 256
#define NNUE_MAX_ACTIVE 32
#define NNUE_SCALE 400.0f

typedef struct Network {
    int hidden;  /* H, 0 = linear only */
    float *skip; /* [NNUE_INPUTS] */
    float *w1;   /* [NNUE_INPUTS][H] */
    float *b1;   /* [H] */
    float *w2;   /* [2H]: side to move's half first */
    float b2;
} Network;

static inline int nnue_input(int perspective, int color, int type, int sq) {
    return (color == perspective ? 0 : 384) + type * 64 + (perspective == WHITE ? sq : sq ^ 56);
}

/* The same piece seen from the other perspective. */
static inline int nnue_flip(int input) {
    return (input < 384 ? input + 384 : input - 384) ^ 56;
}

/* Active inputs of b from the side to move's perspective; returns how many. */
int nnue_inputs(const Board *b, uint16_t out[NNUE_MAX_ACTIVE]);

bool nnue_alloc(Network *net, int hidden);
void nnue_free(Network *net);

/* Skip weights reproducing a linear piece-square evaluation exactly (hidden part zero). */
void nnue_skip_from_weights(Network *net, const Weights *w);

/* Values a forward pass leaves for the backward pass. */
typedef struct {
    float acc[2][NNUE_HIDDEN_MAX];
} NnueCache;

/* Evaluation in float, side to move's view, from the side to move's inputs. */
float nnue_forward(const Network *net, const uint16_t *inputs, int count, NnueCache *cache);

/*
 * Adds d(eval)/d(parameter) * g to grad (same shape as net) for the position
 * of the last nnue_forward with this cache.
 */
void nnue_backward(const Network *net, const uint16_t *inputs, int count, const NnueCache *cache, float g,
                   Network *grad);

/* Engine evaluation: rounded, clamped to EVAL_MAX. */
int nnue_evaluate(const Network *net, const Board *b);

/* "SCHN", version 2, inputs, hidden (uint32 LE), scale (float), then skip, w1, b1, w2, b2 as float32 LE. */
bool nnue_save(const Network *net, const char *path);
bool nnue_load(Network *net, const char *path, char *err, size_t err_size);

#endif
