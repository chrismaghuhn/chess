#include "nnue.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eval.h"

bool nnue_alloc(Network *net, int hidden) {
    memset(net, 0, sizeof *net);
    if (hidden < 1 || hidden > NNUE_HIDDEN_MAX)
        return false;
    net->hidden = hidden;
    net->w1 = calloc((size_t)NNUE_INPUTS * (size_t)hidden, sizeof *net->w1);
    net->b1 = calloc((size_t)hidden, sizeof *net->b1);
    net->w2 = calloc(2 * (size_t)hidden, sizeof *net->w2);
    if (!net->w1 || !net->b1 || !net->w2) {
        nnue_free(net);
        return false;
    }
    return true;
}

void nnue_free(Network *net) {
    free(net->w1);
    free(net->b1);
    free(net->w2);
    memset(net, 0, sizeof *net);
}

int nnue_evaluate(const Network *net, const Board *b) {
    int H = net->hidden;
    float acc[2][NNUE_HIDDEN_MAX];
    int perspectives[2] = { b->side, b->side ^ 1 };

    for (int k = 0; k < 2; k++) {
        float *a = acc[k];
        memcpy(a, net->b1, (size_t)H * sizeof *a);
        for (int color = 0; color < 2; color++) {
            for (int type = 0; type < 6; type++) {
                for (Bitboard bb = b->pieces[color][type]; bb;) {
                    const float *row = net->w1 + (size_t)nnue_input(perspectives[k], color, type, pop_lsb(&bb)) * (size_t)H;
                    for (int h = 0; h < H; h++)
                        a[h] += row[h];
                }
            }
        }
    }

    float out = net->b2;
    for (int k = 0; k < 2; k++) {
        const float *w = net->w2 + k * H;
        for (int h = 0; h < H; h++) {
            float x = acc[k][h] < 0 ? 0 : acc[k][h] > 1 ? 1 : acc[k][h];
            out += w[h] * x;
        }
    }
    float score = out * NNUE_OUTPUT_SCALE;
    if (!(score < EVAL_MAX))
        return EVAL_MAX;
    if (!(score > -EVAL_MAX))
        return -EVAL_MAX;
    return (int)lrintf(score);
}

static const char net_magic[4] = { 'S', 'C', 'H', 'N' };

static bool write_floats(FILE *f, const float *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], 4);
        unsigned char le[4] = { (unsigned char)bits, (unsigned char)(bits >> 8), (unsigned char)(bits >> 16),
                                (unsigned char)(bits >> 24) };
        if (fwrite(le, 1, 4, f) != 4)
            return false;
    }
    return true;
}

static bool read_floats(FILE *f, float *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char le[4];
        if (fread(le, 1, 4, f) != 4)
            return false;
        uint32_t bits = (uint32_t)le[0] | (uint32_t)le[1] << 8 | (uint32_t)le[2] << 16 | (uint32_t)le[3] << 24;
        memcpy(&v[i], &bits, 4);
        if (!isfinite(v[i]))
            return false;
    }
    return true;
}

static bool write_u32(FILE *f, uint32_t v) {
    unsigned char le[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return fwrite(le, 1, 4, f) == 4;
}

static bool read_u32(FILE *f, uint32_t *v) {
    unsigned char le[4];
    if (fread(le, 1, 4, f) != 4)
        return false;
    *v = (uint32_t)le[0] | (uint32_t)le[1] << 8 | (uint32_t)le[2] << 16 | (uint32_t)le[3] << 24;
    return true;
}

bool nnue_save(const Network *net, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t H = (size_t)net->hidden;
    bool ok = fwrite(net_magic, 1, 4, f) == 4 && write_u32(f, 1) && write_u32(f, NNUE_INPUTS)
           && write_u32(f, (uint32_t)H) && write_floats(f, net->w1, NNUE_INPUTS * H)
           && write_floats(f, net->b1, H) && write_floats(f, net->w2, 2 * H) && write_floats(f, &net->b2, 1);
    return fclose(f) == 0 && ok;
}

bool nnue_load(Network *net, const char *path, char *err, size_t err_size) {
    memset(net, 0, sizeof *net);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "cannot open %s", path);
        return false;
    }
    char magic[4];
    uint32_t version, inputs, hidden;
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, net_magic, 4) == 0 && read_u32(f, &version)
           && read_u32(f, &inputs) && read_u32(f, &hidden) && version == 1 && inputs == NNUE_INPUTS
           && nnue_alloc(net, (int)hidden);
    if (ok) {
        size_t H = hidden;
        ok = read_floats(f, net->w1, NNUE_INPUTS * H) && read_floats(f, net->b1, H)
          && read_floats(f, net->w2, 2 * H) && read_floats(f, &net->b2, 1) && fgetc(f) == EOF;
        if (!ok)
            nnue_free(net);
    }
    fclose(f);
    if (!ok)
        snprintf(err, err_size, "%s is not a valid network file", path);
    return ok;
}
