#define _POSIX_C_SOURCE 200809L

/*
 * nnue-train: trains the experimental network (src/nnue.h) on self-play data.
 *
 *   nnue-train --out FILE --resolve WEIGHTS --data FILE [--data FILE ...]
 *              [--init-linear WEIGHTS] [--hidden H] [--epochs N] [--batch N] [--lr X]
 *              [--lr-linear X] [--wd X] [--train-games N] [--threads N] [--seed N]
 *
 * Positions are resolved to quiet leaves with the --resolve linear weights,
 * as in the linear tuner. Split by the game number inside each file: % 10 == 0
 * test (reported only), == 5 validation (picks the epoch), rest training, so
 * games that share an opening across runs land in the same part.
 * --train-games keeps only that many training games, evenly spread (for the
 * learning curve). --hidden 0 trains the linear part alone with the same
 * pipeline, as the baseline.
 */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "datafmt.h"
#include "nnue.h"
#include "rng.h"
#include "search.h"

#define SIGMOID_SCALE 0.005756462732485115 /* ln(10) / 400, as in the linear tuner */
#define CHUNKS 32
#define QUIET_MAX_NODES 100000
#define MAX_FILES 256
#define WEIGHT_LIMIT 2.0f /* |w1|, |w2| stay within what a later int16 version can hold */

enum { TRAIN, VALIDATION, TEST };

typedef struct {
    size_t count;
    uint64_t *offset; /* inputs of position i: input[offset[i] .. offset[i+1]) */
    uint16_t *input;  /* side to move's perspective of the quiet leaf */
    float *target;    /* result for the leaf's side to move: 0, 0.5, 1 */
    uint8_t *split;
    size_t in_split[3];
    size_t games[3];
} NetData;

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static double clock_seconds(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- Loading ---- */

typedef struct {
    NetData *d;
    const uint8_t (*records)[DATA_RECORD_SIZE];
    const Weights *weights;
    size_t begin, end;
    uint16_t *input;
    size_t length, capacity;
    uint8_t *counts;
} Job;

static void *resolve_worker(void *arg) {
    Job *job = arg;
    Search *s = search_new(1);
    search_set_weights(s, job->weights);
    job->counts = xcalloc(job->end - job->begin, 1);
    for (size_t i = job->begin; i < job->end; i++) {
        DataPosition pos;
        if (!data_unpack(job->records[i], &pos)) {
            fprintf(stderr, "invalid record %zu\n", i);
            exit(1);
        }
        Board leaf;
        search_quiet(s, &pos.board, QUIET_MAX_NODES, &leaf, NULL, NULL);
        uint16_t in[NNUE_MAX_ACTIVE];
        int n = nnue_inputs(&leaf, in);
        if (job->length + (size_t)n > job->capacity) {
            job->capacity = (job->capacity + (size_t)n) * 2;
            job->input = realloc(job->input, job->capacity * sizeof *job->input);
            if (!job->input) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
        }
        memcpy(job->input + job->length, in, (size_t)n * sizeof *in);
        job->length += (size_t)n;
        job->counts[i - job->begin] = (uint8_t)n;
        float y = (float)pos.result / 2.0f;
        job->d->target[i] = leaf.side == pos.board.side ? y : 1.0f - y;
    }
    search_free(s);
    return NULL;
}

static bool load(NetData *d, char **paths, int path_count, long train_games, const Weights *resolve, int threads) {
    memset(d, 0, sizeof *d);
    uint8_t (*records)[DATA_RECORD_SIZE] = NULL;
    uint8_t *split = NULL;
    size_t capacity = 0;

    /* First pass: count training games, to spread a --train-games subset evenly. */
    uint64_t total_train = 0;
    for (int p = 0; p < path_count; p++) {
        char err[256];
        uint64_t count;
        FILE *f = data_open(paths[p], &count, err, sizeof err);
        if (!f) {
            fprintf(stderr, "%s\n", err);
            return false;
        }
        int prev = -2;
        long game = -1;
        for (uint64_t i = 0; i < count; i++) {
            uint8_t r[DATA_RECORD_SIZE];
            if (fread(r, 1, sizeof r, f) != sizeof r)
                return false;
            int ply = r[30] | r[31] << 8;
            if (ply != prev + 1) {
                game++;
                total_train += game % 10 != 0 && game % 10 != 5;
            }
            prev = ply;
        }
        fclose(f);
    }

    uint64_t train_ordinal = 0;
    for (int p = 0; p < path_count; p++) {
        char err[256];
        uint64_t count;
        FILE *f = data_open(paths[p], &count, err, sizeof err);
        if (d->count + count > capacity) {
            capacity = (d->count + count) * 2;
            records = realloc(records, capacity * DATA_RECORD_SIZE);
            split = realloc(split, capacity);
            if (!records || !split) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
        }
        int prev = -2;
        long game = -1;
        int part = TRAIN;
        bool keep = true;
        for (uint64_t i = 0; i < count; i++) {
            uint8_t r[DATA_RECORD_SIZE];
            if (fread(r, 1, sizeof r, f) != sizeof r)
                return false;
            int ply = r[30] | r[31] << 8;
            if (ply != prev + 1) {
                game++;
                part = game % 10 == 0 ? TEST : game % 10 == 5 ? VALIDATION : TRAIN;
                keep = true;
                if (part == TRAIN && train_games > 0) {
                    /* keep train game t if floor((t + 1) N / T) > floor(t N / T) */
                    keep = (train_ordinal + 1) * (uint64_t)train_games / total_train
                         > train_ordinal * (uint64_t)train_games / total_train;
                    train_ordinal++;
                }
                if (keep)
                    d->games[part]++;
            }
            prev = ply;
            if (!keep)
                continue;
            memcpy(records[d->count], r, sizeof r);
            split[d->count] = (uint8_t)part;
            d->count++;
        }
        fclose(f);
    }

    d->target = xcalloc(d->count, sizeof *d->target);
    d->split = split;
    Job *jobs = xcalloc((size_t)threads, sizeof *jobs);
    pthread_t *ids = xcalloc((size_t)threads, sizeof *ids);
    for (int t = 0; t < threads; t++) {
        jobs[t] = (Job){ .d = d, .records = (const uint8_t (*)[DATA_RECORD_SIZE])records, .weights = resolve,
                         .begin = d->count * (size_t)t / (size_t)threads,
                         .end = d->count * (size_t)(t + 1) / (size_t)threads };
        pthread_create(&ids[t], NULL, resolve_worker, &jobs[t]);
    }
    size_t total = 0;
    for (int t = 0; t < threads; t++) {
        pthread_join(ids[t], NULL);
        total += jobs[t].length;
    }
    d->offset = xcalloc(d->count + 1, sizeof *d->offset);
    d->input = xcalloc(total, sizeof *d->input);
    size_t at = 0;
    for (int t = 0; t < threads; t++) {
        memcpy(d->input + at, jobs[t].input, jobs[t].length * sizeof *d->input);
        for (size_t i = jobs[t].begin; i < jobs[t].end; i++) {
            d->offset[i] = at;
            at += jobs[t].counts[i - jobs[t].begin];
        }
        free(jobs[t].input);
        free(jobs[t].counts);
    }
    d->offset[d->count] = at;
    for (size_t i = 0; i < d->count; i++)
        d->in_split[d->split[i]]++;
    free(jobs);
    free(ids);
    free(records);
    return true;
}

/* ---- Parameters as flat arrays ---- */

/* The network's parameter arrays, so optimizer code can loop over them. */
typedef struct {
    float *p[5];
    size_t n[5];
} Params;

static Params params_of(Network *net) {
    size_t H = (size_t)net->hidden;
    Params ps = { { net->skip, net->w1, net->b1, net->w2, &net->b2 }, { NNUE_INPUTS, NNUE_INPUTS * H, H, 2 * H, H ? 1 : 0 } };
    return ps;
}

static void net_zero(Network *net) {
    Params ps = params_of(net);
    for (int k = 0; k < 5; k++)
        memset(ps.p[k], 0, ps.n[k] * sizeof(float));
}

static void net_copy(Network *to, Network *from) {
    Params a = params_of(to), b = params_of(from);
    for (int k = 0; k < 5; k++)
        memcpy(a.p[k], b.p[k], a.n[k] * sizeof(float));
}

/* ---- Training ---- */

typedef struct {
    const NetData *d;
    const Network *net;
    const size_t *positions; /* the batch (or evaluation set) */
    size_t count;
    Network grads[CHUNKS];
    double loss[CHUNKS];
    bool backward;
    int threads;
} Pass;

typedef struct {
    Pass *pass;
    int id;
} PassJob;

static void *pass_worker(void *arg) {
    PassJob *job = arg;
    Pass *ps = job->pass;
    const NetData *d = ps->d;
    for (int c = job->id; c < CHUNKS; c += ps->threads) {
        if (ps->backward)
            net_zero(&ps->grads[c]);
        double loss = 0;
        size_t begin = ps->count * (size_t)c / CHUNKS, end = ps->count * (size_t)(c + 1) / CHUNKS;
        for (size_t j = begin; j < end; j++) {
            size_t i = ps->positions[j];
            const uint16_t *in = d->input + d->offset[i];
            int n = (int)(d->offset[i + 1] - d->offset[i]);
            NnueCache cache;
            double eval = nnue_forward(ps->net, in, n, &cache);
            double p = 1.0 / (1.0 + exp(-eval * SIGMOID_SCALE));
            double error = p - d->target[i];
            loss += error * error;
            if (ps->backward)
                nnue_backward(ps->net, in, n, &cache, (float)(2.0 * error * p * (1.0 - p) * SIGMOID_SCALE),
                              &ps->grads[c]);
        }
        ps->loss[c] = loss;
    }
    return NULL;
}

/* Mean loss over positions; with backward, the mean gradient (summed in chunk order) into grad. */
static double run_pass(Pass *ps, const size_t *positions, size_t count, bool backward, Network *grad) {
    ps->positions = positions;
    ps->count = count;
    ps->backward = backward;
    PassJob jobs[CHUNKS];
    pthread_t ids[CHUNKS];
    for (int t = 0; t < ps->threads; t++) {
        jobs[t] = (PassJob){ ps, t };
        pthread_create(&ids[t], NULL, pass_worker, &jobs[t]);
    }
    for (int t = 0; t < ps->threads; t++)
        pthread_join(ids[t], NULL);

    double loss = 0;
    for (int c = 0; c < CHUNKS; c++)
        loss += ps->loss[c];
    if (backward) {
        net_zero(grad);
        Params g = params_of(grad);
        for (int c = 0; c < CHUNKS; c++) {
            Params gc = params_of(&ps->grads[c]);
            for (int k = 0; k < 5; k++)
                for (size_t i = 0; i < g.n[k]; i++)
                    g.p[k][i] += gc.p[k][i];
        }
        for (int k = 0; k < 5; k++)
            for (size_t i = 0; i < g.n[k]; i++)
                g.p[k][i] /= (float)count;
    }
    return count ? loss / (double)count : 0;
}

/* Share of hidden units that never sit inside (0, 1) on a sample: dead or saturated. */
static double stuck_units(const Network *net, const NetData *d, const size_t *sample, size_t count) {
    int H = net->hidden;
    if (!H)
        return 0;
    static bool active[2][NNUE_HIDDEN_MAX];
    memset(active, 0, sizeof active);
    for (size_t j = 0; j < count; j++) {
        size_t i = sample[j];
        NnueCache cache;
        nnue_forward(net, d->input + d->offset[i], (int)(d->offset[i + 1] - d->offset[i]), &cache);
        for (int k = 0; k < 2; k++)
            for (int h = 0; h < H; h++)
                active[k][h] |= cache.acc[k][h] > 0 && cache.acc[k][h] < 1;
    }
    int stuck = 0;
    for (int k = 0; k < 2; k++)
        for (int h = 0; h < H; h++)
            stuck += !active[k][h];
    return stuck / (2.0 * H);
}

static bool parse_long(const char *s, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno || v < 0)
        return false;
    *out = v;
    return true;
}

static bool parse_double(const char *s, double *out) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno || v < 0)
        return false;
    *out = v;
    return true;
}

int main(int argc, char **argv) {
    chess_init();
    char *paths[MAX_FILES];
    int path_count = 0;
    const char *out = NULL, *resolve_path = NULL, *init_path = NULL;
    long hidden = 32, epochs = 30, batch = 16384, train_games = 0, threads = 1, seed = 1;
    double lr = 1e-3, lr_linear = 0.05, wd = 0.01;

    for (int i = 1; i < argc; i += 2) {
        const char *key = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        bool ok = value != NULL;
        if (!ok)
            ;
        else if (strcmp(key, "--out") == 0)
            out = value;
        else if (strcmp(key, "--resolve") == 0)
            resolve_path = value;
        else if (strcmp(key, "--init-linear") == 0)
            init_path = value;
        else if (strcmp(key, "--data") == 0 && path_count < MAX_FILES)
            paths[path_count++] = argv[i + 1];
        else if (strcmp(key, "--hidden") == 0)
            ok = parse_long(value, &hidden) && hidden <= NNUE_HIDDEN_MAX;
        else if (strcmp(key, "--epochs") == 0)
            ok = parse_long(value, &epochs) && epochs >= 1;
        else if (strcmp(key, "--batch") == 0)
            ok = parse_long(value, &batch) && batch >= CHUNKS;
        else if (strcmp(key, "--train-games") == 0)
            ok = parse_long(value, &train_games);
        else if (strcmp(key, "--threads") == 0)
            ok = parse_long(value, &threads) && threads >= 1;
        else if (strcmp(key, "--seed") == 0)
            ok = parse_long(value, &seed);
        else if (strcmp(key, "--lr") == 0)
            ok = parse_double(value, &lr);
        else if (strcmp(key, "--lr-linear") == 0)
            ok = parse_double(value, &lr_linear);
        else if (strcmp(key, "--wd") == 0)
            ok = parse_double(value, &wd);
        else
            ok = false;
        if (!ok) {
            fprintf(stderr, "bad option %s %s\n", key, value ? value : "");
            return 2;
        }
    }
    if (!out || !resolve_path || !path_count) {
        fprintf(stderr, "usage: nnue-train --out FILE --resolve WEIGHTS --data FILE [...] (see source)\n");
        return 2;
    }
    if (threads > CHUNKS)
        threads = CHUNKS;

    double cpu0 = clock_seconds(CLOCK_PROCESS_CPUTIME_ID), wall0 = clock_seconds(CLOCK_MONOTONIC);
    char err[256];
    Weights resolve, init;
    if (!weights_load(resolve_path, &resolve, err, sizeof err) || (init_path && !weights_load(init_path, &init, err, sizeof err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    NetData d;
    if (!load(&d, paths, path_count, train_games, &resolve, (int)threads))
        return 1;
    fprintf(stderr, "games train/val/test %zu/%zu/%zu, positions %zu/%zu/%zu\n", d.games[TRAIN],
            d.games[VALIDATION], d.games[TEST], d.in_split[TRAIN], d.in_split[VALIDATION], d.in_split[TEST]);

    size_t *sets[3];
    for (int k = 0; k < 3; k++) {
        sets[k] = xcalloc(d.in_split[k], sizeof *sets[k]);
        d.in_split[k] = 0;
    }
    for (size_t i = 0; i < d.count; i++)
        sets[d.split[i]][d.in_split[d.split[i]]++] = i;

    /* Initialization: linear part from --init-linear, network small and seeded. */
    Network net, best, grad, m, v;
    nnue_alloc(&net, (int)hidden);
    nnue_alloc(&best, (int)hidden);
    nnue_alloc(&grad, (int)hidden);
    nnue_alloc(&m, (int)hidden);
    nnue_alloc(&v, (int)hidden);
    if (init_path)
        nnue_skip_from_weights(&net, &init);
    uint64_t rng = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 1;
    for (size_t i = 0; i < NNUE_INPUTS * (size_t)hidden; i++)
        net.w1[i] = (float)(((double)rng_next(&rng) / 18446744073709551616.0) * 0.1 - 0.05);
    for (long h = 0; h < hidden; h++)
        net.b1[h] = 0.5f;
    for (long h = 0; h < 2 * hidden; h++)
        net.w2[h] = (float)((((double)rng_next(&rng) / 18446744073709551616.0) * 2 - 1) / sqrt(2.0 * (double)hidden));
    net.b2 = 0;

    Pass *pass = xcalloc(1, sizeof *pass);
    pass->d = &d;
    pass->net = &net;
    pass->threads = (int)threads;
    for (int c = 0; c < CHUNKS; c++)
        nnue_alloc(&pass->grads[c], (int)hidden);

    size_t sample = d.in_split[VALIDATION] < 20000 ? d.in_split[VALIDATION] : 20000;
    double start_val = run_pass(pass, sets[VALIDATION], d.in_split[VALIDATION], false, NULL);
    double start_test = run_pass(pass, sets[TEST], d.in_split[TEST], false, NULL);
    double best_val = start_val;
    double best_epoch = 0;
    net_copy(&best, &net);
    fprintf(stderr, "start: validation %.6f test %.6f\n", start_val, start_test);

    const double beta1 = 0.9, beta2 = 0.999, eps = 1e-8;
    long step = 0;
    size_t train_n = d.in_split[TRAIN];
    size_t batches = (train_n + (size_t)batch - 1) / (size_t)batch;
    for (long epoch = 0; epoch < epochs; epoch++) {
        double decay = epoch < epochs * 2 / 3 ? 1.0 : epoch < epochs * 5 / 6 ? 0.3 : 0.09;
        uint64_t shuffle = (uint64_t)seed * 0xD1B54A32D192ED03ULL + (uint64_t)epoch;
        for (size_t i = train_n; i > 1; i--) {
            size_t j = rng_below(&shuffle, i);
            size_t t = sets[TRAIN][i - 1];
            sets[TRAIN][i - 1] = sets[TRAIN][j];
            sets[TRAIN][j] = t;
        }
        double train_loss = 0;
        for (size_t bidx = 0; bidx < batches; bidx++) {
            size_t first = bidx * (size_t)batch, count = first + (size_t)batch > train_n ? train_n - first : (size_t)batch;
            train_loss += run_pass(pass, sets[TRAIN] + first, count, true, &grad) * (double)count;
            step++;
            double c1 = 1 - pow(beta1, (double)step), c2 = 1 - pow(beta2, (double)step);
            Params P = params_of(&net), G = params_of(&grad), M = params_of(&m), V = params_of(&v);
            for (int k = 0; k < 5; k++) {
                double rate = (k == 0 ? lr_linear : lr) * decay;
                for (size_t i = 0; i < P.n[k]; i++) {
                    double g = G.p[k][i];
                    M.p[k][i] = (float)(beta1 * M.p[k][i] + (1 - beta1) * g);
                    V.p[k][i] = (float)(beta2 * V.p[k][i] + (1 - beta2) * g * g);
                    double w = P.p[k][i] - rate * (M.p[k][i] / c1) / (sqrt(V.p[k][i] / c2) + eps);
                    if (k == 1)
                        w -= rate * wd * w; /* decoupled weight decay on the first layer */
                    if (k == 1 || k == 3)
                        w = w > WEIGHT_LIMIT ? WEIGHT_LIMIT : w < -WEIGHT_LIMIT ? -WEIGHT_LIMIT : w;
                    P.p[k][i] = (float)w;
                }
            }
            /* Validation every half epoch. */
            if (bidx + 1 == batches || bidx + 1 == batches / 2) {
                double val = run_pass(pass, sets[VALIDATION], d.in_split[VALIDATION], false, NULL);
                double at = (double)epoch + (double)(bidx + 1) / (double)batches;
                bool better = val < best_val;
                if (better) {
                    best_val = val;
                    best_epoch = at;
                    net_copy(&best, &net);
                }
                fprintf(stderr, "epoch %5.1f  validation %.6f  stuck units %.0f%%%s\n", at, val,
                        100 * stuck_units(&net, &d, sets[VALIDATION], sample), better ? "  *" : "");
            }
        }
        fprintf(stderr, "epoch %ld  train %.6f\n", epoch + 1, train_loss / (double)train_n);
    }

    pass->net = &best;
    double test = run_pass(pass, sets[TEST], d.in_split[TEST], false, NULL);
    if (!nnue_save(&best, out)) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    double cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu0, wall = clock_seconds(CLOCK_MONOTONIC) - wall0;
    printf("summary hidden=%ld train_games=%zu train_positions=%zu start_val=%.6f val=%.6f start_test=%.6f test=%.6f"
           " best_epoch=%.1f cpu_s=%.1f wall_s=%.1f\n",
           hidden, d.games[TRAIN], train_n, start_val, best_val, start_test, test, best_epoch, cpu, wall);
    return 0;
}
