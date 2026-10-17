#define _POSIX_C_SOURCE 200809L

#include "match.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "eval.h"
#include "game.h"
#include "rng.h"
#include "search.h"

/*
 * Games come in pairs: one random opening, played once with A as white and
 * once with B as white, so neither side profits from a lucky opening. Both
 * sides search a fixed number of nodes per move and break ties at random
 * with seeds derived from the pair number, so a match is reproducible.
 */
typedef struct {
    int pairs;
    int threads;
    uint64_t nodes;
    int random_plies;
    int max_plies;
    uint64_t seed;
    int hash_mb;
    const char *paths[2];
} Config;

typedef struct {
    Config cfg;
    Weights weights[2]; /* A, B */
    pthread_mutex_t lock;
    int next_pair;
    int finished;
    int a_wins, draws, b_wins;
} Match;

static double clock_seconds(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Plays one game; returns the result from A's view: 2 win, 1 draw, 0 loss. */
static int play(const Config *c, Search *searchers[2], const Game *opening, int a_color, uint64_t *rng) {
    Game g = *opening;
    search_clear(searchers[0]);
    search_clear(searchers[1]);

    GameResult result;
    int ply = c->random_plies;
    while ((result = game_result(&g)) == GAME_ONGOING && ply < c->max_plies) {
        int mover = g.board.side == a_color ? 0 : 1;
        SearchLimits limits;
        search_limits_init(&limits);
        limits.nodes = c->nodes;
        limits.seed = rng_next(rng) | 1;
        game_play(&g, search_run(searchers[mover], &g, &limits, false).best);
        ply++;
    }
    if (result == GAME_ONGOING || result == GAME_DRAW)
        return 1;
    bool white_won = result == GAME_WHITE_WINS;
    return white_won == (a_color == WHITE) ? 2 : 0;
}

static void *worker(void *arg) {
    Match *m = arg;
    const Config *c = &m->cfg;
    Search *searchers[2] = { search_new((size_t)c->hash_mb), search_new((size_t)c->hash_mb) };
    if (!searchers[0] || !searchers[1]) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    search_set_weights(searchers[0], &m->weights[0]);
    search_set_weights(searchers[1], &m->weights[1]);

    Board start;
    board_from_fen(&start, START_FEN);
    for (;;) {
        pthread_mutex_lock(&m->lock);
        int pair = m->next_pair++;
        pthread_mutex_unlock(&m->lock);
        if (pair >= c->pairs)
            break;

        uint64_t rng = c->seed ^ (0xA24BAED4963EE407ULL * (uint64_t)(pair + 1));
        Game opening;
        if (!game_random_opening(&opening, &start, c->random_plies, &rng)) {
            fprintf(stderr, "pair %d: no random opening without a game end\n", pair);
            exit(1);
        }
        int scores[2];
        for (int a_color = 0; a_color < 2; a_color++) {
            uint64_t game_rng = rng ^ (uint64_t)(a_color + 1) * 0x9E3779B97F4A7C15ULL;
            scores[a_color] = play(c, searchers, &opening, a_color, &game_rng);
        }

        pthread_mutex_lock(&m->lock);
        for (int k = 0; k < 2; k++) {
            m->a_wins += scores[k] == 2;
            m->draws += scores[k] == 1;
            m->b_wins += scores[k] == 0;
        }
        m->finished++;
        int step = c->pairs >= 10 ? c->pairs / 10 : 1;
        if (m->finished % step == 0 || m->finished == c->pairs)
            fprintf(stderr, "%d/%d games  +%d =%d -%d\n", 2 * m->finished, 2 * c->pairs, m->a_wins,
                    m->draws, m->b_wins);
        pthread_mutex_unlock(&m->lock);
    }
    search_free(searchers[0]);
    search_free(searchers[1]);
    return NULL;
}

static double elo_from_score(double score) {
    return -400.0 * log10(1.0 / score - 1.0);
}

/*
 * Elo difference with a 95% interval from the per-game score variance.
 * Scores of 0 or 1 are pulled in by half a game so the estimate stays finite.
 */
static void elo_estimate(int wins, int draws, int losses, double *elo, double *error) {
    double n = wins + draws + losses;
    double score = (wins + 0.5 * draws) / n;
    double variance = (wins * (1 - score) * (1 - score) + draws * (0.5 - score) * (0.5 - score)
                       + losses * score * score) / n;
    double margin = 1.96 * sqrt(variance / n);
    double floor = 0.5 / n, ceiling = 1 - 0.5 / n;
    double clamp = score < floor ? floor : score > ceiling ? ceiling : score;
    double low = score - margin < floor ? floor : score - margin;
    double high = score + margin > ceiling ? ceiling : score + margin;
    *elo = elo_from_score(clamp);
    *error = (elo_from_score(high) - elo_from_score(low)) / 2;
}

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

static void usage(void) {
    fprintf(stderr, "usage: engine match [--a FILE|zero] [--b FILE|zero] [--games N] [--nodes N]\n"
                    "                    [--threads N] [--seed N] [--random-plies N] [--max-plies N]\n"
                    "                    [--hash MB]\n"
                    "Games are played in pairs with colors swapped; zero means all weights 0.\n");
}

int match_main(int argc, char **argv) {
    Config c = {
        .pairs = 100, .threads = 1, .nodes = 5000, .random_plies = 8, .max_plies = 400,
        .seed = 1, .hash_mb = 4, .paths = { "zero", "zero" },
    };
    for (int i = 1; i < argc; i += 2) {
        const char *key = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        int games = 0;
        bool ok = value != NULL;
        if (!ok)
            ;
        else if (strcmp(key, "--a") == 0)
            c.paths[0] = value;
        else if (strcmp(key, "--b") == 0)
            c.paths[1] = value;
        else if (strcmp(key, "--games") == 0)
            ok = parse_int(value, &games) && games >= 2 && games % 2 == 0 && (c.pairs = games / 2);
        else if (strcmp(key, "--nodes") == 0)
            ok = parse_u64(value, &c.nodes) && c.nodes > 0;
        else if (strcmp(key, "--threads") == 0)
            ok = parse_int(value, &c.threads) && c.threads >= 1 && c.threads <= 256;
        else if (strcmp(key, "--seed") == 0)
            ok = parse_u64(value, &c.seed);
        else if (strcmp(key, "--random-plies") == 0)
            ok = parse_int(value, &c.random_plies) && c.random_plies <= 40;
        else if (strcmp(key, "--max-plies") == 0)
            ok = parse_int(value, &c.max_plies) && c.max_plies <= 10000;
        else if (strcmp(key, "--hash") == 0)
            ok = parse_int(value, &c.hash_mb) && c.hash_mb >= 1;
        else
            ok = false;
        if (!ok) {
            fprintf(stderr, "bad option %s %s (games must be even)\n", key, value ? value : "");
            usage();
            return 2;
        }
    }
    if (c.max_plies <= c.random_plies) {
        usage();
        return 2;
    }

    Match *m = calloc(1, sizeof *m);
    if (!m) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    m->cfg = c;
    for (int k = 0; k < 2; k++) {
        char err[256];
        if (strcmp(c.paths[k], "zero") == 0) {
            weights_zero(&m->weights[k]);
        } else if (!weights_load(c.paths[k], &m->weights[k], err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            free(m);
            return 1;
        }
    }
    pthread_mutex_init(&m->lock, NULL);

    double cpu_start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
    double wall_start = clock_seconds(CLOCK_MONOTONIC);
    pthread_t *threads = calloc((size_t)c.threads, sizeof *threads);
    for (int t = 0; t < c.threads; t++)
        pthread_create(&threads[t], NULL, worker, m);
    for (int t = 0; t < c.threads; t++)
        pthread_join(threads[t], NULL);
    double cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
    double wall = clock_seconds(CLOCK_MONOTONIC) - wall_start;

    int games = 2 * c.pairs;
    double score = (m->a_wins + 0.5 * m->draws) / games, elo, error;
    elo_estimate(m->a_wins, m->draws, m->b_wins, &elo, &error);
    printf("%s vs %s: +%d =%d -%d, score %.1f%%, Elo %+.0f +/- %.0f\n", c.paths[0], c.paths[1],
           m->a_wins, m->draws, m->b_wins, 100 * score, elo, error);
    printf("summary games=%d a_wins=%d draws=%d b_wins=%d score=%.4f elo=%.1f elo_err=%.1f"
           " cpu_s=%.3f wall_s=%.3f\n",
           games, m->a_wins, m->draws, m->b_wins, score, elo, error, cpu, wall);

    pthread_mutex_destroy(&m->lock);
    free(threads);
    free(m);
    return 0;
}
