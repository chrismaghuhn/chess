#include "eval.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int eval_evaluate(const Weights *weights, const Board *b) {
    Feature features[MAX_ACTIVE_FEATURES];
    int count = features_extract(b, features);

    int score = 0;
    for (int i = 0; i < count; i++)
        score += features[i].sign * weights->w[features[i].index];

    if (score > EVAL_MAX)
        return EVAL_MAX;
    if (score < -EVAL_MAX)
        return -EVAL_MAX;
    return score;
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
    if (get_u32(buf + 4) != WEIGHTS_VERSION || get_u32(buf + 8) != FEATURE_COUNT) {
        snprintf(err, err_size, "%s: unsupported version %u or feature count %u", path,
                 (unsigned)get_u32(buf + 4), (unsigned)get_u32(buf + 8));
        return false;
    }
    if (size != 12 + 4 * FEATURE_COUNT) {
        snprintf(err, err_size, "%s: wrong size %zu", path, size);
        return false;
    }
    for (int i = 0; i < FEATURE_COUNT; i++) {
        uint32_t bits = get_u32(buf + 12 + 4 * i);
        memcpy(&out[i], &bits, 4);
        if (!isfinite(out[i])) {
            snprintf(err, err_size, "%s: weight %d is not a number", path, i);
            return false;
        }
    }
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
