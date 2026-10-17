#define _POSIX_C_SOURCE 200809L

/*
 * tuner --out FILE --data FILE [--data FILE ...] [--init FILE] [--epochs N]
 *       [--lr X] [--threads N] [--val-every N] [--report N]
 *
 * Learns evaluation weights from self-play data. --init gives the starting
 * weights (default: all zero), which are also the weights the positions are
 * resolved to quiet leaves with.
 */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "texel.h"

#define MAX_DATA_FILES 256

static double clock_seconds(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool parse_int(const char *s, int *out) {
    if (*s < '0' || *s > '9')
        return false;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end || errno || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static bool parse_double(const char *s, double *out) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno || !(v > 0))
        return false;
    *out = v;
    return true;
}

static void usage(void) {
    fprintf(stderr, "usage: tuner --out FILE --data FILE [--data FILE ...] [--init FILE]\n"
                    "             [--epochs N] [--lr X] [--threads N] [--val-every N] [--report N]\n"
                    "             [--lambda X: weight of self-play search scores in the target, 0..1]\n");
}

/*
 * The average weight of each piece type over the squares it was seen on:
 * the piece values the tuner found, which nobody told it.
 */
static void print_piece_values(const TuneData *d, const float *w) {
    static const char names[] = "PNBRQK";
    double values[6];
    printf("pieces");
    for (int type = 0; type < 6; type++) {
        double sum = 0;
        uint64_t squares = 0;
        for (int sq = 0; sq < 64; sq++) {
            if (d->seen[type * 64 + sq]) {
                sum += w[type * 64 + sq];
                squares++;
            }
        }
        values[type] = squares ? sum / (double)squares : 0;
        printf(" %c=%.0f", names[type], values[type]);
    }
    if (values[PAWN] > 0) {
        printf("  (pawn = 1:");
        for (int type = KNIGHT; type <= QUEEN; type++)
            printf(" %c=%.2f", names[type], values[type] / values[PAWN]);
        printf(")");
    }
    printf("\n");
}

int main(int argc, char **argv) {
    chess_init();

    char *data[MAX_DATA_FILES];
    int data_count = 0, val_every = 10;
    double lambda = 0;
    const char *out = NULL, *init = NULL;
    TuneConfig config = { .epochs = 400, .learning_rate = 2.0, .threads = 1, .report_every = 50 };

    for (int i = 1; i < argc; i += 2) {
        const char *key = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        bool ok = value != NULL;
        if (!ok)
            ;
        else if (strcmp(key, "--out") == 0)
            out = value;
        else if (strcmp(key, "--init") == 0)
            init = value;
        else if (strcmp(key, "--data") == 0 && data_count < MAX_DATA_FILES)
            data[data_count++] = argv[i + 1];
        else if (strcmp(key, "--epochs") == 0)
            ok = parse_int(value, &config.epochs);
        else if (strcmp(key, "--lr") == 0)
            ok = parse_double(value, &config.learning_rate);
        else if (strcmp(key, "--threads") == 0)
            ok = parse_int(value, &config.threads) && config.threads >= 1 && config.threads <= 256;
        else if (strcmp(key, "--val-every") == 0)
            ok = parse_int(value, &val_every);
        else if (strcmp(key, "--report") == 0)
            ok = parse_int(value, &config.report_every);
        else if (strcmp(key, "--lambda") == 0)
            ok = (parse_double(value, &lambda) || strcmp(value, "0") == 0) && lambda <= 1;
        else
            ok = false;
        if (!ok) {
            fprintf(stderr, "bad option %s %s\n", key, value ? value : "");
            usage();
            return 2;
        }
    }
    if (!out || data_count == 0) {
        usage();
        return 2;
    }

    double cpu_start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
    double wall_start = clock_seconds(CLOCK_MONOTONIC);

    float w[FEATURE_COUNT] = { 0 };
    char err[256];
    if (init && !weights_load_float(init, w, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    Weights resolve;
    weights_from_float(w, &resolve);

    TuneData d;
    if (!tune_load(&d, data, data_count, val_every, lambda, &resolve, config.threads, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    fprintf(stderr, "%zu positions (%zu training, %zu validation), %zu with search scores, %zu unresolved, %" PRIu64
            " quiescence nodes\n", d.count, d.train_count, d.count - d.train_count, d.scored, d.unresolved,
            d.resolve_nodes);

    TuneResult r = tune_run(&d, w, &config);
    if (!weights_save_float(out, w)) {
        fprintf(stderr, "cannot write %s\n", out);
        tune_free(&d);
        return 1;
    }

    double cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
    double wall = clock_seconds(CLOCK_MONOTONIC) - wall_start;
    print_piece_values(&d, w);
    printf("summary positions=%zu train=%zu validation=%zu start_train_loss=%.6f start_val_loss=%.6f"
           " train_loss=%.6f val_loss=%.6f best_epoch=%d epochs=%d unresolved=%zu quiet_nodes=%" PRIu64
           " cpu_s=%.3f wall_s=%.3f\n",
           d.count, d.train_count, d.count - d.train_count, r.start_train_loss, r.start_val_loss,
           r.train_loss, r.val_loss, r.best_epoch, config.epochs, d.unresolved, d.resolve_nodes, cpu, wall);
    tune_free(&d);
    return 0;
}
