/*
 * Evaluation, weight file and training data tests.
 *   eval-test <tmp dir>
 * Leaves <tmp dir>/random-weights.bin behind for the self-play tests.
 */
#include <stdio.h>
#include <string.h>

#include "datafmt.h"
#include "eval.h"
#include "nnue.h"
#include <math.h>
#include <stdlib.h>
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
            CHECK(eval_from_psq(eval_psq(&w, &b), b.side) == eval_evaluate(&w, &b),
                  "incremental sum equals the feature sum");

            /* The search's incremental update must match a full recount after every legal move. */
            MoveList legal;
            movegen_legal(&b, &legal);
            for (int k = 0; k < legal.count; k++) {
                Board next;
                board_make_move(&b, legal.moves[k], &next);
                int want = eval_psq(&w, &next);
                int got = eval_psq(&w, &b) + eval_psq_delta(&w, &b, legal.moves[k]);
                char move[6];
                move_to_uci(legal.moves[k], move);
                CHECK(want == got, "eval delta of %s: %d instead of %d", move, got, want);
            }
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

    /* A good file with one thing broken at a time. */
    snprintf(bad, sizeof bad, "%s/bad-weights.bin", dir);
    static const struct {
        const char *what;
        long offset;
        unsigned char byte;
    } patches[] = {
        { "version 2", 4, 2 },
        { "feature count 383", 8, 127 },
        { "one byte too many", 12 + 4 * FEATURE_COUNT, 0 },
    };
    for (size_t i = 0; i < sizeof patches / sizeof patches[0]; i++) {
        weights_save_float(bad, f);
        file = fopen(bad, "r+b");
        fseek(file, patches[i].offset, SEEK_SET);
        fputc(patches[i].byte, file);
        fclose(file);
        CHECK(!weights_load(bad, &loaded, err, sizeof err), "weight file with %s accepted", patches[i].what);
    }

    float g[FEATURE_COUNT];
    memcpy(g, f, sizeof g);
    g[5] = 0.0f / 0.0f;
    weights_save_float(bad, g);
    CHECK(!weights_load(bad, &loaded, err, sizeof err), "weight file with NaN accepted");

    /* Rounding (half to even) and clamping. */
    memset(g, 0, sizeof g);
    g[0] = 20000.0f;
    g[1] = -20000.0f;
    g[2] = 2.5f;
    g[3] = 3.5f;
    g[4] = -2.5f;
    g[5] = 7.49f;
    weights_save_float(bad, g);
    CHECK(weights_load(bad, &loaded, err, sizeof err) && loaded.w[0] == WEIGHT_MAX
              && loaded.w[1] == -WEIGHT_MAX && loaded.w[2] == 2 && loaded.w[3] == 4
              && loaded.w[4] == -2 && loaded.w[5] == 7,
          "weight rounding and clamping");

    Weights big;
    for (int i = 0; i < FEATURE_COUNT; i++)
        big.w[i] = WEIGHT_MAX;
    Board queens;
    board_from_fen(&queens, "4k3/8/8/8/8/8/8/QQQQKQQQ w - - 0 1");
    CHECK(eval_evaluate(&big, &queens) == EVAL_MAX, "evaluation clamps at +EVAL_MAX");
    board_from_fen(&queens, "4k3/8/8/8/8/8/8/QQQQKQQQ b - - 0 1");
    CHECK(eval_evaluate(&big, &queens) == -EVAL_MAX, "evaluation clamps at -EVAL_MAX");
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
    int results[FEN_COUNT * 30], plies[FEN_COUNT * 30], scores[FEN_COUNT * 30];
    for (int i = 0; i < FEN_COUNT; i++) {
        for (int walk = 0; walk < 30; walk++) {
            Board b;
            board_from_fen(&b, fens[i]);
            random_walk(&b, walk * 3, &rng);
            b.fullmove = 1; /* not stored; rebuilt from the ply */
            uint8_t record[DATA_RECORD_SIZE];
            int result = (int)rng_below(&rng, 3), ply = written * 7;
            /* every third record without a score; one with a mate score */
            int score = written % 3 == 0 ? 0 : written == 1 ? -30990 : (int)rng_below(&rng, 4001) - 2000;
            data_pack(&b, RESULT_DRAW, ply, score, written % 3 != 0, record);
            scores[written] = score;
            data_set_outcome(record, result, (GameEnd)(written % END_COUNT));
            fwrite(record, 1, sizeof record, f);

            /* data_set_outcome takes white's view; the record keeps the side to move's. */
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
    int record_size = 0;
    f = data_open(path, &count, &record_size, err, sizeof err);
    CHECK(f && count == (uint64_t)written, "open data: %s (count %llu)", err, (unsigned long long)count);
    if (!f)
        return;
    for (int i = 0; i < written; i++) {
        uint8_t record[DATA_RECORD_SIZE];
        DataPosition pos;
        bool ok = data_read(f, record_size, record) && data_unpack(record, &pos);
        char fen[FEN_MAX] = "";
        if (ok)
            board_to_fen(&pos.board, fen, sizeof fen);
        CHECK(ok && strcmp(fen, expected[i]) == 0 && pos.result == results[i] && pos.ply == plies[i]
                  && pos.end == (GameEnd)(i % END_COUNT) && board_is_consistent(&pos.board)
                  && pos.score == scores[i] && pos.score_valid == (i % 3 != 0),
              "data round trip %d:\n  expected %s\n  got      %s", i, expected[i], fen);
    }
    fclose(f);

    /* A record without kings is not a position. */
    uint8_t record[DATA_RECORD_SIZE] = { 0 };
    DataPosition pos;
    CHECK(!data_unpack(record, &pos), "empty record accepted");

    /* Valid records with one byte broken. */
    static const struct {
        const char *what;
        const char *fen;
        int offset;
        uint8_t value;
    } broken[] = {
        { "side 2", START_FEN, 24, 2 },
        { "result 3", START_FEN, 28, 3 },
        { "unknown game end", START_FEN, 29, END_COUNT },
        { "piece code 12", START_FEN, 8, 0xCC },
        { "en passant on the wrong rank", START_FEN, 26, E3 },
        { "unused nibble set", "4k3/8/8/8/8/8/8/4K3 w - - 0 1", 9, 0x01 },
        { "more than 32 pieces", START_FEN, 2, 0xFF },
        { "a score marked invalid", START_FEN, 32, 5 },
        { "unknown flags", START_FEN, 34, 4 },
        { "reserved byte 35", START_FEN, 35, 1 },
    };
    for (size_t i = 0; i < sizeof broken / sizeof broken[0]; i++) {
        Board b;
        board_from_fen(&b, broken[i].fen);
        data_pack(&b, RESULT_DRAW, 0, 0, false, record);
        CHECK(data_unpack(record, &pos), "unbroken record for %s rejected", broken[i].what);
        record[broken[i].offset] = broken[i].value;
        CHECK(!data_unpack(record, &pos), "record with %s accepted", broken[i].what);
    }

    /* The header's reserved word must be zero. */
    f = fopen(path, "r+b");
    fseek(f, 12, SEEK_SET);
    fputc(1, f);
    fclose(f);
    CHECK(data_open(path, &count, &record_size, err, sizeof err) == NULL, "data file with reserved header word accepted");
    f = fopen(path, "r+b");
    fseek(f, 12, SEEK_SET);
    fputc(0, f);
    fclose(f);

    /* Truncated files are rejected. */
    f = fopen(path, "ab");
    fputc(0, f);
    fclose(f);
    CHECK(data_open(path, &count, &record_size, err, sizeof err) == NULL, "truncated data file accepted");

    /* Version 1 files (32-byte records, no scores) stay readable. */
    char v1[512];
    snprintf(v1, sizeof v1, "%s/format-v1.bin", dir);
    f = fopen(v1, "wb");
    uint8_t header[DATA_HEADER_SIZE] = { 'S', 'C', 'H', 'D', 1, 0, 0, 0, DATA_RECORD_SIZE_V1, 0, 0, 0 };
    fwrite(header, 1, sizeof header, f);
    Board b;
    board_from_fen(&b, fens[1]);
    for (int i = 0; i < 3; i++) {
        data_pack(&b, RESULT_DRAW, 10 + i, 0, false, record);
        data_set_outcome(record, RESULT_WIN, END_MATE);
        fwrite(record, 1, DATA_RECORD_SIZE_V1, f);
    }
    fclose(f);
    f = data_open(v1, &count, &record_size, err, sizeof err);
    bool v1_ok = f && count == 3 && record_size == DATA_RECORD_SIZE_V1;
    for (int i = 0; i < 3 && v1_ok; i++)
        v1_ok = data_read(f, record_size, record) && data_unpack(record, &pos) && pos.ply == 10 + i
             && !pos.score_valid && pos.end == END_MATE;
    if (f)
        fclose(f);
    CHECK(v1_ok, "version 1 data file not readable: %s", err);
    printf("data format: %d records checked\n", written);
}

/* The network: gradients, symmetry, the linear skip part, files. */
static void test_nnue(const char *dir) {
    uint64_t rng = 31337;
    Network net;
    CHECK(nnue_alloc(&net, 4), "allocate network");
    for (int i = 0; i < NNUE_INPUTS; i++)
        net.skip[i] = (float)((int)rng_below(&rng, 201) - 100);
    for (int i = 0; i < NNUE_INPUTS * 4; i++)
        net.w1[i] = (float)((int)rng_below(&rng, 2001) - 1000) / 10000.0f;
    for (int h = 0; h < 4; h++)
        net.b1[h] = 0.3f + 0.1f * (float)h;
    for (int h = 0; h < 8; h++)
        net.w2[h] = (float)((int)rng_below(&rng, 2001) - 1000) / 1000.0f;
    net.b2 = 0.05f;

    /* Analytic gradients against finite differences, for parameters of every group. */
    Board b;
    board_from_fen(&b, fens[1]);
    uint16_t in[NNUE_MAX_ACTIVE];
    int n = nnue_inputs(&b, in);
    Network grad;
    nnue_alloc(&grad, 4);
    NnueCache cache;
    nnue_forward(&net, in, n, &cache);
    nnue_backward(&net, in, n, &cache, 1.0f, &grad);
    float *params[] = { &net.skip[in[3]], &net.w1[in[5] * 4 + 1], &net.w1[nnue_flip(in[7]) * 4 + 2], &net.b1[0],
                        &net.w2[1], &net.w2[6], &net.b2 };
    float *grads[] = { &grad.skip[in[3]], &grad.w1[in[5] * 4 + 1], &grad.w1[nnue_flip(in[7]) * 4 + 2], &grad.b1[0],
                       &grad.w2[1], &grad.w2[6], &grad.b2 };
    for (size_t k = 0; k < sizeof params / sizeof params[0]; k++) {
        float keep = *params[k], h = k == 0 ? 0.5f : 1e-3f;
        *params[k] = keep + h;
        double up = nnue_forward(&net, in, n, &cache);
        *params[k] = keep - h;
        double down = nnue_forward(&net, in, n, &cache);
        *params[k] = keep;
        double numeric = (up - down) / (2 * h), analytic = *grads[k];
        CHECK(fabs(numeric - analytic) <= 0.02 * fabs(numeric) + 0.05, "nnue gradient %zu: %f vs %f", k, analytic,
              numeric);
    }
    nnue_free(&grad);

    /* Colour flip and mirror must not change the evaluation. */
    for (int i = 0; i < 200; i++) {
        Board p, flipped;
        board_from_fen(&p, fens[i % FEN_COUNT]);
        random_walk(&p, i % 50, &rng);
        if (color_flip(&p, &flipped))
            CHECK(abs(nnue_evaluate(&net, &p) - nnue_evaluate(&net, &flipped)) <= 1,
                  "nnue colour flip: %d vs %d", nnue_evaluate(&net, &p), nnue_evaluate(&net, &flipped));
    }

    /* Saving and loading keeps every value. */
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/test-net.bin", dir);
    Network loaded;
    CHECK(nnue_save(&net, path) && nnue_load(&loaded, path, err, sizeof err), "nnue file round trip: %s", err);
    CHECK(loaded.hidden == 4 && nnue_evaluate(&loaded, &b) == nnue_evaluate(&net, &b), "nnue loaded network differs");
    nnue_free(&loaded);
    nnue_free(&net);

    /* A linear-only network from linear weights evaluates exactly like the linear evaluation. */
    Weights w;
    float f[FEATURE_COUNT];
    random_weights(&w, f, &rng);
    Network linear;
    nnue_alloc(&linear, 0);
    nnue_skip_from_weights(&linear, &w);
    for (int i = 0; i < 200; i++) {
        Board p;
        board_from_fen(&p, fens[i % FEN_COUNT]);
        random_walk(&p, i % 50, &rng);
        if (nnue_evaluate(&linear, &p) != eval_evaluate(&w, &p)) {
            CHECK(false, "linear network %d vs linear eval %d", nnue_evaluate(&linear, &p), eval_evaluate(&w, &p));
            break;
        }
    }
    nnue_free(&linear);

    /* Integer form: accumulators updated move by move (null moves too) equal a fresh computation, and the
       evaluation matches the float network up to rounding. 20 hidden units: padded, more than one lane block. */
    Network big;
    QNetwork q;
    CHECK(nnue_alloc(&big, 20), "allocate network");
    for (int i = 0; i < NNUE_INPUTS; i++)
        big.skip[i] = (float)((int)rng_below(&rng, 201) - 100) / 3.0f;
    for (int i = 0; i < NNUE_INPUTS * 20; i++)
        big.w1[i] = (float)((int)rng_below(&rng, 2001) - 1000) / 20000.0f;
    for (int h = 0; h < 20; h++)
        big.b1[h] = 0.5f + 0.02f * (float)h;
    for (int h = 0; h < 40; h++)
        big.w2[h] = (float)((int)rng_below(&rng, 2001) - 1000) / 1000.0f;
    big.b2 = -0.1f;
    CHECK(nnue_quantize(&big, &q), "quantize network");
    int updates = 0, worst = 0;
    for (int game = 0; game < 40 && failures == 0; game++) {
        Board p;
        board_from_fen(&p, fens[game % FEN_COUNT]);
        NnueAccumulator acc, fresh;
        nnue_refresh(&q, &p, &acc);
        for (int ply = 0; ply < 120; ply++) {
            MoveList list;
            movegen_legal(&p, &list);
            if (!list.count)
                break;
            Board next;
            if (!board_in_check(&p) && rng_below(&rng, 10) == 0)
                board_make_null(&p, &next);
            else
                board_make_move(&p, list.moves[rng_below(&rng, (uint64_t)list.count)], &next);
            NnueAccumulator after;
            nnue_update(&q, &p, &next, &acc, &after);
            nnue_refresh(&q, &next, &fresh);
            updates++;
            size_t used = (size_t)q.padded * sizeof(int16_t); /* the rest of the arrays is unused */
            if (memcmp(after.acc[0], fresh.acc[0], used) != 0 || memcmp(after.acc[1], fresh.acc[1], used) != 0
                || after.skip[0] != fresh.skip[0]
                || after.skip[1] != fresh.skip[1]) {
                CHECK(false, "nnue update differs from refresh after ply %d of game %d", ply, game);
                break;
            }
            int diff = abs(nnue_evaluate_quantized(&q, &after, next.side) - nnue_evaluate(&big, &next));
            worst = diff > worst ? diff : worst;
            acc = after;
            p = next;
        }
    }
    CHECK(worst <= 2, "integer network differs from float by %d", worst);
    nnue_quantized_free(&q);
    nnue_free(&big);
    printf("nnue: checked (%d incremental updates, integer vs float at most %d)\n", updates, worst);
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
    test_nnue(argv[1]);

    if (failures)
        printf("%d FAILED\n", failures);
    else
        printf("eval tests: all passed\n");
    return failures ? 1 : 0;
}
