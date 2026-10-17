#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "game.h"
#include "match.h"
#include "search.h"
#include "selfplay.h"
#include "uci.h"

#define BENCH_DEPTH 7

static const char *bench_fens[] = {
    START_FEN,
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
};

/*
 * Fixed-depth search over a few positions. The node count is a fingerprint
 * of the search: it changes exactly when search behavior changes.
 */
static int bench(int depth) {
    Search *s = search_new(16);
    SearchLimits limits;
    search_limits_init(&limits);
    limits.depth = depth;

    uint64_t nodes = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < sizeof bench_fens / sizeof bench_fens[0]; i++) {
        Board b;
        Game g;
        board_from_fen(&b, bench_fens[i]);
        game_init(&g, &b);
        search_clear(s);
        nodes += search_run(s, &g, &limits, false).nodes;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("%" PRIu64 " nodes %" PRIu64 " nps\n", nodes,
           elapsed > 0 ? (uint64_t)((double)nodes / elapsed) : 0);
    search_free(s);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    chess_init();

    if (argc >= 2 && strcmp(argv[1], "selfplay") == 0)
        return selfplay_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "match") == 0)
        return match_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "data-info") == 0)
        return data_info_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
        int depth = argc >= 3 ? atoi(argv[2]) : BENCH_DEPTH;
        return bench(depth > 0 ? depth : 1);
    }

    uci_loop();
    return 0;
}
