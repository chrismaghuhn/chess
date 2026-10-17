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
    size_t invalid, unresolved;
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
        if (!d->needs_resolving[i])
            continue;
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
    }
    search_free(s);
    return NULL;
}

/* ---- Leaf cache ---- */

static const char leaves_magic[4] = { 'S', 'C', 'H', 'L' };
#define LEAVES_VERSION 1

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}

static bool read_le(FILE *f, uint64_t *v, int bytes) {
    uint8_t b[8];
    if (fread(b, 1, (size_t)bytes, f) != (size_t)bytes)
        return false;
    *v = 0;
    for (int i = 0; i < bytes; i++)
        *v |= (uint64_t)b[i] << (8 * i);
    return true;
}

static bool write_le(FILE *f, uint64_t v, int bytes) {
    uint8_t b[8];
    for (int i = 0; i < bytes; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    return fwrite(b, 1, (size_t)bytes, f) == (size_t)bytes;
}

/* Loads the cached leaves of positions [first, first + count); false if missing or stale. */
static bool load_leaves(TuneData *d, const char *path, size_t first, size_t count, uint64_t hash) {
    char name[4096];
    snprintf(name, sizeof name, "%s.leaves", path);
    FILE *f = fopen(name, "rb");
    if (!f)
        return false;
    char magic[4];
    uint64_t version, features, n, stored_hash;
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, leaves_magic, 4) == 0 && read_le(f, &version, 4)
           && version == LEAVES_VERSION && read_le(f, &features, 4) && features == FEATURE_COUNT
           && read_le(f, &n, 8) && n == count && read_le(f, &stored_hash, 8) && stored_hash == hash;
    for (size_t i = first; ok && i < first + count; i++) {
        uint64_t k = 0, packed = 0;
        ok = read_le(f, &k, 1) && k <= MAX_ACTIVE_FEATURES;
        for (uint64_t j = 0; ok && j < k; j++) {
            ok = read_le(f, &packed, 2) && (packed & ~FEATURE_NEGATIVE) < FEATURE_COUNT;
            d->features[i][j] = (PackedFeature)packed;
        }
        d->feature_count[i] = (uint8_t)k;
    }
    ok = ok && fgetc(f) == EOF;
    fclose(f);
    return ok;
}

/* Writes the leaves of positions [first, first + count) next to the data file (atomically). */
static void save_leaves(const TuneData *d, const char *path, size_t first, size_t count, uint64_t hash) {
    char name[4096], part[4200];
    snprintf(name, sizeof name, "%s.leaves", path);
    snprintf(part, sizeof part, "%s.part", name);
    FILE *f = fopen(part, "wb");
    if (!f)
        return; /* only a cache */
    bool ok = fwrite(leaves_magic, 1, 4, f) == 4 && write_le(f, LEAVES_VERSION, 4) && write_le(f, FEATURE_COUNT, 4)
           && write_le(f, count, 8) && write_le(f, hash, 8);
    for (size_t i = first; ok && i < first + count; i++) {
        ok = write_le(f, d->feature_count[i], 1);
        for (int j = 0; ok && j < d->feature_count[i]; j++)
            ok = write_le(f, d->features[i][j], 2);
    }
    if (fclose(f) == 0 && ok)
        rename(part, name);
    else
        remove(part);
}

bool tune_load(TuneData *d, char *const *paths, int path_count, int val_every, double lambda,
               bool leaf_cache, const Weights *resolve_with, int threads, char *err, size_t err_size) {
    memset(d, 0, sizeof *d);
    d->lambda = lambda;
    size_t *file_first = checked_calloc((size_t)path_count, sizeof *file_first);
    size_t *file_count = checked_calloc((size_t)path_count, sizeof *file_count);
    uint64_t *file_hash = checked_calloc((size_t)path_count, sizeof *file_hash);

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
        file_first[p] = d->count;
        file_count[p] = count;
        file_hash[p] = 0xCBF29CE484222325ULL;
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
            file_hash[p] = fnv1a(file_hash[p], r, DATA_RECORD_SIZE);
            d->count++;
        }
        fclose(f);
    }

    d->features = checked_calloc(d->count, sizeof *d->features);
    d->feature_count = checked_calloc(d->count, sizeof *d->feature_count);
    d->target = checked_calloc(d->count, sizeof *d->target);
    d->validation = validation ? validation : checked_calloc(1, sizeof *d->validation);

    /* Targets come straight from the records (see tune_load in texel.h). */
    for (size_t i = 0; i < d->count; i++) {
        const uint8_t *r = records[i];
        double target = r[28] / 2.0;
        if (r[34] == 1) {
            d->scored++;
            int score = (int16_t)(r[32] | r[33] << 8);
            double expected = 1.0 / (1.0 + exp(-score * SIGMOID_SCALE));
            target = r[29] == END_PLY_LIMIT ? expected : d->lambda * expected + (1 - d->lambda) * target;
        }
        d->target[i] = (float)target;
    }

    /* Leaves from caches where they are valid; everything else gets resolved. */
    d->needs_resolving = checked_calloc(d->count, sizeof *d->needs_resolving);
    bool *cached_file = checked_calloc((size_t)path_count, sizeof *cached_file);
    for (int p = 0; p < path_count; p++) {
        cached_file[p] = leaf_cache && load_leaves(d, paths[p], file_first[p], file_count[p], file_hash[p]);
        if (cached_file[p])
            d->cached += file_count[p];
        else
            for (size_t i = file_first[p]; i < file_first[p] + file_count[p]; i++)
                d->needs_resolving[i] = true;
    }

    ResolveJob *jobs = checked_calloc((size_t)threads, sizeof *jobs);
    pthread_t *ids = checked_calloc((size_t)threads, sizeof *ids);
    for (int t = 0; t < threads; t++) {
        jobs[t] = (ResolveJob){ d, (const uint8_t (*)[DATA_RECORD_SIZE])records, resolve_with, threads, t, 0, 0, 0 };
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
        d->resolve_nodes += jobs[t].nodes;
    }
    free(jobs);
    free(ids);
    free(records);
    if (leaf_cache && !invalid)
        for (int p = 0; p < path_count; p++)
            if (!cached_file[p])
                save_leaves(d, paths[p], file_first[p], file_count[p], file_hash[p]);
    free(cached_file);
    free(file_first);
    free(file_count);
    free(file_hash);
    free(d->needs_resolving);
    d->needs_resolving = NULL;

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
    r.epochs_run = 0;

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
        r.epochs_run = epoch;
        if (config->patience && has_validation && epoch - r.best_epoch >= config->patience)
            break; /* no better validation loss for a while: further epochs would be wasted */
    }

    memcpy(w, best, sizeof best);
    free(e);
    return r;
}
