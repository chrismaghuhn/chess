#ifndef TEXEL_H
#define TEXEL_H

#include <stddef.h>

#include "eval.h"

/*
 * Texel tuning: fit the linear evaluation to game results. The predicted
 * score of a position is sigmoid(eval * SIGMOID_SCALE), compared with the
 * result (0, 0.5, 1) by squared error. The scale makes 400 units a 10:1
 * win ratio, so the learned units read roughly like centipawns.
 */
#define SIGMOID_SCALE 0.005756462732485115 /* ln(10) / 400 */

typedef struct {
    size_t count;
    size_t train_count;
    /* Features of position i are index/value[offset[i] .. offset[i+1]). */
    uint64_t *offset;
    uint16_t *index;
    int8_t *value;     /* phase share (see features.h), signed from the labelled side's view */
    float *target;     /* result for the side to move: 0, 0.5 or 1 */
    bool *validation;  /* every val_every-th game goes to validation */
    uint64_t seen[FEATURE_COUNT]; /* training positions using each feature */
    size_t unresolved;            /* positions whose quiet leaf was not found in time */
    uint64_t resolve_nodes;       /* quiescence nodes spent finding quiet leaves */
} TuneData;

/*
 * Loads training data and resolves every position to its quiet leaf with
 * the given weights (what the engine with those weights would evaluate).
 */
bool tune_load(TuneData *d, char *const *paths, int path_count, int val_every,
               const Weights *resolve_with, int threads, char *err, size_t err_size);
void tune_free(TuneData *d);

typedef struct {
    int epochs;
    double learning_rate;
    int threads;
    int report_every; /* print losses to stderr every n epochs; 0 = never */
    double l2;        /* adds l2 * sum(w^2) to the training loss; 0 = none */
} TuneConfig;

typedef struct {
    double start_train_loss, start_val_loss;
    double train_loss, val_loss; /* at the best epoch */
    int best_epoch;              /* by validation loss, or the last one without validation */
} TuneResult;

/* Full-batch Adam on w (updated in place). Deterministic for any thread count. */
TuneResult tune_run(const TuneData *d, float w[FEATURE_COUNT], const TuneConfig *config);

#endif
