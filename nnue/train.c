#define _POSIX_C_SOURCE 200809L

/*
 * nnue-train: trains the experimental network (src/nnue.h) on self-play data.
 *
 *   nnue-train --out FILE --resolve WEIGHTS --data FILE [--data FILE ...]
 *              [--init-linear WEIGHTS] [--hidden H] [--epochs N] [--batch N] [--lr X]
 *              [--lr-linear X] [--wd X] [--train-games N] [--threads N] [--seed N] [--smooth X]
 *              [--init-net FILE] [--lambda X]
 *
 * Positions are resolved to quiet leaves with the --resolve linear weights,
 * as in the linear tuner. Split by the game number inside each file: % 10 == 0
 * test (reported only), == 5 validation (picks the epoch), rest training, so
 * games that share an opening across runs land in the same part.
 * --train-games keeps only that many training games, evenly spread (for the
 * learning curve). --hidden 0 trains the linear part alone with the same
 * pipeline, as the baseline. --init-net continues from a network (same
 * --hidden) instead; with validation picking the epoch, a network that does
 * not get better is written back unchanged.
 *
 * --lambda X (milestone 16) mixes the self-play search score into the
 * target where the data has one, as the linear tuner does:
 * X * sigmoid(score) + (1 - X) * result, the score alone at the ply limit
 * (for X > 0; X = 0 is the plain result everywhere).
 *
 * --smooth X (milestone 13): game results label whole games, so they say
 * nothing about how the value changes from one move to the next - yet that
 * is all a search compares. Each position also gets one random quiet child
 * (a legal move that neither captures nor promotes), and training adds
 * X * ((r(position) + r(child)) * SIGMOID_SCALE / 4)^2, where r is the
 * network part of the evaluation (without the linear part): a quiet move
 * should hardly change what the network adds. X = 1 weighs that change like
 * a prediction error of the same size at an even position. r includes b2,
 * so the penalty also keeps the network from learning a constant bonus for
 * the side to move (tempo); the M13 networks were trained that way.
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
#include "movegen.h"
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
    uint64_t *child_offset; /* inputs of a random quiet child of leaf i (none: empty), its side to move's view */
    uint16_t *child_input;
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
    double lambda;
    size_t begin, end;
    uint16_t *input, *child_input;
    size_t length, capacity, child_length, child_capacity;
    uint8_t *counts, *child_counts;
} Job;

static void append(uint16_t **buf, size_t *length, size_t *capacity, const uint16_t *in, int n) {
    if (*length + (size_t)n > *capacity) {
        *capacity = (*capacity + (size_t)n) * 2;
        *buf = realloc(*buf, *capacity * sizeof **buf);
        if (!*buf) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    memcpy(*buf + *length, in, (size_t)n * sizeof *in);
    *length += (size_t)n;
}

/* Inputs of a random quiet child of b (deterministic per position index), 0 if it has none. */
static int quiet_child(const Board *b, size_t index, uint16_t in[NNUE_MAX_ACTIVE]) {
    MoveList list;
    movegen_legal(b, &list);
    Board children[MAX_MOVES];
    int quiet = 0;
    for (int k = 0; k < list.count; k++) {
        Board c;
        board_make_move(b, list.moves[k], &c);
        if (popcount(c.all) == popcount(b->all) && popcount(c.pieces[b->side][PAWN]) == popcount(b->pieces[b->side][PAWN]))
            children[quiet++] = c;
    }
    if (!quiet)
        return 0;
    uint64_t rng = (uint64_t)index * 0x9E3779B97F4A7C15ULL + 7;
    return nnue_inputs(&children[rng_below(&rng, (uint64_t)quiet)], in);
}

static void *resolve_worker(void *arg) {
    Job *job = arg;
    Search *s = search_new(1);
    search_set_weights(s, job->weights);
    job->counts = xcalloc(job->end - job->begin, 1);
    if (job->d->child_offset)
        job->child_counts = xcalloc(job->end - job->begin, 1);
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
        append(&job->input, &job->length, &job->capacity, in, n);
        job->counts[i - job->begin] = (uint8_t)n;
        if (job->child_counts) {
            n = quiet_child(&leaf, i, in);
            append(&job->child_input, &job->child_length, &job->child_capacity, in, n);
            job->child_counts[i - job->begin] = (uint8_t)n;
        }
        double y = pos.result / 2.0;
        if (pos.score_valid && job->lambda > 0) { /* 0: results only, exactly as before */
            double expected = 1.0 / (1.0 + exp(-pos.score * SIGMOID_SCALE));
            y = pos.end == END_PLY_LIMIT ? expected : job->lambda * expected + (1 - job->lambda) * y;
        }
        job->d->target[i] = (float)(leaf.side == pos.board.side ? y : 1.0 - y);
    }
    search_free(s);
    return NULL;
}

static bool load(NetData *d, char **paths, int path_count, long train_games, const Weights *resolve, int threads,
                 bool children, double lambda) {
    memset(d, 0, sizeof *d);
    uint8_t (*records)[DATA_RECORD_SIZE] = NULL;
    uint8_t *split = NULL;
    size_t capacity = 0;

    /* First pass: count training games, to spread a --train-games subset evenly. */
    uint64_t total_train = 0;
    for (int p = 0; p < path_count; p++) {
        char err[256];
        uint64_t count;
        int record_size;
        FILE *f = data_open(paths[p], &count, &record_size, err, sizeof err);
        if (!f) {
            fprintf(stderr, "%s\n", err);
            return false;
        }
        int prev = -2;
        long game = -1;
        for (uint64_t i = 0; i < count; i++) {
            uint8_t r[DATA_RECORD_SIZE];
            if (!data_read(f, record_size, r))
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
        int record_size;
        FILE *f = data_open(paths[p], &count, &record_size, err, sizeof err);
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
            if (!data_read(f, record_size, r))
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
    if (children)
        d->child_offset = xcalloc(d->count + 1, sizeof *d->child_offset);
    Job *jobs = xcalloc((size_t)threads, sizeof *jobs);
    pthread_t *ids = xcalloc((size_t)threads, sizeof *ids);
    for (int t = 0; t < threads; t++) {
        jobs[t] = (Job){ .d = d, .records = (const uint8_t (*)[DATA_RECORD_SIZE])records, .weights = resolve, .lambda = lambda,
                         .begin = d->count * (size_t)t / (size_t)threads,
                         .end = d->count * (size_t)(t + 1) / (size_t)threads };
        pthread_create(&ids[t], NULL, resolve_worker, &jobs[t]);
    }
    size_t total = 0, child_total = 0;
    for (int t = 0; t < threads; t++) {
        pthread_join(ids[t], NULL);
        total += jobs[t].length;
        child_total += jobs[t].child_length;
    }
    if (children) {
        d->child_input = xcalloc(child_total, sizeof *d->child_input);
        size_t at = 0;
        for (int t = 0; t < threads; t++) {
            memcpy(d->child_input + at, jobs[t].child_input, jobs[t].child_length * sizeof *d->child_input);
            for (size_t i = jobs[t].begin; i < jobs[t].end; i++) {
                d->child_offset[i] = at;
                at += jobs[t].child_counts[i - jobs[t].begin];
            }
            free(jobs[t].child_input);
            free(jobs[t].child_counts);
        }
        d->child_offset[d->count] = at;
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
    double change[CHUNKS]; /* sum of |r(position) + r(child)| over positions with a child */
    size_t changed[CHUNKS];
    bool backward;
    double smooth;
    double mean_change; /* of the last pass, in evaluation units */
    int threads;
} Pass;

/* The network part of an evaluation: everything but the linear skip part. */
static double network_part(const Network *net, const uint16_t *in, int n, NnueCache *cache) {
    double linear = 0;
    for (int k = 0; k < n; k++)
        linear += net->skip[in[k]];
    return nnue_forward(net, in, n, cache) - linear;
}

/* Gradient of the network part only: nnue_backward minus its skip contribution. */
static void network_backward(const Network *net, const uint16_t *in, int n, const NnueCache *cache, float g,
                             Network *grad) {
    nnue_backward(net, in, n, cache, g, grad);
    for (int k = 0; k < n; k++)
        grad->skip[in[k]] -= g;
}

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
        double loss = 0, change = 0;
        size_t changed = 0;
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

            /* The child is only needed for the penalty and for measuring (evaluation passes). */
            int cn = d->child_offset ? (int)(d->child_offset[i + 1] - d->child_offset[i]) : 0;
            if (cn && ps->net->hidden && (ps->smooth > 0 || !ps->backward)) {
                const uint16_t *cin = d->child_input + d->child_offset[i];
                NnueCache child_cache;
                double linear = 0;
                for (int k = 0; k < n; k++)
                    linear += ps->net->skip[in[k]];
                double rho = (eval - linear) + network_part(ps->net, cin, cn, &child_cache);
                change += fabs(rho);
                changed++;
                if (ps->backward && ps->smooth > 0) {
                    const double unit = SIGMOID_SCALE / 4;
                    float g = (float)(2.0 * ps->smooth * rho * unit * unit);
                    network_backward(ps->net, in, n, &cache, g, &ps->grads[c]);
                    network_backward(ps->net, cin, cn, &child_cache, g, &ps->grads[c]);
                }
            }
        }
        ps->loss[c] = loss;
        ps->change[c] = change;
        ps->changed[c] = changed;
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
    ps->mean_change = 0;
    size_t changed = 0;
    for (int c = 0; c < CHUNKS; c++) {
        loss += ps->loss[c];
        ps->mean_change += ps->change[c];
        changed += ps->changed[c];
    }
    ps->mean_change = changed ? ps->mean_change / (double)changed : 0;
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
    const char *out = NULL, *resolve_path = NULL, *init_path = NULL, *init_net_path = NULL;
    long hidden = 32, epochs = 30, batch = 16384, train_games = 0, threads = 1, seed = 1;
    double lr = 1e-3, lr_linear = 0.05, wd = 0.01, smooth = 0, lambda = 0;

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
        else if (strcmp(key, "--init-net") == 0)
            init_net_path = value;
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
        else if (strcmp(key, "--smooth") == 0)
            ok = parse_double(value, &smooth);
        else if (strcmp(key, "--lambda") == 0)
            ok = parse_double(value, &lambda) && lambda <= 1;
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
    if (!load(&d, paths, path_count, train_games, &resolve, (int)threads, hidden > 0, lambda))
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
    if (init_net_path) {
        Network start;
        if (!nnue_load(&start, init_net_path, err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        if (start.hidden != hidden) {
            fprintf(stderr, "%s has %d hidden units, not %ld\n", init_net_path, start.hidden, hidden);
            return 1;
        }
        net_copy(&net, &start);
        nnue_free(&start);
    }

    Pass *pass = xcalloc(1, sizeof *pass);
    pass->d = &d;
    pass->net = &net;
    pass->threads = (int)threads;
    pass->smooth = smooth;
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
                fprintf(stderr, "epoch %5.1f  validation %.6f  quiet-move change %.1f  stuck units %.0f%%%s\n", at, val,
                        pass->mean_change, 100 * stuck_units(&net, &d, sets[VALIDATION], sample), better ? "  *" : "");
            }
        }
        fprintf(stderr, "epoch %ld  train %.6f\n", epoch + 1, train_loss / (double)train_n);
    }

    pass->net = &best;
    double test = run_pass(pass, sets[TEST], d.in_split[TEST], false, NULL);
    double test_change = pass->mean_change;
    bool saved = nnue_save(&best, out);
    for (int c = 0; c < CHUNKS; c++)
        nnue_free(&pass->grads[c]);
    free(pass);
    Network *nets[] = { &net, &best, &grad, &m, &v };
    for (size_t k = 0; k < sizeof nets / sizeof nets[0]; k++)
        nnue_free(nets[k]);
    for (int k = 0; k < 3; k++)
        free(sets[k]);
    free(d.offset);
    free(d.input);
    free(d.target);
    free(d.split);
    free(d.child_offset);
    free(d.child_input);
    if (!saved) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    double cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu0, wall = clock_seconds(CLOCK_MONOTONIC) - wall0;
    printf("summary hidden=%ld train_games=%zu train_positions=%zu start_val=%.6f val=%.6f start_test=%.6f test=%.6f"
           " best_epoch=%.1f smooth=%g lambda=%g quiet_change=%.1f cpu_s=%.1f wall_s=%.1f\n",
           hidden, d.games[TRAIN], train_n, start_val, best_val, start_test, test, best_epoch, smooth, lambda, test_change, cpu,
           wall);
    return 0;
}
