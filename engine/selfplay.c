#define _POSIX_C_SOURCE 200809L

#include "selfplay.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "datafmt.h"
#include "eval.h"
#include "game.h"
#include "movegen.h"
#include "rng.h"
#include "search.h"

typedef struct {
    int games;
    int threads;
    uint64_t nodes;   /* per move */
    int random_plies; /* uniformly random moves at the start of each game */
    int max_plies;    /* longer games are scored as draws */
    uint64_t seed;
    int hash_mb;      /* per thread */
    const char *weights_path;
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
} Played;

/*
 * Game i always uses the same random seed, and games are written in order
 * of their number, so the output does not depend on the thread count.
 */
typedef struct {
    Config cfg;
    Weights weights;
    FILE *out;
    bool write_failed;

    pthread_mutex_t lock;
    int next_game;
    int next_write;
    Played **done; /* finished games waiting for their turn to be written */

    uint64_t positions, plies, nodes;
    int results[4];
    int ends[END_COUNT];
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

static int legal_moves(const Board *b, Move out[MAX_MOVES]) {
    MoveList list;
    movegen_generate(b, &list);
    int count = 0;
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (board_make_move(b, list.moves[i], &next))
            out[count++] = list.moves[i];
    }
    return count;
}

static Played *play_game(const Config *c, Search *s, int id) {
    uint64_t rng = c->seed ^ (0xD1B54A32D192ED03ULL * (uint64_t)(id + 1));
    Played *p = checked_alloc(sizeof *p);
    p->records = checked_alloc((size_t)c->max_plies * DATA_RECORD_SIZE);

    Board start;
    board_from_fen(&start, START_FEN);
    Game g;

    /* Random opening; start over if it already ended the game. */
    do {
        game_init(&g, &start);
        for (int ply = 0; ply < c->random_plies; ply++) {
            Move moves[MAX_MOVES];
            int n = legal_moves(&g.board, moves);
            if (!n)
                break;
            game_play(&g, moves[rng_below(&rng, (uint64_t)n)]);
        }
    } while (game_result(&g) != GAME_ONGOING);

    search_clear(s);
    int ply = c->random_plies;
    GameEnd end;
    GameResult result;
    while ((result = game_result_detail(&g, &end)) == GAME_ONGOING) {
        if (ply >= c->max_plies) {
            result = GAME_DRAW;
            end = END_PLY_LIMIT;
            break;
        }
        data_pack(&g.board, RESULT_DRAW, ply, p->records[p->count++]);

        SearchLimits limits;
        search_limits_init(&limits);
        limits.nodes = c->nodes;
        limits.seed = rng_next(&rng) | 1;
        SearchResult r = search_run(s, &g, &limits, false);
        p->nodes += r.nodes;
        game_play(&g, r.best);
        ply++;
    }

    int white_result = result == GAME_WHITE_WINS ? RESULT_WIN
                     : result == GAME_BLACK_WINS ? RESULT_LOSS : RESULT_DRAW;
    for (int i = 0; i < p->count; i++)
        data_set_result(p->records[i], white_result);
    p->result = result;
    p->end = end;
    p->plies = ply;
    return p;
}

/* Hands in a finished game and writes every game whose turn has come. */
static void store(Selfplay *sp, int id, Played *p) {
    pthread_mutex_lock(&sp->lock);
    sp->done[id] = p;
    while (sp->next_write < sp->cfg.games && sp->done[sp->next_write]) {
        Played *q = sp->done[sp->next_write];
        if (fwrite(q->records, DATA_RECORD_SIZE, (size_t)q->count, sp->out) != (size_t)q->count)
            sp->write_failed = true;
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

    for (;;) {
        pthread_mutex_lock(&sp->lock);
        int id = sp->next_game++;
        pthread_mutex_unlock(&sp->lock);
        if (id >= sp->cfg.games)
            break;
        store(sp, id, play_game(&sp->cfg, s, id));
    }
    search_free(s);
    return NULL;
}

static void usage(void) {
    fprintf(stderr,
            "usage: engine selfplay --out FILE [--games N] [--nodes N] [--threads N]\n"
            "                       [--weights FILE] [--seed N] [--random-plies N]\n"
            "                       [--max-plies N] [--hash MB]\n");
}

static bool parse_args(int argc, char **argv, Config *c) {
    for (int i = 1; i < argc; i += 2) {
        const char *key = argv[i];
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", key);
            return false;
        }
        const char *value = argv[i + 1];
        if (strcmp(key, "--out") == 0)
            c->out_path = value;
        else if (strcmp(key, "--weights") == 0)
            c->weights_path = value;
        else if (strcmp(key, "--games") == 0)
            c->games = atoi(value);
        else if (strcmp(key, "--nodes") == 0)
            c->nodes = strtoull(value, NULL, 10);
        else if (strcmp(key, "--threads") == 0)
            c->threads = atoi(value);
        else if (strcmp(key, "--seed") == 0)
            c->seed = strtoull(value, NULL, 10);
        else if (strcmp(key, "--random-plies") == 0)
            c->random_plies = atoi(value);
        else if (strcmp(key, "--max-plies") == 0)
            c->max_plies = atoi(value);
        else if (strcmp(key, "--hash") == 0)
            c->hash_mb = atoi(value);
        else {
            fprintf(stderr, "unknown option %s\n", key);
            return false;
        }
    }

    if (!c->out_path)
        fprintf(stderr, "--out is required\n");
    else if (c->games < 1 || c->nodes < 1 || c->threads < 1 || c->threads > 256 || c->hash_mb < 1
             || c->random_plies < 0 || c->max_plies <= c->random_plies || c->max_plies > 10000)
        fprintf(stderr, "invalid option value\n");
    else
        return true;
    return false;
}

int selfplay_main(int argc, char **argv) {
    Config c = {
        .games = 100, .threads = 1, .nodes = 5000, .random_plies = 8, .max_plies = 400,
        .seed = 1, .hash_mb = 4,
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
    sp->out = data_create(c.out_path);
    if (!sp->out) {
        fprintf(stderr, "cannot create %s\n", c.out_path);
        free(sp);
        return 1;
    }
    sp->done = checked_alloc((size_t)c.games * sizeof *sp->done);
    pthread_mutex_init(&sp->lock, NULL);

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
    bool ok = !sp->write_failed && fclose(sp->out) == 0;

    printf("games %d, positions %" PRIu64 ", white wins %d, draws %d, black wins %d\n", c.games,
           sp->positions, sp->results[GAME_WHITE_WINS], sp->results[GAME_DRAW],
           sp->results[GAME_BLACK_WINS]);
    printf("ends:");
    for (int e = END_MATE; e < END_COUNT; e++)
        printf(" %s %d", game_end_names[e], sp->ends[e]);
    printf("\naverage %.1f plies, %" PRIu64 " nodes, cpu %.2f s, wall %.2f s\n",
           (double)sp->plies / c.games, sp->nodes, cpu, wall);
    printf("summary games=%d positions=%" PRIu64 " white_wins=%d draws=%d black_wins=%d plies=%" PRIu64
           " nodes=%" PRIu64 " cpu_s=%.3f wall_s=%.3f\n",
           c.games, sp->positions, sp->results[GAME_WHITE_WINS], sp->results[GAME_DRAW],
           sp->results[GAME_BLACK_WINS], sp->plies, sp->nodes, cpu, wall);
    if (!ok)
        fprintf(stderr, "error writing %s\n", c.out_path);

    pthread_mutex_destroy(&sp->lock);
    free(threads);
    free(sp->done);
    free(sp);
    return ok ? 0 : 1;
}

int data_info_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: engine data-info FILE [samples]\n");
        return 2;
    }
    int samples = argc >= 3 ? atoi(argv[2]) : 5;

    char err[256];
    uint64_t count;
    FILE *f = data_open(argv[1], &count, err, sizeof err);
    if (!f) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    uint64_t results[3] = { 0 }, invalid = 0, in_check = 0, ply_sum = 0;
    uint8_t record[DATA_RECORD_SIZE];
    for (uint64_t i = 0; i < count; i++) {
        DataPosition pos;
        if (fread(record, 1, sizeof record, f) != sizeof record || !data_unpack(record, &pos)) {
            invalid++;
            continue;
        }
        results[pos.result]++;
        in_check += board_in_check(&pos.board);
        ply_sum += (uint64_t)pos.ply;
        if ((int64_t)i < samples) {
            char fen[FEN_MAX];
            board_to_fen(&pos.board, fen, sizeof fen);
            static const char *names[] = { "loss", "draw", "win" };
            printf("%-4s ply %3d  %s\n", names[pos.result], pos.ply, fen);
        }
    }
    fclose(f);

    printf("positions=%" PRIu64 " wins=%" PRIu64 " draws=%" PRIu64 " losses=%" PRIu64
           " in_check=%" PRIu64 " invalid=%" PRIu64 " average_ply=%.1f\n",
           count, results[RESULT_WIN], results[RESULT_DRAW], results[RESULT_LOSS], in_check, invalid,
           count > invalid ? (double)ply_sum / (double)(count - invalid) : 0.0);
    return invalid ? 1 : 0;
}
