#include "nnue.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif

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

/* ---- Integer form ---- */

bool nnue_quantize(const Network *net, QNetwork *q) {
    memset(q, 0, sizeof *q);
    int H = net->hidden, P = (H + NNUE_LANES - 1) / NNUE_LANES * NNUE_LANES;
    /* Largest possible accumulator: b1 plus the 32 largest rows of any unit. */
    double bound = 1;
    for (int h = 0; h < H; h++) {
        float top[NNUE_MAX_ACTIVE] = { 0 };
        for (int i = 0; i < NNUE_INPUTS; i++) {
            float v = fabsf(net->w1[(size_t)i * (size_t)H + (size_t)h]);
            for (int k = 0; k < NNUE_MAX_ACTIVE; k++)
                if (v > top[k]) {
                    float t = top[k];
                    top[k] = v;
                    v = t;
                }
        }
        double sum = fabsf(net->b1[h]);
        for (int k = 0; k < NNUE_MAX_ACTIVE; k++)
            sum += top[k];
        if (sum > bound)
            bound = sum;
    }
    double qa = floor((32767.0 - NNUE_MAX_ACTIVE) / bound); /* each of the 33 rounded terms may add 0.5 */
    if (qa > 4096)
        qa = 4096;
    for (int i = 0; i < NNUE_INPUTS; i++)
        if (!(fabsf(net->skip[i]) * NNUE_SKIP_UNITS < 1e8))
            return false; /* the skip sums must stay far inside int32 */
    /* Output sums: 2 * padded products of at most qa * 32767 must fit in int32. */
    double w2_max = 0;
    for (int h = 0; h < 2 * H; h++)
        w2_max = fmax(w2_max, fabsf(net->w2[h]) * NNUE_QB);
    if (qa < 64 || w2_max > 32767 || 2.0 * P * qa * w2_max > 2e9)
        return false;

    q->hidden = H;
    q->padded = P;
    q->qa = (int16_t)qa;
    q->w1 = aligned_alloc(64, ((size_t)NNUE_INPUTS * (size_t)P * sizeof *q->w1 + 63) / 64 * 64 + 64);
    q->b1 = aligned_alloc(64, ((size_t)P * sizeof *q->b1 + 63) / 64 * 64 + 64);
    q->skip = calloc(NNUE_INPUTS, sizeof *q->skip);
    q->w2 = aligned_alloc(64, ((size_t)2 * (size_t)P * sizeof *q->w2 + 63) / 64 * 64 + 64);
    if (!q->w1 || !q->b1 || !q->skip || !q->w2) {
        nnue_quantized_free(q);
        return false;
    }
    for (int i = 0; i < NNUE_INPUTS; i++) {
        q->skip[i] = (int32_t)lrint(net->skip[i] * NNUE_SKIP_UNITS);
        for (int h = 0; h < P; h++)
            q->w1[(size_t)i * (size_t)P + (size_t)h] =
                (int16_t)(h < H ? lrint(net->w1[(size_t)i * (size_t)H + (size_t)h] * qa) : 0);
    }
    for (int h = 0; h < P; h++) {
        q->b1[h] = (int16_t)(h < H ? lrint(net->b1[h] * qa) : 0);
        for (int k = 0; k < 2; k++)
            q->w2[k * P + h] = (int16_t)(h < H ? lrint(net->w2[k * H + h] * NNUE_QB) : 0);
    }
    /* Check the bound again on the rounded integers: b1 plus the 32 largest rows of every unit. */
    for (int h = 0; h < H; h++) {
        int top[NNUE_MAX_ACTIVE] = { 0 };
        for (int i = 0; i < NNUE_INPUTS; i++) {
            int v = abs(q->w1[(size_t)i * (size_t)P + (size_t)h]);
            for (int k = 0; k < NNUE_MAX_ACTIVE; k++)
                if (v > top[k]) {
                    int t = top[k];
                    top[k] = v;
                    v = t;
                }
        }
        long sum = abs(q->b1[h]);
        for (int k = 0; k < NNUE_MAX_ACTIVE; k++)
            sum += top[k];
        if (sum > 32767) {
            nnue_quantized_free(q);
            return false;
        }
    }
    q->b2 = net->b2 * NNUE_SCALE;
    q->out_scale = (float)(NNUE_SCALE / (qa * NNUE_QB));
    return true;
}

void nnue_quantized_free(QNetwork *q) {
    free(q->w1);
    free(q->b1);
    free(q->skip);
    free(q->w2);
    memset(q, 0, sizeof *q);
}

static inline void acc_add(int16_t *restrict a, const int16_t *restrict row, int P) {
    for (int h = 0; h < P; h += NNUE_LANES)
        for (int k = 0; k < NNUE_LANES; k++)
            a[h + k] = (int16_t)(a[h + k] + row[h + k]);
}

void nnue_refresh(const QNetwork *q, const Board *b, NnueAccumulator *a) {
    int P = q->padded;
    for (int persp = 0; persp < 2; persp++) {
        memcpy(a->acc[persp], q->b1, (size_t)P * sizeof *q->b1);
        a->skip[persp] = 0;
        for (int color = 0; color < 2; color++)
            for (int type = 0; type < 6; type++)
                for (Bitboard bb = b->pieces[color][type]; bb;) {
                    int input = nnue_input(persp, color, type, pop_lsb(&bb));
                    acc_add(a->acc[persp], q->w1 + (size_t)input * (size_t)P, P);
                    a->skip[persp] += q->skip[input];
                }
    }
}

void nnue_update(const QNetwork *q, const Board *before, const Board *after, const NnueAccumulator *from,
                 NnueAccumulator *to) {
    int P = q->padded;
    /* The pieces the move took away and put down (at most 4 of each for one move). */
    int gone[2][NNUE_MAX_ACTIVE], come[2][NNUE_MAX_ACTIVE], ng = 0, nc = 0;
    for (int color = 0; color < 2; color++)
        for (int type = 0; type < 6; type++) {
            Bitboard changed = before->pieces[color][type] ^ after->pieces[color][type];
            for (Bitboard bb = changed & before->pieces[color][type]; bb && ng < NNUE_MAX_ACTIVE; ng++) {
                int sq = pop_lsb(&bb);
                gone[WHITE][ng] = nnue_input(WHITE, color, type, sq);
                gone[BLACK][ng] = nnue_input(BLACK, color, type, sq);
            }
            for (Bitboard bb = changed & after->pieces[color][type]; bb && nc < NNUE_MAX_ACTIVE; nc++) {
                int sq = pop_lsb(&bb);
                come[WHITE][nc] = nnue_input(WHITE, color, type, sq);
                come[BLACK][nc] = nnue_input(BLACK, color, type, sq);
            }
        }
    for (int persp = 0; persp < 2; persp++) {
        int32_t skip = from->skip[persp];
        const int16_t *src = from->acc[persp];
        int16_t *dst = to->acc[persp];
        for (int h = 0; h < P; h += NNUE_LANES) {
            int16_t v[NNUE_LANES];
            for (int k = 0; k < NNUE_LANES; k++)
                v[k] = src[h + k];
            for (int i = 0; i < ng; i++) {
                const int16_t *row = q->w1 + (size_t)gone[persp][i] * (size_t)P + h;
                for (int k = 0; k < NNUE_LANES; k++)
                    v[k] = (int16_t)(v[k] - row[k]);
            }
            for (int i = 0; i < nc; i++) {
                const int16_t *row = q->w1 + (size_t)come[persp][i] * (size_t)P + h;
                for (int k = 0; k < NNUE_LANES; k++)
                    v[k] = (int16_t)(v[k] + row[k]);
            }
            for (int k = 0; k < NNUE_LANES; k++)
                dst[h + k] = v[k];
        }
        for (int i = 0; i < ng; i++)
            skip -= q->skip[gone[persp][i]];
        for (int i = 0; i < nc; i++)
            skip += q->skip[come[persp][i]];
        to->skip[persp] = skip;
    }
}

/* Sum of clamp(acc, 0, qa) * w over padded values, exactly in int32. */
static int32_t dot_clamped(const int16_t *acc, const int16_t *w, int P, int16_t qa) {
#ifdef __AVX2__
    __m256i zero = _mm256_setzero_si256(), top = _mm256_set1_epi16(qa), sum = _mm256_setzero_si256();
    for (int h = 0; h < P; h += 16) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(acc + h));
        x = _mm256_min_epi16(_mm256_max_epi16(x, zero), top);
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(x, _mm256_loadu_si256((const __m256i *)(w + h))));
    }
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
#else
    int32_t sum = 0;
    for (int h = 0; h < P; h++) {
        int16_t x = acc[h] < 0 ? 0 : acc[h] > qa ? qa : acc[h];
        sum += (int32_t)x * w[h];
    }
    return sum;
#endif
}

int nnue_evaluate_quantized(const QNetwork *q, const NnueAccumulator *a, int side) {
    int P = q->padded;
    int32_t sum = dot_clamped(a->acc[side], q->w2, P, q->qa) + dot_clamped(a->acc[!side], q->w2 + P, P, q->qa);
    float score = (float)a->skip[side] / NNUE_SKIP_UNITS + q->b2 + (float)sum * q->out_scale;
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
