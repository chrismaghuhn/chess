#include "eval.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "attacks.h"

/* Weight of a piece of `color` on sq, seen from its owner, in one phase. */
static inline int psq_weight(const Weights *w, int phase_offset, int color, int type, int sq) {
    return w->w[phase_offset + F_PSQ + type * 64 + (color == WHITE ? sq : sq ^ 56)];
}

Psq eval_psq(const Weights *weights, const Board *b) {
    Psq psq = { 0, 0 };
    for (int color = 0; color < 2; color++) {
        int sign = color == WHITE ? 1 : -1;
        for (int type = 0; type < 6; type++) {
            for (Bitboard bb = b->pieces[color][type]; bb;) {
                int sq = pop_lsb(&bb);
                psq.mg += sign * psq_weight(weights, 0, color, type, sq);
                psq.eg += sign * psq_weight(weights, FEATURE_EG, color, type, sq);
            }
        }
    }
    return psq;
}

/* Change of one phase's piece-square sum, from the mover's view. */
static int psq_delta_phase(const Weights *w, int ph, const Board *b, Move m) {
    int us = b->side, them = us ^ 1;
    int from = move_from(m), to = move_to(m), flags = move_flags(m);
    int type = piece_type(b->squares[from]);

    int own = psq_weight(w, ph, us, type, to) - psq_weight(w, ph, us, type, from), theirs = 0;
    if (flags == MF_EP_CAPTURE)
        theirs = psq_weight(w, ph, them, PAWN, to ^ 8);
    else if (flags & MF_CAPTURE)
        theirs = psq_weight(w, ph, them, piece_type(b->squares[to]), to);
    if (flags & MF_PROMO)
        own += psq_weight(w, ph, us, move_promo_type(m), to) - psq_weight(w, ph, us, PAWN, to);
    else if (flags == MF_KING_CASTLE)
        own += psq_weight(w, ph, us, ROOK, to - 1) - psq_weight(w, ph, us, ROOK, to + 1);
    else if (flags == MF_QUEEN_CASTLE)
        own += psq_weight(w, ph, us, ROOK, to + 1) - psq_weight(w, ph, us, ROOK, to - 2);

    return own + theirs; /* a captured piece no longer counts against us */
}

Psq eval_psq_delta(const Weights *w, const Board *b, Move m) {
    int sign = b->side == WHITE ? 1 : -1;
    Psq delta = { sign * psq_delta_phase(w, 0, b, m), sign * psq_delta_phase(w, FEATURE_EG, b, m) };
    return delta;
}

/* Adds one feature's middlegame and endgame weights. */
static inline void add_weight(const Weights *w, int index, int *mg, int *eg) {
    *mg += w->w[index];
    *eg += w->w[FEATURE_EG + index];
}

/* Everything but piece-square for one color, added to its two phase sums (white's view). */
static void add_structure(const Weights *w, const Board *b, int color, int *mg, int *eg) {
    int sign = color == WHITE ? 1 : -1, mirror = color == WHITE ? 0 : 56;
    const Bitboard *own = b->pieces[color];
    Bitboard own_all = b->occupied[color], enemy_pawns = b->pieces[color ^ 1][PAWN];
    int sum_mg = 0, sum_eg = 0;

    for (int type = KNIGHT; type <= QUEEN; type++) {
        for (Bitboard bb = own[type]; bb;) {
            int sq = pop_lsb(&bb);
            Bitboard reach = type == KNIGHT ? knight_attacks[sq]
                           : type == BISHOP ? bishop_attacks(sq, b->all)
                           : type == ROOK ? rook_attacks(sq, b->all)
                           : queen_attacks(sq, b->all);
            add_weight(w, F_MOBILITY + mobility_offset(type) + popcount(reach & ~own_all), &sum_mg, &sum_eg);
        }
    }

    for (Bitboard bb = own[PAWN]; bb;) {
        int sq = pop_lsb(&bb);
        if (!(passed_span_bb[color][sq] & enemy_pawns))
            add_weight(w, F_PASSED + rank_of(sq ^ mirror) - 1, &sum_mg, &sum_eg);
        if (forward_file_bb[color][sq] & own[PAWN])
            add_weight(w, F_DOUBLED, &sum_mg, &sum_eg);
        if (!(adjacent_files_bb[file_of(sq)] & own[PAWN]))
            add_weight(w, F_ISOLATED, &sum_mg, &sum_eg);
    }
    if (popcount(own[BISHOP]) >= 2)
        add_weight(w, F_BISHOP_PAIR, &sum_mg, &sum_eg);

    *mg += sign * sum_mg;
    *eg += sign * sum_eg;
}

int eval_static(const Weights *weights, const Board *b, Psq psq) {
    int mg = psq.mg, eg = psq.eg;
    add_structure(weights, b, WHITE, &mg, &eg);
    add_structure(weights, b, BLACK, &mg, &eg);
    return eval_mix(b, mg, eg);
}

int eval_mix(const Board *b, int mg, int eg) {
    int phase = board_phase(b);
    int total = (mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX; /* white's view */
    int score = b->side == WHITE ? total : -total;
    return score > EVAL_MAX ? EVAL_MAX : score < -EVAL_MAX ? -EVAL_MAX : score;
}

bool weights_have_structure(const Weights *weights) {
    for (int phase = 0; phase < 2; phase++)
        for (int i = F_MOBILITY; i < PHASED_FEATURES; i++)
            if (weights->w[phase * FEATURE_EG + i])
                return true;
    return false;
}

int eval_evaluate(const Weights *weights, const Board *b) {
    return eval_static(weights, b, eval_psq(weights, b));
}

void weights_zero(Weights *weights) {
    memset(weights, 0, sizeof *weights);
}

static const char weights_magic[4] = { 'S', 'C', 'H', 'W' };

static void put_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool weights_save_float(const char *path, const float weights[FEATURE_COUNT]) {
    unsigned char buf[12 + 4 * FEATURE_COUNT];
    memcpy(buf, weights_magic, 4);
    put_u32(buf + 4, WEIGHTS_VERSION);
    put_u32(buf + 8, FEATURE_COUNT);
    for (int i = 0; i < FEATURE_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &weights[i], 4);
        put_u32(buf + 12 + 4 * i, bits);
    }

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    return fclose(f) == 0 && ok;
}

bool weights_load_float(const char *path, float out[FEATURE_COUNT], char *err, size_t err_size) {
    unsigned char buf[12 + 4 * FEATURE_COUNT + 1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "cannot open %s", path);
        return false;
    }
    size_t size = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (size < 12 || memcmp(buf, weights_magic, 4) != 0) {
        snprintf(err, err_size, "%s is not a weight file", path);
        return false;
    }
    uint32_t count = get_u32(buf + 8);
    if (get_u32(buf + 4) != WEIGHTS_VERSION || (count != FEATURE_COUNT && count != LEGACY_FEATURE_COUNT)) {
        snprintf(err, err_size, "%s: unsupported version %u or feature count %u", path,
                 (unsigned)get_u32(buf + 4), (unsigned)count);
        return false;
    }
    if (size != 12 + 4 * (size_t)count) {
        snprintf(err, err_size, "%s: wrong size %zu", path, size);
        return false;
    }
    memset(out, 0, FEATURE_COUNT * sizeof out[0]);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bits = get_u32(buf + 12 + 4 * i);
        memcpy(&out[i], &bits, 4);
        if (!isfinite(out[i])) {
            snprintf(err, err_size, "%s: weight %u is not a number", path, (unsigned)i);
            return false;
        }
    }
    /*
     * An old piece-square-only file: the same weights in both phases, so
     * that every phase mix gives the old evaluation exactly.
     */
    if (count == LEGACY_FEATURE_COUNT)
        for (int i = 0; i < LEGACY_FEATURE_COUNT; i++)
            out[FEATURE_EG + F_PSQ + i] = out[F_PSQ + i];
    return true;
}

void weights_from_float(const float in[FEATURE_COUNT], Weights *out) {
    for (int i = 0; i < FEATURE_COUNT; i++) {
        float v = in[i] > WEIGHT_MAX ? WEIGHT_MAX : in[i] < -WEIGHT_MAX ? -WEIGHT_MAX : in[i];
        out->w[i] = (int32_t)lrintf(v);
    }
}

bool weights_load(const char *path, Weights *weights, char *err, size_t err_size) {
    float w[FEATURE_COUNT];
    if (!weights_load_float(path, w, err, err_size))
        return false;
    weights_from_float(w, weights);
    return true;
}
