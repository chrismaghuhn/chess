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
 * The architecture is chosen by hand, all values are learned. Training and
 * nnue_evaluate compute in float from scratch; the search uses the integer
 * form below.
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

/*
 * Fast integer form for the search (milestone 14). Per color perspective, an
 * accumulator holds b1 + the w1 rows of that perspective's active inputs as
 * int16 (QA units per 1.0, QA chosen per network so no sum can overflow),
 * plus the skip sum (64 units per evaluation unit). The search keeps one per
 * ply and updates it from the parent with just the pieces a move changed.
 * The output layer uses int16 weights (NNUE_QB units per 1.0) and int32
 * sums. Equal to nnue_evaluate up to rounding (a few units: at most 4 seen
 * on the milestone 13 networks).
 */
#define NNUE_SKIP_UNITS 64
#define NNUE_LANES 16 /* hidden sizes are padded to a multiple of this */
#define NNUE_QB 4096

typedef struct {
    int hidden, padded;
    int16_t qa;     /* the clipped ReLU's 1.0 */
    int16_t *w1;    /* [NNUE_INPUTS][padded] */
    int16_t *b1;    /* [padded] */
    int32_t *skip;  /* [NNUE_INPUTS] */
    int16_t *w2;    /* [2][padded], times NNUE_QB */
    float b2;       /* times NNUE_SCALE */
    float out_scale; /* NNUE_SCALE / (qa * NNUE_QB) */
} QNetwork;

typedef struct {
    _Alignas(64) int16_t acc[2][NNUE_HIDDEN_MAX]; /* [perspective color] */
    int32_t skip[2];
} NnueAccumulator;

/* False if the network cannot be stored this way (weights too large). */
bool nnue_quantize(const Network *net, QNetwork *q);
void nnue_quantized_free(QNetwork *q);

/* Accumulators of b from scratch. */
void nnue_refresh(const QNetwork *q, const Board *b, NnueAccumulator *a);

/* Accumulators of after (any position reached by one move or a null move from before), from those of before. */
void nnue_update(const QNetwork *q, const Board *before, const Board *after, const NnueAccumulator *from,
                 NnueAccumulator *to);

/* Evaluation of the position whose accumulators these are, side to move's view; like nnue_evaluate. */
int nnue_evaluate_quantized(const QNetwork *q, const NnueAccumulator *a, int side);

/* "SCHN", version 2, inputs, hidden (uint32 LE), scale (float), then skip, w1, b1, w2, b2 as float32 LE. */
bool nnue_save(const Network *net, const char *path);
bool nnue_load(Network *net, const char *path, char *err, size_t err_size);

#endif
