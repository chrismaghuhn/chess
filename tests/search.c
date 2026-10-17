/*
 * Search and game tests:
 *   - mates are found with the right distance, checked against a brute-force
 *     mate solver that shares nothing with the search except the move generator
 *   - positions without legal moves
 *   - game end rules (mate, stalemate, repetition, 50 moves, material)
 *   - gen 0 plays complete games with only legal moves
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "movegen.h"
#include "search.h"

static int failures;

#define CHECK(cond, ...)               \
    do {                               \
        if (!(cond)) {                 \
            printf("FAIL " __VA_ARGS__); \
            printf("\n");              \
            failures++;                \
        }                              \
    } while (0)

/* ---- Brute-force mate solver ---- */

static bool can_mate_in(const Board *b, int moves);

static bool is_mated(const Board *b) {
    if (!board_in_check(b))
        return false;
    MoveList list;
    movegen_generate(b, &list);
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (board_make_move(b, list.moves[i], &next))
            return false;
    }
    return true;
}

/* Every legal reply still allows the opponent to mate within `moves` moves. */
static bool all_replies_lose(const Board *b, int moves) {
    MoveList list;
    movegen_generate(b, &list);
    bool any = false;
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (!board_make_move(b, list.moves[i], &next))
            continue;
        any = true;
        if (!can_mate_in(&next, moves))
            return false;
    }
    return any;
}

/* The side to move can force mate within `moves` of its own moves. */
static bool can_mate_in(const Board *b, int moves) {
    if (moves <= 0)
        return false;
    MoveList list;
    movegen_generate(b, &list);
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (!board_make_move(b, list.moves[i], &next))
            continue;
        if (is_mated(&next) || (moves > 1 && all_replies_lose(&next, moves - 1)))
            return true;
    }
    return false;
}

/* Exact mate distance as the search reports it (negative: side to move gets mated), 0 if none within max. */
static int oracle_mate(const Board *b, int max) {
    for (int n = 1; n <= max; n++) {
        if (can_mate_in(b, n))
            return n;
        if (all_replies_lose(b, n))
            return -n;
    }
    return 0;
}

/* ---- Tests ---- */

static Search *search;

static SearchResult search_fen(const char *fen, int depth, Game *game) {
    Board b;
    board_from_fen(&b, fen);
    game_init(game, &b);
    SearchLimits limits;
    search_limits_init(&limits);
    limits.depth = depth;
    search_clear(search);
    return search_run(search, game, &limits, false);
}

static void test_mates(void) {
    static const struct {
        const char *name;
        const char *fen;
        int mate;
    } cases[] = {
        { "back-rank mate in 1", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", 1 },
        { "rook ladder mate in 2", "7k/8/8/8/8/8/R7/1R4K1 w - - 0 1", 2 },
        { "rook ladder mate in 3", "8/6k1/8/8/8/8/R7/1R4K1 w - - 0 1", 3 },
        { "black mates in 3", "1r4k1/r7/8/8/8/8/6K1/8 b - - 0 1", 3 },
        { "gets mated in 1", "7k/1R6/8/8/8/8/R7/6K1 b - - 0 1", -1 },
        { "gets mated in 2", "8/6k1/1R6/8/8/8/R7/6K1 b - - 0 1", -2 },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int expected = cases[i].mate;
        int distance = expected > 0 ? expected : -expected;

        Board b;
        board_from_fen(&b, cases[i].fen);
        int oracle = oracle_mate(&b, distance);
        CHECK(oracle == expected, "%s: oracle says %d, test expects %d", cases[i].name, oracle, expected);

        Game game;
        SearchResult r = search_fen(cases[i].fen, 2 * distance + 2, &game);
        int found = search_mate_moves(r.score);
        CHECK(found == expected, "%s: search reports mate %d (score %d)", cases[i].name, found, r.score);

        if (expected > 0) {
            Board next;
            bool legal = r.best && board_make_move(&b, r.best, &next);
            bool mates = legal && (is_mated(&next) || all_replies_lose(&next, expected - 1));
            char move[6] = "0000";
            if (r.best)
                move_to_uci(r.best, move);
            CHECK(mates, "%s: best move %s does not mate in %d", cases[i].name, move, expected);
        }
    }
}

static void test_no_legal_moves(void) {
    Game game;
    SearchResult r = search_fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", 5, &game);
    CHECK(r.best == 0 && r.score == 0, "stalemate: best %d score %d", r.best, r.score);

    r = search_fen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1", 5, &game);
    CHECK(r.best == 0 && r.score == -SCORE_MATE, "mated: best %d score %d", r.best, r.score);
}

static bool play_moves(Game *g, const char *moves);

/* With eval 0 a draw only shows against a mate score, so each case pits one against the other. */
static void test_search_draws(void) {
    Game game;
    SearchResult r = search_fen("6k1/5ppp/8/8/8/8/8/R5K1 w - - 99 80", 4, &game);
    CHECK(search_mate_moves(r.score) == 1, "mate on the 100th ply beats the 50-move rule (score %d)", r.score);

    /* White's only move Kg1 runs into Rb1#, unless Kg1 repeats a position from the game. */
    const char *lost = "7k/8/8/8/8/1r6/r7/7K w - - 0 1";
    r = search_fen(lost, 4, &game);
    CHECK(search_mate_moves(r.score) == -1, "lost position without history (score %d)", r.score);

    Board b;
    board_from_fen(&b, lost);
    game_init(&game, &b);
    play_moves(&game, "h1g1 h8g8 g1h1 g8h8");
    SearchLimits limits;
    search_limits_init(&limits);
    limits.depth = 4;
    search_clear(search);
    r = search_run(search, &game, &limits, false);
    CHECK(r.score == 0, "repetition with game history is a draw (score %d)", r.score);
}

/* Plays UCI moves; returns false if one is illegal. */
static bool play_moves(Game *g, const char *moves) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", moves);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        Move m;
        if (!game_find_move(&g->board, tok, &m) || !game_play(g, m))
            return false;
    }
    return true;
}

static GameResult result_after(const char *fen, const char *moves) {
    Board b;
    Game g;
    board_from_fen(&b, fen);
    game_init(&g, &b);
    if (!play_moves(&g, moves))
        return (GameResult)-1;
    return game_result(&g);
}

static void test_game_results(void) {
    CHECK(result_after(START_FEN, "f2f3 e7e5 g2g4 d8h4") == GAME_BLACK_WINS, "fool's mate");
    CHECK(result_after("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", "") == GAME_DRAW, "stalemate");

    const char *shuffle = "g1f3 g8f6 f3g1 f6g8";
    CHECK(result_after(START_FEN, shuffle) == GAME_ONGOING, "second occurrence is not a draw");
    CHECK(result_after(START_FEN, "g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8") == GAME_DRAW,
          "threefold repetition");
    CHECK(result_after(START_FEN, "g1f3 g8f6 f3g1 f6g8 e2e4 e7e5 g1f3 g8f6 f3g1 f6g8") == GAME_ONGOING,
          "pawn move resets repetition");

    const char *rook_ending = "4k3/8/8/8/8/8/8/R3K3 w - - 99 80";
    CHECK(result_after(rook_ending, "") == GAME_ONGOING, "99 plies is not yet a draw");
    CHECK(result_after(rook_ending, "a1a2") == GAME_DRAW, "50-move rule");

    CHECK(result_after("8/8/8/4k3/8/8/8/4KB2 w - - 0 1", "") == GAME_DRAW, "K+B vs K");
    CHECK(result_after("8/8/8/4k3/8/8/8/4KN2 w - - 0 1", "") == GAME_DRAW, "K+N vs K");
    CHECK(result_after("8/8/8/4k3/8/8/8/3NKN2 w - - 0 1", "") == GAME_ONGOING, "K+N+N vs K");

    /* More reversible moves than the history holds: old hashes get dropped, nothing breaks. */
    Board b;
    Game g;
    board_from_fen(&b, "4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    game_init(&g, &b);
    bool ok = true;
    for (int i = 0; i < 300 && ok; i++)
        ok = play_moves(&g, "a1a2 e8d8 a2a1 d8e8");
    CHECK(ok && g.count <= GAME_MAX_HISTORY && g.count > 0 && g.hashes[g.count - 1] == g.board.hash,
          "history truncation (count %d)", g.count);
    CHECK(game_result(&g) == GAME_DRAW, "long shuffle ends in a draw");
}

static uint64_t rng_state = 0x2545F4914F6CDD1DULL;

static uint64_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Self-play with random openings: every move must be legal and every game must end by a rule or the ply cap. */
static void test_legal_games(void) {
    enum { GAMES = 20, RANDOM_PLIES = 8, MAX_PLIES = 300, NODES = 3000 };
    int results[4] = { 0 }, capped = 0;
    uint64_t nodes = 0;

    for (int game_no = 0; game_no < GAMES; game_no++) {
        Board start;
        Game g;
        board_from_fen(&start, START_FEN);
        game_init(&g, &start);
        search_clear(search);

        int ply = 0;
        for (; ply < MAX_PLIES && game_result(&g) == GAME_ONGOING; ply++) {
            Move m;
            if (ply < RANDOM_PLIES) {
                MoveList list, legal = { .count = 0 };
                movegen_generate(&g.board, &list);
                for (int i = 0; i < list.count; i++) {
                    Board next;
                    if (board_make_move(&g.board, list.moves[i], &next))
                        legal.moves[legal.count++] = list.moves[i];
                }
                m = legal.moves[rng_next() % (uint64_t)legal.count];
            } else {
                SearchLimits limits;
                search_limits_init(&limits);
                limits.nodes = NODES;
                SearchResult r = search_run(search, &g, &limits, false);
                nodes += r.nodes;
                m = r.best;
            }

            char name[6];
            move_to_uci(m, name);
            Move check;
            if (!m || !game_find_move(&g.board, name, &check) || check != m) {
                CHECK(false, "game %d ply %d: illegal move %s", game_no, ply, name);
                return;
            }
            game_play(&g, m);
            if (!board_is_consistent(&g.board)) {
                CHECK(false, "game %d ply %d: inconsistent board", game_no, ply);
                return;
            }
        }
        if (ply == MAX_PLIES)
            capped++;
        else
            results[game_result(&g)]++;
    }

    printf("legal games: %d white wins, %d black wins, %d draws, %d hit the %d-ply cap (%" PRIu64 " nodes)\n",
           results[GAME_WHITE_WINS], results[GAME_BLACK_WINS], results[GAME_DRAW], capped,
           (int)MAX_PLIES, nodes);
}

int main(void) {
    chess_init();
    search = search_new(16);

    test_mates();
    test_no_legal_moves();
    test_search_draws();
    test_game_results();
    test_legal_games();

    search_free(search);
    if (failures)
        printf("%d FAILED\n", failures);
    else
        printf("search tests: all passed\n");
    return failures ? 1 : 0;
}
