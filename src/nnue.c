#include "nnue.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nnue_inputs(const Board *b, uint16_t out[NNUE_MAX_ACTIVE]) {
    int count = 0;
    for (int color = 0; color < 2; color++)
        for (int type = 0; type < 6; type++)
            for (Bitboard bb = b->pieces[color][type]; bb;)
                out[count++] = (uint16_t)nnue_input(b->side, color, type, pop_lsb(&bb));
    return count;
}

bool nnue_alloc(Network *net, int hidden) {
    memset(net, 0, sizeof *net);
    if (hidden < 0 || hidden > NNUE_HIDDEN_MAX)
        return false;
    size_t H = (size_t)hidden;
    net->hidden = hidden;
    net->skip = calloc(NNUE_INPUTS, sizeof *net->skip);
    net->w1 = calloc(NNUE_INPUTS * H + 1, sizeof *net->w1);
    net->b1 = calloc(H + 1, sizeof *net->b1);
    net->w2 = calloc(2 * H + 1, sizeof *net->w2);
    if (!net->skip || !net->w1 || !net->b1 || !net->w2) {
        nnue_free(net);
        return false;
    }
    return true;
}

void nnue_free(Network *net) {
    free(net->skip);
    free(net->w1);
    free(net->b1);
    free(net->w2);
    memset(net, 0, sizeof *net);
}

void nnue_skip_from_weights(Network *net, const Weights *w) {
    /* Own piece on (stm-relative) square s: +w; enemy piece: -w seen from its owner, i.e. mirrored. */
    for (int type = 0; type < 6; type++) {
        for (int s = 0; s < 64; s++) {
            net->skip[type * 64 + s] = (float)w->w[type * 64 + s];
            net->skip[384 + type * 64 + s] = -(float)w->w[type * 64 + (s ^ 56)];
        }
    }
}

float nnue_forward(const Network *net, const uint16_t *inputs, int count, NnueCache *cache) {
    int H = net->hidden;
    float linear = 0;
    for (int i = 0; i < count; i++)
        linear += net->skip[inputs[i]];
    if (H == 0)
        return linear;

    for (int k = 0; k < 2; k++) {
        float *a = cache->acc[k];
        memcpy(a, net->b1, (size_t)H * sizeof *a);
        for (int i = 0; i < count; i++) {
            int input = k == 0 ? inputs[i] : nnue_flip(inputs[i]);
            const float *row = net->w1 + (size_t)input * (size_t)H;
            for (int h = 0; h < H; h++)
                a[h] += row[h];
        }
    }
    float out = net->b2;
    for (int k = 0; k < 2; k++) {
        const float *w = net->w2 + k * H;
        for (int h = 0; h < H; h++) {
            float x = cache->acc[k][h];
            out += w[h] * (x < 0 ? 0 : x > 1 ? 1 : x);
        }
    }
    return linear + NNUE_SCALE * out;
}

void nnue_backward(const Network *net, const uint16_t *inputs, int count, const NnueCache *cache, float g,
                   Network *grad) {
    int H = net->hidden;
    for (int i = 0; i < count; i++)
        grad->skip[inputs[i]] += g;
    if (H == 0)
        return;

    float go = g * NNUE_SCALE;
    grad->b2 += go;
    float dacc[2][NNUE_HIDDEN_MAX];
    for (int k = 0; k < 2; k++) {
        const float *w = net->w2 + k * H;
        float *gw = grad->w2 + k * H;
        for (int h = 0; h < H; h++) {
            float x = cache->acc[k][h];
            bool inside = x > 0 && x < 1;
            gw[h] += go * (x < 0 ? 0 : x > 1 ? 1 : x);
            dacc[k][h] = inside ? go * w[h] : 0;
            grad->b1[h] += dacc[k][h];
        }
    }
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < count; i++) {
            int input = k == 0 ? inputs[i] : nnue_flip(inputs[i]);
            float *row = grad->w1 + (size_t)input * (size_t)H;
            for (int h = 0; h < H; h++)
                row[h] += dacc[k][h];
        }
    }
}

int nnue_evaluate(const Network *net, const Board *b) {
    uint16_t inputs[NNUE_MAX_ACTIVE];
    NnueCache cache;
    float score = nnue_forward(net, inputs, nnue_inputs(b, inputs), &cache);
    if (!(score < EVAL_MAX))
        return EVAL_MAX;
    if (!(score > -EVAL_MAX))
        return -EVAL_MAX;
    return (int)lrintf(score);
}

static const char net_magic[4] = { 'S', 'C', 'H', 'N' };
#define NET_VERSION 2

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

static bool write_floats(FILE *f, const float *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], 4);
        if (!write_u32(f, bits))
            return false;
    }
    return true;
}

static bool read_floats(FILE *f, float *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        if (!read_u32(f, &bits))
            return false;
        memcpy(&v[i], &bits, 4);
        if (!isfinite(v[i]))
            return false;
    }
    return true;
}

bool nnue_save(const Network *net, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t H = (size_t)net->hidden;
    float scale = NNUE_SCALE;
    bool ok = fwrite(net_magic, 1, 4, f) == 4 && write_u32(f, NET_VERSION) && write_u32(f, NNUE_INPUTS)
           && write_u32(f, (uint32_t)H) && write_floats(f, &scale, 1) && write_floats(f, net->skip, NNUE_INPUTS)
           && write_floats(f, net->w1, NNUE_INPUTS * H) && write_floats(f, net->b1, H)
           && write_floats(f, net->w2, 2 * H) && write_floats(f, &net->b2, 1);
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
    float scale;
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, net_magic, 4) == 0 && read_u32(f, &version)
           && version == NET_VERSION && read_u32(f, &inputs) && inputs == NNUE_INPUTS && read_u32(f, &hidden)
           && read_floats(f, &scale, 1) && scale == NNUE_SCALE && nnue_alloc(net, (int)hidden);
    if (ok) {
        size_t H = hidden;
        ok = read_floats(f, net->skip, NNUE_INPUTS) && read_floats(f, net->w1, NNUE_INPUTS * H)
          && read_floats(f, net->b1, H) && read_floats(f, net->w2, 2 * H) && read_floats(f, &net->b2, 1)
          && fgetc(f) == EOF;
        if (!ok)
            nnue_free(net);
    }
    fclose(f);
    if (!ok)
        snprintf(err, err_size, "%s is not a valid network file", path);
    return ok;
}
