/*
 * Perft suite: compares move tree sizes with known values. Any mismatch
 * means the move generator or make-move has a bug.
 *
 *   perft             cases up to 25M nodes
 *   perft --quick     cases up to 2M nodes (for sanitizer builds)
 *   perft --full      all cases
 *   perft divide <depth> <fen|startpos>
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "movegen.h"
#include "perft.h"

typedef struct {
    const char *name;
    const char *fen;
    int depth;
    uint64_t nodes;
} PerftCase;

#define KIWIPETE "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
#define POS3 "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"
#define POS4 "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1"
#define POS4_MIRROR "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1"
#define POS5 "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"
#define POS6 "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10"

static const PerftCase cases[] = {
    { "start", START_FEN, 1, 20 },
    { "start", START_FEN, 2, 400 },
    { "start", START_FEN, 3, 8902 },
    { "start", START_FEN, 4, 197281 },
    { "start", START_FEN, 5, 4865609 },
    { "start", START_FEN, 6, 119060324 },

    { "kiwipete", KIWIPETE, 1, 48 },
    { "kiwipete", KIWIPETE, 2, 2039 },
    { "kiwipete", KIWIPETE, 3, 97862 },
    { "kiwipete", KIWIPETE, 4, 4085603 },
    { "kiwipete", KIWIPETE, 5, 193690690 },

    { "pos3", POS3, 1, 14 },
    { "pos3", POS3, 2, 191 },
    { "pos3", POS3, 3, 2812 },
    { "pos3", POS3, 4, 43238 },
    { "pos3", POS3, 5, 674624 },
    { "pos3", POS3, 6, 11030083 },
    { "pos3", POS3, 7, 178633661 },

    { "pos4", POS4, 1, 6 },
    { "pos4", POS4, 2, 264 },
    { "pos4", POS4, 3, 9467 },
    { "pos4", POS4, 4, 422333 },
    { "pos4", POS4, 5, 15833292 },
    { "pos4", POS4, 6, 706045033 },

    { "pos4-mirror", POS4_MIRROR, 1, 6 },
    { "pos4-mirror", POS4_MIRROR, 2, 264 },
    { "pos4-mirror", POS4_MIRROR, 3, 9467 },
    { "pos4-mirror", POS4_MIRROR, 4, 422333 },
    { "pos4-mirror", POS4_MIRROR, 5, 15833292 },

    { "pos5", POS5, 1, 44 },
    { "pos5", POS5, 2, 1486 },
    { "pos5", POS5, 3, 62379 },
    { "pos5", POS5, 4, 2103487 },
    { "pos5", POS5, 5, 89941194 },

    { "pos6", POS6, 1, 46 },
    { "pos6", POS6, 2, 2079 },
    { "pos6", POS6, 3, 89890 },
    { "pos6", POS6, 4, 3894594 },
    { "pos6", POS6, 5, 164075551 },

    /* Special-rule edge cases. */
    { "ep-illegal-pin", "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1", 6, 1134888 },
    { "ep-illegal-check", "8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1", 6, 1015133 },
    { "ep-gives-check", "8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", 6, 1440467 },
    { "castle-gives-check-k", "5k2/8/8/8/8/8/8/4K2R w K - 0 1", 6, 661072 },
    { "castle-gives-check-q", "3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", 6, 803711 },
    { "castle-rights", "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", 4, 1274206 },
    { "castle-prevented", "r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1", 4, 1720476 },
    { "promo-out-of-check", "2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1", 6, 3821001 },
    { "discovered-check", "8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1", 5, 1004658 },
    { "promo-gives-check", "4k3/1P6/8/8/8/8/K7/8 w - - 0 1", 6, 217342 },
    { "underpromo-check", "8/P1k5/K7/8/8/8/8/8 w - - 0 1", 6, 92683 },
    { "self-stalemate", "K1k5/8/P7/8/8/8/8/8 w - - 0 1", 6, 2217 },
    { "stalemate-mate-1", "8/k1P5/8/1K6/8/8/8/8 w - - 0 1", 7, 567584 },
    { "stalemate-mate-2", "8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1", 4, 23527 },
};

/* FENs that must be rejected. */
static const char *invalid_fens[] = {
    "",
    "8/8/8/8/8/8/8/8 w - - 0 1",                                        /* no kings */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",         /* side */
    "rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",         /* rank too long */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",                  /* 7 ranks */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 extra",   /* trailing junk */
    "4k3/8/8/8/8/8/8/4K2P w - - 0 1",                                   /* pawn on rank 1 */
    "4k3/8/8/8/8/8/8/4K3 b - e3 0 1",                                   /* ep without pawn */
    "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1",                                  /* side not to move in check */
    "4k3/8/8/8/8/8/8/4K3 w - e30 1",                                    /* junk after ep square */
    "4k3/8/8/8/8/8/8/4K3 w - - 0 1 2",                                  /* too many counters */
    "qqqqk3/qqqqqq2/8/8/8/8/6PP/6RK b - - 0 1",                         /* 10 queens */
};

/* FENs that are valid but get normalized: { input, expected output }. */
static const char *normalized_fens[][2] = {
    { "4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1", "4k3/8/8/8/8/8/8/4K3 w - - 0 1" },
    { "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
      "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1" },
    { "4k3/8/8/8/8/8/8/4K3 w - -", "4k3/8/8/8/8/8/8/4K3 w - - 0 1" },
    { "4k3/8/8/8/8/8/8/4K3 w - - ", "4k3/8/8/8/8/8/8/4K3 w - - 0 1" },
    { "4k3/8/8/8/8/8/8/4K3 w - - 3 0", "4k3/8/8/8/8/8/8/4K3 w - - 3 1" },
    /* Nine queens are fine if all eight pawns promoted. */
    { "qqqqk3/qqqqq3/8/8/8/8/6PP/6RK b - - 0 1", "qqqqk3/qqqqq3/8/8/8/8/6PP/6RK b - - 0 1" },
};

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static double now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int test_fens(void) {
    int failures = 0;
    Board b;
    char out[FEN_MAX];

    for (int i = 0; i < COUNT(cases); i++) {
        if (!board_from_fen(&b, cases[i].fen) || !board_is_consistent(&b)) {
            printf("FAIL fen parse: %s\n", cases[i].fen);
            failures++;
            continue;
        }
        board_to_fen(&b, out, sizeof out);
        if (strcmp(out, cases[i].fen) != 0) {
            printf("FAIL fen round trip:\n  in:  %s\n  out: %s\n", cases[i].fen, out);
            failures++;
        }
    }

    for (int i = 0; i < COUNT(invalid_fens); i++) {
        if (board_from_fen(&b, invalid_fens[i])) {
            printf("FAIL fen accepted but invalid: \"%s\"\n", invalid_fens[i]);
            failures++;
        }
    }

    for (int i = 0; i < COUNT(normalized_fens); i++) {
        if (!board_from_fen(&b, normalized_fens[i][0])) {
            printf("FAIL fen parse: %s\n", normalized_fens[i][0]);
            failures++;
            continue;
        }
        board_to_fen(&b, out, sizeof out);
        if (strcmp(out, normalized_fens[i][1]) != 0) {
            printf("FAIL fen normalize:\n  in:       %s\n  expected: %s\n  out:      %s\n",
                   normalized_fens[i][0], normalized_fens[i][1], out);
            failures++;
        }
    }

    printf("fen: %s\n\n", failures ? "FAILED" : "ok");
    return failures;
}

/* The noisy generator must produce exactly the captures and promotions of the full one, in order. */
static int test_noisy_movegen(void) {
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    int failures = 0, positions = 0;

    for (int i = 0; i < COUNT(cases); i++) {
        if (i > 0 && strcmp(cases[i].fen, cases[i - 1].fen) == 0)
            continue;
        for (int walk = 0; walk < 50; walk++) {
            Board b;
            board_from_fen(&b, cases[i].fen);
            for (int ply = 0; ply < 60; ply++, positions++) {
                MoveList all, noisy, legal = { .count = 0 };
                movegen_generate(&b, &all);
                movegen_generate_noisy(&b, &noisy);

                int n = 0;
                bool same = true;
                for (int j = 0; j < all.count; j++) {
                    Move m = all.moves[j];
                    if (move_is_capture(m) || move_is_promo(m))
                        same = same && n < noisy.count && noisy.moves[n++] == m;
                    Board next;
                    if (board_make_move(&b, m, &next))
                        legal.moves[legal.count++] = m;
                }
                if (!same || n != noisy.count) {
                    char fen[FEN_MAX];
                    board_to_fen(&b, fen, sizeof fen);
                    printf("FAIL noisy movegen: %s\n", fen);
                    failures++;
                    break;
                }
                if (!legal.count)
                    break;

                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                Board next;
                board_make_move(&b, legal.moves[rng % (uint64_t)legal.count], &next);
                b = next;
            }
        }
    }
    printf("noisy movegen: %s (%d positions)\n\n", failures ? "FAILED" : "ok", positions);
    return failures;
}

static int test_perft(uint64_t max_nodes) {
    int failures = 0;
    uint64_t total_nodes = 0;
    double total_time = 0;

    for (int i = 0; i < COUNT(cases); i++) {
        const PerftCase *c = &cases[i];
        if (c->nodes > max_nodes)
            continue;

        Board b;
        if (!board_from_fen(&b, c->fen)) {
            printf("FAIL %-22s d%d  invalid FEN\n", c->name, c->depth);
            failures++;
            continue;
        }
        double start = now();
        uint64_t nodes = perft(&b, c->depth);
        double elapsed = now() - start;
        total_nodes += nodes;
        total_time += elapsed;

        bool ok = nodes == c->nodes;
        printf("%-4s %-22s d%d %12" PRIu64 "  %7.2fs", ok ? "ok" : "FAIL", c->name, c->depth,
               nodes, elapsed);
        if (!ok) {
            printf("  expected %" PRIu64, c->nodes);
            failures++;
        }
        printf("\n");
    }

    printf("\n%" PRIu64 " nodes in %.2fs (%.1f Mnps)\n", total_nodes, total_time,
           total_time > 0 ? (double)total_nodes / total_time / 1e6 : 0.0);
    return failures;
}

static int divide(int argc, char **argv) {
    int depth = atoi(argv[2]);
    if (depth < 1) {
        fprintf(stderr, "depth must be at least 1\n");
        return 2;
    }
    char fen[256] = "";
    size_t len = 0;
    for (int i = 3; i < argc && len < sizeof fen; i++)
        len += (size_t)snprintf(fen + len, sizeof fen - len, "%s%s", i > 3 ? " " : "", argv[i]);

    Board b;
    if (!board_from_fen(&b, strcmp(fen, "startpos") == 0 ? START_FEN : fen)) {
        fprintf(stderr, "invalid FEN: %s\n", fen);
        return 2;
    }
    board_print(&b, stdout);
    printf("\n");
    perft_divide(&b, depth, stdout);
    return 0;
}

int main(int argc, char **argv) {
    chess_init();

    if (argc >= 4 && strcmp(argv[1], "divide") == 0)
        return divide(argc, argv);

    uint64_t max_nodes = 25000000;
    if (argc == 2 && strcmp(argv[1], "--quick") == 0) {
        max_nodes = 2000000;
    } else if (argc == 2 && strcmp(argv[1], "--full") == 0) {
        max_nodes = UINT64_MAX;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--quick | --full]\n"
                        "       %s divide <depth> <fen | startpos>\n", argv[0], argv[0]);
        return 2;
    }

    int failures = test_fens() + test_noisy_movegen() + test_perft(max_nodes);
    if (failures)
        printf("%d FAILED\n", failures);
    else
        printf("all passed\n");
    return failures ? 1 : 0;
}
