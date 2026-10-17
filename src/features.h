#ifndef FEATURES_H
#define FEATURES_H

#include "board.h"

/*
 * The evaluation's view of a position. Every feature exists twice, for the
 * middlegame and the endgame, and the two are mixed by the game phase: the
 * number of pieces other than pawns and kings, all counted alike. With p
 * such pieces (at most PHASE_MAX), a middlegame feature contributes
 * p / PHASE_MAX of its weight and its endgame twin (PHASE_MAX - p) / PHASE_MAX.
 *
 * Which features exist is chosen by hand; what they are worth is learned.
 * Each group is a structural assumption and is listed in PLAN.md:
 *   - piece-square: piece type on a square, seen from its owner (the board itself)
 *   - mobility: how many squares a knight, bishop, rook or queen can move to
 *     (empty or enemy-occupied), one weight per count
 *   - passed pawn by rank (no enemy pawn ahead on its file or the next files),
 *     doubled pawn (own pawn ahead on the same file), isolated pawn (no own
 *     pawn on the next files), bishop pair
 *
 * Features of the side to move count positively, the opponent's negatively.
 * Engine and tuner both use these definitions; tests check that the
 * engine's fast evaluation equals the sum over features_extract.
 */
#define PHASE_MAX 14

enum {
    MOB_KNIGHT = 0,      /* 0..8 squares */
    MOB_BISHOP = 9,      /* 0..13 */
    MOB_ROOK = 23,       /* 0..14 */
    MOB_QUEEN = 38,      /* 0..27 */
    MOBILITY_SIZE = 66,
};

enum {
    F_PSQ = 0,                                 /* 6 * 64 */
    F_MOBILITY = F_PSQ + 6 * 64,               /* MOBILITY_SIZE */
    F_PASSED = F_MOBILITY + MOBILITY_SIZE,     /* relative rank 2..7 -> 0..5 */
    F_DOUBLED = F_PASSED + 6,
    F_ISOLATED,
    F_BISHOP_PAIR,
    PHASED_FEATURES,                           /* features per phase */
    FEATURE_EG = PHASED_FEATURES,              /* offset of the endgame twins */
    FEATURE_COUNT = 2 * PHASED_FEATURES,
};

/* A piece-square-only weight file from before the phase features had this many weights. */
#define LEGACY_FEATURE_COUNT (6 * 64)

#define MAX_ACTIVE_FEATURES 256

typedef struct {
    uint16_t index;
    int8_t value; /* phase share in 1/PHASE_MAX, negative for the opponent */
} Feature;

static inline int board_phase(const Board *b) {
    int pieces = popcount(b->all & ~(b->pieces[WHITE][PAWN] | b->pieces[BLACK][PAWN]
                                     | b->pieces[WHITE][KING] | b->pieces[BLACK][KING]));
    return pieces < PHASE_MAX ? pieces : PHASE_MAX;
}

static inline int mobility_offset(int type) {
    static const int offsets[6] = { -1, MOB_KNIGHT, MOB_BISHOP, MOB_ROOK, MOB_QUEEN, -1 };
    return offsets[type];
}

/*
 * Fills out with the active features of b from the side to move's view
 * (features with a zero share are left out); returns how many. The
 * evaluation is sum(value * weight) / PHASE_MAX.
 */
int features_extract(const Board *b, Feature out[MAX_ACTIVE_FEATURES]);

#endif
