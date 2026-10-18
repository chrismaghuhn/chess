#define _POSIX_C_SOURCE 200809L

#include "selfplay.h"

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
#include "eval.h"
#include "game.h"
#include "movegen.h"
#include "nnue.h"
#include "rng.h"
#include "search.h"

#define MAX_RANDOM_PLIES 40

/*
 * Resignation. A side resigns once its own search and the opponent's agree
 * for RESIGN_PLIES plies in a row that it is at least T behind. T is
 * calibrated in every run: the first CALIBRATION_SHARE of the games never
 * resign and record, for each candidate T, whether the side that would have
 * resigned went on to lose anyway. The smallest T whose false-resignation
 * rate stays below MAX_FALSE_RESIGN (one-sided 95% Wilson bound, at least
 * MIN_TRIGGERS cases) is used for the rest; if none qualifies, nobody
 * resigns. Games cut at the ply limit say nothing and are left out. The
 * calibration games depend only on their seeds, so T does too.
 */
#define RESIGN_PLIES 4
#define CALIBRATION_SHARE 0.2
#define MAX_FALSE_RESIGN 0.025
#define MIN_TRIGGERS 20
static const int resign_candidates[] = { 200, 300, 400, 600, 800, 1200, 2000 };
#define CANDIDATES (int)(sizeof resign_candidates / sizeof resign_candidates[0])

typedef struct {
    int games;
    int threads;
    uint64_t nodes;   /* per move */
    int random_plies; /* uniformly random moves at the start of each game */
    int max_plies;    /* longer games are scored as draws */
    uint64_t seed;
    int hash_mb;      /* per thread */
    const char *weights_path;
    const char *net_path; /* evaluate with this network (weights still order moves) */
    unsigned options; /* search options */
    bool resign;      /* calibrated resignation (see above) */
    const char *out_path;
} Config;

/* One finished game: its positions, results still to be filled in. */
typedef struct {
    uint8_t (*records)[DATA_RECORD_SIZE];
    int count;
    GameResult result;
    GameEnd end;
    int plies;
    uint64_t nodes;
    int trigger[CANDIDATES]; /* side that would have resigned at each candidate, or -1 */
} Played;

/*
 * Game i always uses the same random seed, and games are written in order
 * of their number, so the output does not depend on the thread count.
 */
typedef struct {
    Config cfg;
    Weights weights;
    Network net;
    bool has_net;
    FILE *out;
    bool write_failed;

    pthread_mutex_t lock;
    int next_game;
    int next_write;
    Played **done; /* finished games waiting for their turn to be written */

    uint64_t positions, plies, nodes;
    int results[4];
    int ends[END_COUNT];

    /* Resignation calibration. */
    pthread_cond_t calibrated;
    int calibration_games, calibration_done;
    int triggers[CANDIDATES], false_triggers[CANDIDATES];
    int threshold; /* 0 = no resignation */
    bool zero_weights;
} Selfplay;

static double clock_seconds(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *checked_alloc(size_t size) {
    void *p = calloc(1, size);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

/*
 * Plays game id. With threshold > 0 a side resigns as described above;
 * either way, the plies at which each candidate threshold would have fired
 * are recorded for calibration.
 */
static Played *play_game(const Config *c, Search *s, int id, int threshold, bool scores_valid) {
    uint64_t rng = c->seed ^ (0xD1B54A32D192ED03ULL * (uint64_t)(id + 1));
    Played *p = checked_alloc(sizeof *p);
    p->records = checked_alloc((size_t)c->max_plies * DATA_RECORD_SIZE);
    for (int k = 0; k < CANDIDATES; k++)
        p->trigger[k] = -1;

    Board start;
    board_from_fen(&start, START_FEN);
    Game g;
    if (!game_random_opening(&g, &start, c->random_plies, &rng)) {
        fprintf(stderr, "game %d: no random opening without a game end\n", id);
        exit(1);
    }

    search_clear(s);
    int ply = c->random_plies;
    int recent[RESIGN_PLIES], seen = 0; /* last search scores, white's view */
    GameEnd end;
    GameResult result;
    while ((result = game_result_detail(&g, &end)) == GAME_ONGOING) {
        if (ply >= c->max_plies) {
            result = GAME_DRAW;
            end = END_PLY_LIMIT;
            break;
        }

        SearchLimits limits;
        search_limits_init(&limits);
        limits.nodes = c->nodes;
        limits.seed = rng_next(&rng) | 1;
        SearchResult r = search_run(s, &g, &limits, false);
        p->nodes += r.nodes;
        data_pack(&g.board, RESULT_DRAW, ply, r.score, scores_valid, p->records[p->count++]);

        recent[seen++ % RESIGN_PLIES] = g.board.side == WHITE ? r.score : -r.score;
        int loser = -1;
        if (seen >= RESIGN_PLIES) {
            int low = recent[0], high = recent[0];
            for (int k = 1; k < RESIGN_PLIES; k++) {
                low = recent[k] < low ? recent[k] : low;
                high = recent[k] > high ? recent[k] : high;
            }
            for (int k = 0; k < CANDIDATES; k++) {
                int side = high <= -resign_candidates[k] ? WHITE : low >= resign_candidates[k] ? BLACK : -1;
                if (side >= 0 && p->trigger[k] < 0)
                    p->trigger[k] = side;
                if (side >= 0 && resign_candidates[k] == threshold)
                    loser = side;
            }
        }
        if (loser >= 0) {
            result = loser == WHITE ? GAME_BLACK_WINS : GAME_WHITE_WINS;
            end = END_RESIGN;
            break;
        }

        game_play(&g, r.best);
        ply++;
    }

    int white_result = result == GAME_WHITE_WINS ? RESULT_WIN
                     : result == GAME_BLACK_WINS ? RESULT_LOSS : RESULT_DRAW;
    for (int i = 0; i < p->count; i++)
        data_set_outcome(p->records[i], white_result, end);
    p->result = result;
    p->end = end;
    p->plies = ply;
    return p;
}

/* Upper end of a one-sided 95% Wilson interval for a rate of hits in n. */
static double wilson_upper(int hits, int n) {
    if (n == 0)
        return 1;
    const double z = 1.6449;
    double phat = (double)hits / n, z2 = z * z;
    double center = phat + z2 / (2.0 * n), margin = z * sqrt(phat * (1 - phat) / n + z2 / (4.0 * n * n));
    return (center + margin) / (1 + z2 / n);
}

/* Counts a finished calibration game; the last one picks the threshold. Lock held. */
static void calibrate(Selfplay *sp, const Played *p) {
    if (p->end != END_PLY_LIMIT) {
        for (int k = 0; k < CANDIDATES; k++) {
            if (p->trigger[k] < 0)
                continue;
            sp->triggers[k]++;
            bool lost = p->result == (p->trigger[k] == WHITE ? GAME_BLACK_WINS : GAME_WHITE_WINS);
            sp->false_triggers[k] += !lost;
        }
    }
    if (++sp->calibration_done < sp->calibration_games)
        return;
    sp->threshold = 0;
    for (int k = 0; k < CANDIDATES && sp->cfg.resign; k++) {
        if (sp->triggers[k] >= MIN_TRIGGERS && wilson_upper(sp->false_triggers[k], sp->triggers[k]) <= MAX_FALSE_RESIGN) {
            sp->threshold = resign_candidates[k];
            break;
        }
    }
    pthread_cond_broadcast(&sp->calibrated);
}

/* Hands in a finished game and writes every game whose turn has come. */
static void store(Selfplay *sp, int id, Played *p) {
    pthread_mutex_lock(&sp->lock);
    sp->done[id] = p;
    while (!sp->write_failed && sp->next_write < sp->cfg.games && sp->done[sp->next_write]) {
        Played *q = sp->done[sp->next_write];
        if (fwrite(q->records, DATA_RECORD_SIZE, (size_t)q->count, sp->out) != (size_t)q->count) {
            sp->write_failed = true; /* workers stop taking new games */
            break;
        }
        sp->positions += (uint64_t)q->count;
        sp->plies += (uint64_t)q->plies;
        sp->nodes += q->nodes;
        sp->results[q->result]++;
        sp->ends[q->end]++;
        free(q->records);
        free(q);
        sp->done[sp->next_write++] = NULL;

        int step = sp->cfg.games >= 10 ? sp->cfg.games / 10 : 1;
        if (sp->next_write % step == 0 || sp->next_write == sp->cfg.games)
            fprintf(stderr, "%d/%d games\n", sp->next_write, sp->cfg.games);
    }
    pthread_mutex_unlock(&sp->lock);
}

static void *worker(void *arg) {
    Selfplay *sp = arg;
    Search *s = search_new((size_t)sp->cfg.hash_mb);
    if (!s) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    search_set_weights(s, &sp->weights);
    search_set_options(s, sp->cfg.options);
    if (sp->has_net && !search_set_network(s, &sp->net)) {
        fprintf(stderr, "network %s cannot be used (weights too large or out of memory)\n", sp->cfg.net_path);
        exit(1);
    }

    for (;;) {
        pthread_mutex_lock(&sp->lock);
        int id = sp->write_failed ? sp->cfg.games : sp->next_game++;
        /* Games after the calibration games wait for the threshold. */
        bool calibrating = id < sp->calibration_games;
        while (!calibrating && id < sp->cfg.games && sp->calibration_done < sp->calibration_games)
            pthread_cond_wait(&sp->calibrated, &sp->lock);
        int threshold = calibrating ? 0 : sp->threshold;
        pthread_mutex_unlock(&sp->lock);
        if (id >= sp->cfg.games)
            break;

        Played *p = play_game(&sp->cfg, s, id, threshold, !sp->zero_weights || sp->has_net);
        if (calibrating) {
            pthread_mutex_lock(&sp->lock);
            calibrate(sp, p);
            pthread_mutex_unlock(&sp->lock);
        }
        store(sp, id, p);
    }
    search_free(s);
    return NULL;
}

static void usage(void) {
    fprintf(stderr,
            "usage: engine selfplay --out FILE [--games N] [--nodes N] [--threads N]\n"
            "                       [--weights FILE] [--search OPTS] [--seed N] [--random-plies 0..%d]\n"
            "                       [--max-plies N] [--hash MB] [--resign on|off] [--net FILE]\n",
            MAX_RANDOM_PLIES);
}

/* Digits only: no sign, no trailing junk, no overflow. */
static bool parse_u64(const char *s, uint64_t *out) {
    if (*s < '0' || *s > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (*end || errno)
        return false;
    *out = v;
    return true;
}

static bool parse_int(const char *s, int *out) {
    uint64_t v;
    if (!parse_u64(s, &v) || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static bool parse_args(int argc, char **argv, Config *c) {
    for (int i = 1; i < argc; i += 2) {
        const char *key = argv[i];
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", key);
            return false;
        }
        const char *value = argv[i + 1];
        bool ok = true;
        if (strcmp(key, "--out") == 0)
            c->out_path = value;
        else if (strcmp(key, "--weights") == 0)
            c->weights_path = value;
        else if (strcmp(key, "--net") == 0)
            c->net_path = value;
        else if (strcmp(key, "--resign") == 0)
            ok = (strcmp(value, "on") == 0 && (c->resign = true)) || (strcmp(value, "off") == 0 && !(c->resign = false));
        else if (strcmp(key, "--search") == 0)
            ok = search_parse_options(value, &c->options);
        else if (strcmp(key, "--games") == 0)
            ok = parse_int(value, &c->games);
        else if (strcmp(key, "--nodes") == 0)
            ok = parse_u64(value, &c->nodes);
        else if (strcmp(key, "--threads") == 0)
            ok = parse_int(value, &c->threads);
        else if (strcmp(key, "--seed") == 0)
            ok = parse_u64(value, &c->seed);
        else if (strcmp(key, "--random-plies") == 0)
            ok = parse_int(value, &c->random_plies);
        else if (strcmp(key, "--max-plies") == 0)
            ok = parse_int(value, &c->max_plies);
        else if (strcmp(key, "--hash") == 0)
            ok = parse_int(value, &c->hash_mb);
        else {
            fprintf(stderr, "unknown option %s\n", key);
            return false;
        }
        if (!ok) {
            fprintf(stderr, "invalid value for %s: %s\n", key, value);
            return false;
        }
    }

    if (!c->out_path)
        fprintf(stderr, "--out is required\n");
    else if (c->games < 1 || c->nodes < 1 || c->threads < 1 || c->threads > 256 || c->hash_mb < 1
             || c->random_plies > MAX_RANDOM_PLIES || c->max_plies <= c->random_plies
             || c->max_plies > 10000)
        fprintf(stderr, "option value out of range\n");
    else
        return true;
    return false;
}

int selfplay_main(int argc, char **argv) {
    Config c = {
        .games = 100, .threads = 1, .nodes = 5000, .random_plies = 8, .max_plies = 400,
        .seed = 1, .hash_mb = 4, .options = SEARCH_DEFAULT,
    };
    if (!parse_args(argc, argv, &c)) {
        usage();
        return 2;
    }

    Selfplay *sp = checked_alloc(sizeof *sp);
    sp->cfg = c;
    if (c.weights_path) {
        char err[256];
        if (!weights_load(c.weights_path, &sp->weights, err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            free(sp);
            return 1;
        }
    }
    if (c.net_path) {
        char err[256];
        if (!(sp->has_net = nnue_load(&sp->net, c.net_path, err, sizeof err))) {
            fprintf(stderr, "%s\n", err);
            free(sp);
            return 1;
        }
    }
    sp->out = data_create(c.out_path);
    if (!sp->out) {
        fprintf(stderr, "cannot create %s\n", c.out_path);
        if (sp->has_net)
            nnue_free(&sp->net);
        free(sp);
        return 1;
    }
    sp->done = checked_alloc((size_t)c.games * sizeof *sp->done);
    pthread_mutex_init(&sp->lock, NULL);
    pthread_cond_init(&sp->calibrated, NULL);
    sp->calibration_games = c.resign ? (int)ceil(c.games * CALIBRATION_SHARE) : c.games;
    sp->zero_weights = true;
    for (int i = 0; i < FEATURE_COUNT; i++)
        sp->zero_weights = sp->zero_weights && sp->weights.w[i] == 0;

    double cpu_start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
    double wall_start = clock_seconds(CLOCK_MONOTONIC);

    pthread_t *threads = checked_alloc((size_t)c.threads * sizeof *threads);
    for (int i = 0; i < c.threads; i++) {
        if (pthread_create(&threads[i], NULL, worker, sp) != 0) {
            fprintf(stderr, "cannot start thread %d\n", i);
            exit(1);
        }
    }
    for (int i = 0; i < c.threads; i++)
        pthread_join(threads[i], NULL);

    double cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
    double wall = clock_seconds(CLOCK_MONOTONIC) - wall_start;
    bool closed = fclose(sp->out) == 0;
    bool ok = !sp->write_failed && closed;

    /* After a write error, finished games that never got their turn are still here. */
    for (int i = 0; i < c.games; i++) {
        if (sp->done[i]) {
            free(sp->done[i]->records);
            free(sp->done[i]);
        }
    }

    if (ok) {
        printf("games %d, positions %" PRIu64 ", white wins %d, draws %d, black wins %d\n", c.games,
               sp->positions, sp->results[GAME_WHITE_WINS], sp->results[GAME_DRAW],
               sp->results[GAME_BLACK_WINS]);
        printf("ends:");
        for (int e = END_MATE; e < END_COUNT; e++)
            printf(" %s %d", game_end_names[e], sp->ends[e]);
        printf("\naverage %.1f plies, %" PRIu64 " nodes, cpu %.2f s, wall %.2f s\n",
               (double)sp->plies / c.games, sp->nodes, cpu, wall);
        printf("resignation (from %d games that never resign): threshold", sp->calibration_games);
        for (int k = 0; k < CANDIDATES; k++)
            printf(" %d: %d/%d false", resign_candidates[k], sp->false_triggers[k], sp->triggers[k]);
        printf(" -> %s %d\n", c.resign ? "using" : "not used", sp->threshold);
        int k_used = -1;
        for (int k = 0; k < CANDIDATES; k++)
            if (resign_candidates[k] == sp->threshold)
                k_used = k;
        printf("summary games=%d positions=%" PRIu64 " white_wins=%d draws=%d black_wins=%d"
               " plies=%" PRIu64 " nodes=%" PRIu64 " resigned=%d resign_threshold=%d resign_false=%d resign_checks=%d"
               " cpu_s=%.3f wall_s=%.3f\n",
               c.games, sp->positions, sp->results[GAME_WHITE_WINS], sp->results[GAME_DRAW],
               sp->results[GAME_BLACK_WINS], sp->plies, sp->nodes, sp->ends[END_RESIGN], sp->threshold,
               k_used >= 0 ? sp->false_triggers[k_used] : 0, k_used >= 0 ? sp->triggers[k_used] : 0, cpu, wall);
    } else {
        fprintf(stderr, "error writing %s after %d games; the file is incomplete\n", c.out_path,
                sp->next_write);
    }

    pthread_mutex_destroy(&sp->lock);
    pthread_cond_destroy(&sp->calibrated);
    free(threads);
    free(sp->done);
    if (sp->has_net)
        nnue_free(&sp->net);
    free(sp);
    return ok ? 0 : 1;
}

/*
 * Statistics, plus a check of the labels: within a game (ply going up by
 * one) the result flips with the side to move and the end reason stays the
 * same; only mates are decisive; and the last position of a mated game
 * belongs to the winner, who is about to deliver mate.
 */
int data_info_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: engine data-info FILE [samples]\n");
        return 2;
    }
    int samples = argc >= 3 ? atoi(argv[2]) : 5;

    char err[256];
    uint64_t count;
    int record_size;
    FILE *f = data_open(argv[1], &count, &record_size, err, sizeof err);
    if (!f) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    uint64_t results[3] = { 0 }, ends[END_COUNT] = { 0 }, scored = 0;
    uint64_t invalid = 0, in_check = 0, ply_sum = 0, games = 0, label_errors = 0;
    DataPosition prev = { .ply = -2 };
    bool have_prev = false;
    uint8_t record[DATA_RECORD_SIZE];

    for (uint64_t i = 0; i <= count; i++) {
        DataPosition pos;
        bool last = i == count;
        if (!last && (!data_read(f, record_size, record) || !data_unpack(record, &pos))) {
            invalid++;
            have_prev = false;
            continue;
        }

        bool same_game = !last && have_prev && pos.ply == prev.ply + 1;
        if (have_prev && !same_game && prev.end == END_MATE && prev.result != RESULT_WIN)
            label_errors++; /* game over: its last position must be the mating side's */
        if (last)
            break;

        if (!same_game) {
            games++;
            ends[pos.end]++;
        } else if (pos.result != RESULT_WIN - prev.result || pos.end != prev.end) {
            label_errors++;
        }
        if ((pos.end == END_MATE || pos.end == END_RESIGN) == (pos.result == RESULT_DRAW))
            label_errors++;

        results[pos.result]++;
        in_check += board_in_check(&pos.board);
        scored += pos.score_valid;
        ply_sum += (uint64_t)pos.ply;
        if ((int64_t)i < samples) {
            char fen[FEN_MAX];
            board_to_fen(&pos.board, fen, sizeof fen);
            static const char *names[] = { "loss", "draw", "win" };
            printf("%-4s ply %3d  %-11s %s\n", names[pos.result], pos.ply, game_end_names[pos.end], fen);
        }
        prev = pos;
        have_prev = true;
    }
    fclose(f);

    printf("ends:");
    for (int e = END_MATE; e < END_COUNT; e++)
        printf(" %s %" PRIu64, game_end_names[e], ends[e]);
    printf("\npositions=%" PRIu64 " games=%" PRIu64 " wins=%" PRIu64 " draws=%" PRIu64
           " losses=%" PRIu64 " in_check=%" PRIu64 " scored=%" PRIu64 " invalid=%" PRIu64 " label_errors=%" PRIu64
           " average_ply=%.1f\n",
           count, games, results[RESULT_WIN], results[RESULT_DRAW], results[RESULT_LOSS], in_check, scored,
           invalid, label_errors, count > invalid ? (double)ply_sum / (double)(count - invalid) : 0.0);
    return invalid || label_errors ? 1 : 0;
}
