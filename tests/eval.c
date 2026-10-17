/*
 * Evaluation, weight file and training data tests.
 *   eval-test <tmp dir>
 * Leaves <tmp dir>/random-weights.bin behind for the self-play tests.
 */
#include <stdio.h>
#include <string.h>

#include "datafmt.h"
#include "eval.h"
#include "movegen.h"
#include "rng.h"

static int failures;

#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL " __VA_ARGS__); \
            printf("\n");                \
            failures++;                  \
        }                                \
    } while (0)

static const char *fens[] = {
    START_FEN,
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1",
};
#define FEN_COUNT (int)(sizeof fens / sizeof fens[0])

/* Plays n random legal moves; stops early when the game is over. */
static void random_walk(Board *b, int n, uint64_t *rng) {
    for (int i = 0; i < n; i++) {
        MoveList list;
        movegen_generate(b, &list);
        Move legal[MAX_MOVES];
        int count = 0;
        for (int j = 0; j < list.count; j++) {
            Board next;
            if (board_make_move(b, list.moves[j], &next))
                legal[count++] = list.moves[j];
        }
        if (!count)
            return;
        Board next;
        board_make_move(b, legal[rng_below(rng, (uint64_t)count)], &next);
        *b = next;
    }
}

/* The same position with colors swapped and the board mirrored top to bottom. */
static bool color_flip(const Board *b, Board *out) {
    uint8_t squares[64];
    for (int sq = 0; sq < 64; sq++) {
        int piece = b->squares[sq];
        squares[sq ^ 56] = piece == NO_PIECE ? NO_PIECE : (uint8_t)make_piece(piece_color(piece) ^ 1, piece_type(piece));
    }
    int castling = (b->castling & 3) << 2 | (b->castling >> 2 & 3);
    int ep = b->ep == SQ_NONE ? SQ_NONE : b->ep ^ 56;
    return board_setup(out, squares, b->side ^ 1, castling, ep, b->halfmove, b->fullmove);
}

static void random_weights(Weights *w, float *f, uint64_t *rng) {
    for (int i = 0; i < FEATURE_COUNT; i++) {
        f[i] = (float)((int)rng_below(rng, 401) - 200) + 0.25f;
        w->w[i] = (int32_t)(f[i] - 0.25f);
    }
}

static void test_eval(void) {
    uint64_t rng = 12345;
    Weights w;
    float f[FEATURE_COUNT];
    random_weights(&w, f, &rng);

    Board start;
    board_from_fen(&start, START_FEN);
    CHECK(eval_evaluate(&w, &start) == 0, "start position must be 0 for any weights");

    Weights zero;
    weights_zero(&zero);
    int checked = 0;
    for (int i = 0; i < FEN_COUNT; i++) {
        for (int walk = 0; walk < 40; walk++) {
            Board b;
            board_from_fen(&b, fens[i]);
            random_walk(&b, walk, &rng);

            Feature features[MAX_ACTIVE_FEATURES];
            int n = features_extract(&b, features);
            int sum = 0;
            for (int k = 0; k < n; k++)
                sum += features[k].sign * w.w[features[k].index];
            CHECK(n == popcount(b.all), "one feature per piece");
            CHECK(sum == eval_evaluate(&w, &b), "eval equals the feature sum");
            CHECK(eval_evaluate(&zero, &b) == 0, "zero weights give 0");

            Board flipped;
            bool ok = color_flip(&b, &flipped);
            CHECK(ok && eval_evaluate(&w, &flipped) == eval_evaluate(&w, &b),
                  "color flip changes the evaluation");
            checked++;
        }
    }
    printf("eval: %d positions checked\n", checked);
}

static void test_weight_files(const char *dir) {
    char path[512], err[256];
    uint64_t rng = 999;
    Weights w, loaded;
    float f[FEATURE_COUNT], back[FEATURE_COUNT];
    random_weights(&w, f, &rng);

    snprintf(path, sizeof path, "%s/random-weights.bin", dir);
    CHECK(weights_save_float(path, f), "save weights to %s", path);
    CHECK(weights_load_float(path, back, err, sizeof err) && memcmp(f, back, sizeof f) == 0,
          "float round trip: %s", err);
    CHECK(weights_load(path, &loaded, err, sizeof err), "load: %s", err);
    bool rounded = true;
    for (int i = 0; i < FEATURE_COUNT; i++)
        rounded = rounded && loaded.w[i] == w.w[i];
    CHECK(rounded, "weights round to the nearest integer");

    /* Broken files must be rejected. */
    char bad[512];
    snprintf(bad, sizeof bad, "%s/bad-weights.bin", dir);
    FILE *file = fopen(bad, "wb");
    fwrite("SCHW", 1, 4, file);
    fclose(file);
    CHECK(!weights_load(bad, &loaded, err, sizeof err), "truncated weight file accepted");

    file = fopen(bad, "wb");
    fwrite("XXXX", 1, 4, file);
    fwrite(f, sizeof f[0], FEATURE_COUNT, file);
    fclose(file);
    CHECK(!weights_load(bad, &loaded, err, sizeof err), "weight file with wrong magic accepted");

    snprintf(bad, sizeof bad, "%s/does-not-exist.bin", dir);
    CHECK(!weights_load(bad, &loaded, err, sizeof err), "missing weight file accepted");
}

static void test_data_format(const char *dir) {
    uint64_t rng = 4242;
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/format-test.bin", dir);

    FILE *f = data_create(path);
    CHECK(f != NULL, "create %s", path);
    if (!f)
        return;

    int written = 0;
    char expected[FEN_COUNT * 30][FEN_MAX];
    int results[FEN_COUNT * 30], plies[FEN_COUNT * 30];
    for (int i = 0; i < FEN_COUNT; i++) {
        for (int walk = 0; walk < 30; walk++) {
            Board b;
            board_from_fen(&b, fens[i]);
            random_walk(&b, walk * 3, &rng);
            b.fullmove = 1; /* not stored; rebuilt from the ply */
            uint8_t record[DATA_RECORD_SIZE];
            int result = (int)rng_below(&rng, 3), ply = written * 7;
            data_pack(&b, RESULT_DRAW, ply, record);
            data_set_result(record, result);
            fwrite(record, 1, sizeof record, f);

            /* data_set_result takes white's view; the record keeps the side to move's. */
            results[written] = b.side == WHITE ? result : RESULT_WIN - result;
            plies[written] = ply;
            b.fullmove = (uint16_t)(ply / 2 + 1);
            if (b.halfmove > 255)
                b.halfmove = 255;
            board_to_fen(&b, expected[written], FEN_MAX);
            written++;
        }
    }
    fclose(f);

    uint64_t count = 0;
    f = data_open(path, &count, err, sizeof err);
    CHECK(f && count == (uint64_t)written, "open data: %s (count %llu)", err, (unsigned long long)count);
    if (!f)
        return;
    for (int i = 0; i < written; i++) {
        uint8_t record[DATA_RECORD_SIZE];
        DataPosition pos;
        bool ok = fread(record, 1, sizeof record, f) == sizeof record && data_unpack(record, &pos);
        char fen[FEN_MAX] = "";
        if (ok)
            board_to_fen(&pos.board, fen, sizeof fen);
        CHECK(ok && strcmp(fen, expected[i]) == 0 && pos.result == results[i] && pos.ply == plies[i]
                  && board_is_consistent(&pos.board),
              "data round trip %d:\n  expected %s\n  got      %s", i, expected[i], fen);
    }
    fclose(f);

    /* A record without kings is not a position. */
    uint8_t record[DATA_RECORD_SIZE] = { 0 };
    DataPosition pos;
    CHECK(!data_unpack(record, &pos), "empty record accepted");

    /* Truncated files are rejected. */
    f = fopen(path, "ab");
    fputc(0, f);
    fclose(f);
    CHECK(data_open(path, &count, err, sizeof err) == NULL, "truncated data file accepted");
    printf("data format: %d records checked\n", written);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <tmp dir>\n", argv[0]);
        return 2;
    }
    chess_init();
    test_eval();
    test_weight_files(argv[1]);
    test_data_format(argv[1]);

    if (failures)
        printf("%d FAILED\n", failures);
    else
        printf("eval tests: all passed\n");
    return failures ? 1 : 0;
}
