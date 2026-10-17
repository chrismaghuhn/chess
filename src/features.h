#ifndef FEATURES_H
#define FEATURES_H

#include "board.h"

/*
 * The evaluation's view of a position: which piece type stands on which
 * square, seen from the side whose piece it is (black squares are mirrored).
 * Own pieces count +1, the opponent's -1. That is just the board itself,
 * no chess knowledge. Engine and tuner both use this one function, so the
 * tuner learns exactly what the engine evaluates.
 */
#define FEATURE_COUNT (6 * 64)
#define MAX_ACTIVE_FEATURES 32

typedef struct {
    uint16_t index; /* type * 64 + square from the owner's view */
    int8_t sign;    /* +1 side to move, -1 opponent */
} Feature;

/* Fills out with the active features of b (side to move's view); returns how many. */
int features_extract(const Board *b, Feature out[MAX_ACTIVE_FEATURES]);

#endif
