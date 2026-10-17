#ifndef NNUE_H
#define NNUE_H

#include <stddef.h>

#include "board.h"

/*
 * A small neural evaluation (experiment, milestone 9). Its inputs are just
 * the board: for each of the two perspectives (side to move, opponent), one
 * input per piece type, square and "own or enemy", squares mirrored for
 * black's perspective. Both perspectives share the first layer:
 *
 *   acc_p = b1 + sum of w1 rows of p's active inputs       (H values each)
 *   h     = clamp(acc_stm, 0, 1) ++ clamp(acc_opponent, 0, 1)
 *   eval  = NNUE_OUTPUT_SCALE * (w2 . h + b2)              (side to move's view)
 *
 * The architecture is chosen by hand, all values are learned. This version
 * evaluates in float from scratch; an incremental, quantized one would come
 * only if the network proves worth it.
 */
#define NNUE_INPUTS 768
#define NNUE_HIDDEN_MAX 512
#define NNUE_OUTPUT_SCALE 400.0f

typedef struct {
    int hidden;  /* H */
    float *w1;   /* [NNUE_INPUTS][H] */
    float *b1;   /* [H] */
    float *w2;   /* [2H]: side to move's half first */
    float b2;
} Network;

static inline int nnue_input(int perspective, int color, int type, int sq) {
    return (color == perspective ? 0 : 384) + type * 64 + (perspective == WHITE ? sq : sq ^ 56);
}

bool nnue_alloc(Network *net, int hidden);
void nnue_free(Network *net);

/* Side to move's view, clamped to EVAL_MAX like the linear evaluation. */
int nnue_evaluate(const Network *net, const Board *b);

/* "SCHN", version, inputs, hidden (uint32 LE), then w1, b1, w2, b2 as float32 LE. */
bool nnue_save(const Network *net, const char *path);
bool nnue_load(Network *net, const char *path, char *err, size_t err_size);

#endif
