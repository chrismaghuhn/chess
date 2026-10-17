#include "texel.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datafmt.h"
#include "search.h"

/*
 * Positions are split into a fixed number of chunks. Each chunk sums its
 * gradient on its own and the chunks are added in order, so the floating
 * point result is the same no matter how many threads share the work.
 */
#define CHUNKS 64

/* Positions whose quiescence search needs more nodes stay unresolved (trained as they are). */
#define QUIET_MAX_NODES 100000

static void *checked_calloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

/* ---- Loading ---- */

typedef struct {
    TuneData *d;
    const uint8_t (*records)[DATA_RECORD_SIZE];
    const Weights *weights;
    int threads, id;
    size_t invalid, unresolved, scored;
    uint64_t nodes;
} ResolveJob;

static void *resolve_worker(void *arg) {
    ResolveJob *job = arg;
    TuneData *d = job->d;
    Search *s = search_new(1);
    if (!s) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    search_set_weights(s, job->weights);

    for (size_t i = (size_t)job->id; i < d->count; i += (size_t)job->threads) {
        DataPosition pos;
        if (!data_unpack(job->records[i], &pos)) {
            job->invalid++;
            d->feature_count[i] = 0;
            continue;
        }
        Board leaf;
        uint64_t nodes;
        if (!search_quiet(s, &pos.board, QUIET_MAX_NODES, &leaf, NULL, &nodes))
            job->unresolved++;
        job->nodes += nodes;

        /* Features are from the leaf's side to move; the label is from the root's. */
        Feature f[MAX_ACTIVE_FEATURES];
        int n = features_extract(&leaf, f);
        bool flip = leaf.side != pos.board.side;
        for (int k = 0; k < n; k++) {
            bool negative = (f[k].sign < 0) != flip;
            d->features[i][k] = (PackedFeature)(f[k].index | (negative ? FEATURE_NEGATIVE : 0));
        }
        d->feature_count[i] = (uint8_t)n;
        double target = pos.result / 2.0;
        if (pos.score_valid) {
            job->scored++;
            double expected = 1.0 / (1.0 + exp(-pos.score * SIGMOID_SCALE));
            target = pos.end == END_PLY_LIMIT ? expected : d->lambda * expected + (1 - d->lambda) * target;
        }
        d->target[i] = (float)target;
    }
    search_free(s);
    return NULL;
}

bool tune_load(TuneData *d, char *const *paths, int path_count, int val_every, double lambda,
               const Weights *resolve_with, int threads, char *err, size_t err_size) {
    memset(d, 0, sizeof *d);
    d->lambda = lambda;

    /* Read all records, marking validation games on the way. */
    size_t capacity = 0;
    uint8_t (*records)[DATA_RECORD_SIZE] = NULL;
    bool *validation = NULL;
    uint64_t game = 0;

    for (int p = 0; p < path_count; p++) {
        uint64_t count;
        int record_size;
        FILE *f = data_open(paths[p], &count, &record_size, err, err_size);
        if (!f) {
            free(records);
            free(validation);
            return false;
        }
        if (d->count + count > capacity) {
            capacity = (d->count + count) * 2;
            records = realloc(records, capacity * DATA_RECORD_SIZE);
            validation = realloc(validation, capacity * sizeof *validation);
            if (!records || !validation) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
        }
        int prev_ply = -2;
        for (uint64_t i = 0; i < count; i++) {
            uint8_t *r = records[d->count];
            if (!data_read(f, record_size, r)) {
                snprintf(err, err_size, "%s: read error", paths[p]);
                fclose(f);
                free(records);
                free(validation);
                return false;
            }
            int ply = r[30] | r[31] << 8;
            if (ply != prev_ply + 1)
                game++; /* a new game starts wherever the ply does not go up by one */
            prev_ply = ply;
            validation[d->count] = val_every > 0 && game % (uint64_t)val_every == 0;
            d->count++;
        }
        fclose(f);
    }

    d->features = checked_calloc(d->count, sizeof *d->features);
    d->feature_count = checked_calloc(d->count, sizeof *d->feature_count);
    d->target = checked_calloc(d->count, sizeof *d->target);
    d->validation = validation ? validation : checked_calloc(1, sizeof *d->validation);

    ResolveJob *jobs = checked_calloc((size_t)threads, sizeof *jobs);
    pthread_t *ids = checked_calloc((size_t)threads, sizeof *ids);
    for (int t = 0; t < threads; t++) {
        jobs[t] = (ResolveJob){ d, (const uint8_t (*)[DATA_RECORD_SIZE])records, resolve_with, threads, t, 0, 0, 0, 0 };
        if (pthread_create(&ids[t], NULL, resolve_worker, &jobs[t]) != 0) {
            fprintf(stderr, "cannot start thread %d\n", t);
            exit(1);
        }
    }
    size_t invalid = 0;
    for (int t = 0; t < threads; t++) {
        pthread_join(ids[t], NULL);
        invalid += jobs[t].invalid;
        d->unresolved += jobs[t].unresolved;
        d->scored += jobs[t].scored;
        d->resolve_nodes += jobs[t].nodes;
    }
    free(jobs);
    free(ids);
    free(records);

    if (invalid) {
        snprintf(err, err_size, "%zu invalid records", invalid);
        tune_free(d);
        return false;
    }

    for (size_t i = 0; i < d->count; i++) {
        if (d->validation[i])
            continue;
        d->train_count++;
        for (int k = 0; k < d->feature_count[i]; k++)
            d->seen[d->features[i][k] & ~FEATURE_NEGATIVE]++;
    }
    return true;
}

void tune_free(TuneData *d) {
    free(d->features);
    free(d->feature_count);
    free(d->target);
    free(d->validation);
    memset(d, 0, sizeof *d);
}

/* ---- Training ---- */

typedef struct {
    const TuneData *d;
    const float *w;
    double grad[CHUNKS][FEATURE_COUNT];
    double train_loss[CHUNKS], val_loss[CHUNKS];
    int threads;
} Epoch;

typedef struct {
    Epoch *e;
    int id;
} EpochJob;

static void *epoch_worker(void *arg) {
    EpochJob *job = arg;
    Epoch *e = job->e;
    const TuneData *d = e->d;

    for (int c = job->id; c < CHUNKS; c += e->threads) {
        double *grad = e->grad[c];
        double train_loss = 0, val_loss = 0;
        memset(grad, 0, sizeof e->grad[c]);

        size_t begin = d->count * (size_t)c / CHUNKS, end = d->count * (size_t)(c + 1) / CHUNKS;
        for (size_t i = begin; i < end; i++) {
            const PackedFeature *f = d->features[i];
            int n = d->feature_count[i];
            double eval = 0;
            for (int k = 0; k < n; k++) {
                float weight = e->w[f[k] & ~FEATURE_NEGATIVE];
                eval += (f[k] & FEATURE_NEGATIVE) ? -weight : weight;
            }
            double p = 1.0 / (1.0 + exp(-eval * SIGMOID_SCALE));
            double error = p - d->target[i];
            if (d->validation[i]) {
                val_loss += error * error;
                continue;
            }
            train_loss += error * error;
            double slope = 2.0 * error * p * (1.0 - p) * SIGMOID_SCALE;
            for (int k = 0; k < n; k++)
                grad[f[k] & ~FEATURE_NEGATIVE] += (f[k] & FEATURE_NEGATIVE) ? -slope : slope;
        }
        e->train_loss[c] = train_loss;
        e->val_loss[c] = val_loss;
    }
    return NULL;
}

/* One pass over all positions: losses, and the training gradient into grad. */
static void evaluate(Epoch *e, const float *w, double grad[FEATURE_COUNT], double *train_loss,
                     double *val_loss) {
    e->w = w;
    EpochJob jobs[CHUNKS];
    pthread_t ids[CHUNKS];
    for (int t = 0; t < e->threads; t++) {
        jobs[t] = (EpochJob){ e, t };
        if (pthread_create(&ids[t], NULL, epoch_worker, &jobs[t]) != 0) {
            fprintf(stderr, "cannot start thread %d\n", t);
            exit(1);
        }
    }
    for (int t = 0; t < e->threads; t++)
        pthread_join(ids[t], NULL);

    const TuneData *d = e->d;
    size_t val_count = d->count - d->train_count;
    *train_loss = *val_loss = 0;
    memset(grad, 0, FEATURE_COUNT * sizeof grad[0]);
    for (int c = 0; c < CHUNKS; c++) {
        *train_loss += e->train_loss[c];
        *val_loss += e->val_loss[c];
        for (int k = 0; k < FEATURE_COUNT; k++)
            grad[k] += e->grad[c][k];
    }
    *train_loss /= d->train_count ? (double)d->train_count : 1.0;
    *val_loss /= val_count ? (double)val_count : 1.0;
    for (int k = 0; k < FEATURE_COUNT; k++)
        grad[k] /= d->train_count ? (double)d->train_count : 1.0;
}

TuneResult tune_run(const TuneData *d, float w[FEATURE_COUNT], const TuneConfig *config) {
    const double beta1 = 0.9, beta2 = 0.999, epsilon = 1e-8;
    bool has_validation = d->count > d->train_count;

    Epoch *e = checked_calloc(1, sizeof *e);
    e->d = d;
    e->threads = config->threads < 1 ? 1 : config->threads > CHUNKS ? CHUNKS : config->threads;

    double grad[FEATURE_COUNT], m[FEATURE_COUNT] = { 0 }, v[FEATURE_COUNT] = { 0 };
    float best[FEATURE_COUNT];
    memcpy(best, w, sizeof best);

    TuneResult r;
    evaluate(e, w, grad, &r.start_train_loss, &r.start_val_loss);
    r.train_loss = r.start_train_loss;
    r.val_loss = r.start_val_loss;
    r.best_epoch = 0;

    for (int epoch = 1; epoch <= config->epochs; epoch++) {
        /* grad holds the gradient at the current weights from the last evaluation. */
        double c1 = 1.0 - pow(beta1, epoch), c2 = 1.0 - pow(beta2, epoch);
        for (int k = 0; k < FEATURE_COUNT; k++) {
            m[k] = beta1 * m[k] + (1 - beta1) * grad[k];
            v[k] = beta2 * v[k] + (1 - beta2) * grad[k] * grad[k];
            double step = config->learning_rate * (m[k] / c1) / (sqrt(v[k] / c2) + epsilon);
            double next = w[k] - step;
            w[k] = (float)(next > WEIGHT_MAX ? WEIGHT_MAX : next < -WEIGHT_MAX ? -WEIGHT_MAX : next);
        }

        double train_loss, val_loss;
        evaluate(e, w, grad, &train_loss, &val_loss);
        bool better = has_validation ? val_loss < r.val_loss : true;
        if (better) {
            r.train_loss = train_loss;
            r.val_loss = val_loss;
            r.best_epoch = epoch;
            memcpy(best, w, sizeof best);
        }
        if (config->report_every && (epoch % config->report_every == 0 || epoch == config->epochs))
            fprintf(stderr, "epoch %4d  train %.6f  validation %.6f%s\n", epoch, train_loss, val_loss,
                    better ? "  *" : "");
    }

    memcpy(w, best, sizeof best);
    free(e);
    return r;
}
