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

/* Packed feature: index in the low 15 bits, the top bit set for -1. */
typedef uint16_t PackedFeature;
#define FEATURE_NEGATIVE 0x8000

typedef struct {
    size_t count;
    size_t train_count;
    PackedFeature (*features)[MAX_ACTIVE_FEATURES];
    uint8_t *feature_count;
    float *target;     /* result for the side to move: 0, 0.5 or 1 */
    bool *validation;  /* every val_every-th game goes to validation */
    uint64_t seen[FEATURE_COUNT]; /* training positions using each feature */
    size_t unresolved;            /* positions whose quiet leaf was not found in time */
    size_t scored;                /* positions whose target includes a search score */
    double lambda;
    uint64_t resolve_nodes;       /* quiescence nodes spent finding quiet leaves */
    size_t cached;                /* positions whose quiet leaves came from a .leaves cache */
    bool *needs_resolving;        /* during loading only */
} TuneData;

/*
 * Loads training data and resolves every position to its quiet leaf with
 * the given weights (what the engine with those weights would evaluate).
 *
 * The target mixes the self-play search score into the game result where
 * the data has a valid score (version 2, not generation 0):
 *   target = lambda * sigmoid(score) + (1 - lambda) * result,
 * and uses the score alone for games cut at the ply limit, whose "draw" is
 * not a real result. lambda = 0 trains on results only, as before.
 *
 * With leaf_cache, the quiet leaves of each data file are stored next to it
 * (FILE.leaves, checked against a hash of the file's records) and reused by
 * later runs instead of being resolved again. A file is resolved once, with
 * the weights of the first tuning that reads it – for self-play data that
 * is the generation that played it – so older data keeps somewhat older
 * leaves; that is the price for not redoing the work every generation.
 */
bool tune_load(TuneData *d, char *const *paths, int path_count, int val_every, double lambda,
               bool leaf_cache, const Weights *resolve_with, int threads, char *err, size_t err_size);
void tune_free(TuneData *d);

typedef struct {
    int epochs;
    double learning_rate;
    int threads;
    int report_every; /* print losses to stderr every n epochs; 0 = never */
    int patience;     /* stop after this many epochs without a better validation loss; 0 = never */
} TuneConfig;

typedef struct {
    double start_train_loss, start_val_loss;
    double train_loss, val_loss; /* at the best epoch */
    int best_epoch;              /* by validation loss, or the last one without validation */
    int epochs_run;
} TuneResult;

/* Full-batch Adam on w (updated in place). Deterministic for any thread count. */
TuneResult tune_run(const TuneData *d, float w[FEATURE_COUNT], const TuneConfig *config);

#endif
