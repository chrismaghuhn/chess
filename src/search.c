#define _POSIX_C_SOURCE 200809L

#include "search.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "eval.h"
#include "movegen.h"
#include "rng.h"
#include "tt.h"

#define HISTORY_MAX 16384

/*
 * Move ordering uses no built-in chess knowledge: hash move first, then
 * captures and promotions, then killer moves, then the rest by history.
 * Captures are sorted by the *learned* piece values (most valuable victim,
 * then least valuable attacker), which come from the current weights; with
 * all weights 0 they are all equal and history decides, as in generation 0.
 * The king gets its learned value too, although it is only a positional
 * average (a king is never captured); that only affects capture order. The
 * tuner's printed piece values average over the squares seen in training
 * instead of the nonzero weights; the two agree up to rarely used squares.
 */
#define ORDER_HASH (INT64_C(1) << 60)
#define ORDER_NOISY (INT64_C(1) << 50)
#define ORDER_KILLER_1 (INT64_C(1) << 20)
#define ORDER_KILLER_2 (INT64_C(1) << 19)

static const Weights zero_weights;

struct Search {
    TT tt;
    const Weights *weights;
    int piece_value[6];     /* learned: average weight of each piece type */
    int history[2][64][64]; /* [side][from][to] */

    /* State of the current run. */
    const SearchLimits *limits;
    double start;
    double soft_limit; /* seconds; no new iteration after this, < 0 = none */
    double hard_limit; /* seconds; abort the search, < 0 = none */
    uint64_t nodes;
    int seldepth;
    bool stopped;
    Move killers[MAX_PLY][2];
    Move pv[MAX_PLY][MAX_PLY];
    int pv_len[MAX_PLY];
    uint64_t hashes[GAME_MAX_HISTORY + MAX_PLY]; /* game history, then the search path */
    int hash_count;
};

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double max_d(double a, double b) { return a > b ? a : b; }
static double min_d(double a, double b) { return a < b ? a : b; }

Search *search_new(size_t hash_mb) {
    Search *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (!tt_init(&s->tt, hash_mb)) {
        free(s);
        return NULL;
    }
    s->weights = &zero_weights;
    return s;
}

void search_set_weights(Search *s, const Weights *weights) {
    s->weights = weights ? weights : &zero_weights;

    /* Squares a piece never stood on in training keep weight 0 and are left out. */
    for (int type = 0; type < 6; type++) {
        int64_t sum = 0;
        int used = 0;
        for (int sq = 0; sq < 64; sq++) {
            int32_t w = s->weights->w[type * 64 + sq];
            sum += w;
            used += w != 0;
        }
        s->piece_value[type] = used ? (int)(sum / used) : 0;
    }
}

void search_free(Search *s) {
    if (!s)
        return;
    tt_free(&s->tt);
    free(s);
}

bool search_resize(Search *s, size_t hash_mb) {
    TT tt;
    if (!tt_init(&tt, hash_mb))
        return false;
    tt_free(&s->tt);
    s->tt = tt;
    return true;
}

void search_clear(Search *s) {
    tt_clear(&s->tt);
    memset(s->history, 0, sizeof s->history);
}

void search_limits_init(SearchLimits *limits) {
    memset(limits, 0, sizeof *limits);
    limits->time[WHITE] = limits->time[BLACK] = -1;
}

int search_mate_moves(int score) {
    if (score >= SCORE_MATE_BOUND)
        return (SCORE_MATE - score + 1) / 2;
    if (score <= -SCORE_MATE_BOUND)
        return -(SCORE_MATE + score) / 2;
    return 0;
}

/* Mate scores are stored relative to the node, not the root. */
static int score_to_tt(int score, int ply) {
    if (score >= SCORE_MATE_BOUND)
        return score + ply;
    if (score <= -SCORE_MATE_BOUND)
        return score - ply;
    return score;
}

static int score_from_tt(int score, int ply) {
    if (score >= SCORE_MATE_BOUND)
        return score - ply;
    if (score <= -SCORE_MATE_BOUND)
        return score + ply;
    return score;
}

static void set_time_limits(Search *s, const SearchLimits *l, int side) {
    const double overhead = 0.02; /* GUI and pipe latency */

    /* If the GUI only sends the other side's clock, assume ours is the same. */
    if (l->time[side] < 0 && l->time[side ^ 1] >= 0)
        side ^= 1;

    s->soft_limit = s->hard_limit = -1;
    if (l->movetime > 0) {
        s->soft_limit = s->hard_limit = max_d(0.001, l->movetime / 1000.0 - overhead);
    } else if (l->time[side] >= 0) {
        double left = l->time[side] / 1000.0;
        double inc = l->inc[side] > 0 ? l->inc[side] / 1000.0 : 0;
        int moves = l->movestogo > 0 ? l->movestogo : 30;
        double base = left / moves + inc * 0.75;
        s->hard_limit = max_d(0.001, min_d(left * 0.5, base * 3) - overhead);
        s->soft_limit = min_d(base, s->hard_limit);
    }
}

static bool should_stop(Search *s) {
    if (s->stopped)
        return true;
    if (s->limits->nodes && s->nodes >= s->limits->nodes)
        s->stopped = true;
    else if (s->limits->stop && atomic_load_explicit(s->limits->stop, memory_order_relaxed))
        s->stopped = true;
    else if (s->hard_limit >= 0 && (s->nodes & 1023) == 0 && now() - s->start >= s->hard_limit)
        s->stopped = true;
    return s->stopped;
}

static bool has_legal_move(const Board *b) {
    MoveList list;
    movegen_generate(b, &list);
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (board_make_move(b, list.moves[i], &next))
            return true;
    }
    return false;
}

/* Draw by rule or repetition. Any repetition inside the search counts. */
static bool is_draw(const Search *s, const Board *b) {
    if (board_insufficient_material(b))
        return true;
    /* Checkmate on the 100th ply still wins. */
    if (b->halfmove >= 100)
        return !board_in_check(b) || has_legal_move(b);
    int last = s->hash_count - 1;
    for (int i = last - 2; i >= 0 && last - i <= b->halfmove; i -= 2)
        if (s->hashes[i] == b->hash)
            return true;
    return false;
}

/* Learned gain of a capture or promotion, then the cheaper attacker first. */
static int64_t capture_order(const Search *s, const Board *b, Move m) {
    const int *value = s->piece_value;
    int flags = move_flags(m);
    int victim = flags == MF_EP_CAPTURE ? PAWN
               : move_is_capture(m) ? piece_type(b->squares[move_to(m)]) : -1;
    int64_t gain = victim >= 0 ? value[victim] : 0;
    if (move_is_promo(m))
        gain += value[move_promo_type(m)] - value[PAWN];
    return gain * 64 - value[piece_type(b->squares[move_from(m)])];
}

static void score_moves(const Search *s, const Board *b, const MoveList *list, int64_t *scores,
                        Move hash_move, int ply) {
    const int (*history)[64] = s->history[b->side];
    for (int i = 0; i < list->count; i++) {
        Move m = list->moves[i];
        int h = history[move_from(m)][move_to(m)];
        if (m == hash_move)
            scores[i] = ORDER_HASH;
        else if (move_is_capture(m) || move_is_promo(m))
            scores[i] = ORDER_NOISY + capture_order(s, b, m) * 32768 + h;
        else if (m == s->killers[ply][0])
            scores[i] = ORDER_KILLER_1;
        else if (m == s->killers[ply][1])
            scores[i] = ORDER_KILLER_2;
        else
            scores[i] = h;
    }
}

/* Selection sort step: moves the best remaining move to index i. */
static Move pick_move(MoveList *list, int64_t *scores, int i) {
    int best = i;
    for (int j = i + 1; j < list->count; j++)
        if (scores[j] > scores[best])
            best = j;
    Move m = list->moves[best];
    int64_t score = scores[best];
    list->moves[best] = list->moves[i];
    scores[best] = scores[i];
    list->moves[i] = m;
    scores[i] = score;
    return m;
}

static void reward_history(int *entry, int depth) {
    int bonus = depth * depth < HISTORY_MAX ? depth * depth : HISTORY_MAX;
    *entry += bonus - *entry * bonus / HISTORY_MAX; /* stays within HISTORY_MAX */
}

static int qsearch(Search *s, const Board *b, int alpha, int beta, int ply) {
    s->pv_len[ply] = 0;
    s->nodes++;
    if (ply > s->seldepth)
        s->seldepth = ply;
    if (should_stop(s))
        return 0;
    if (ply >= MAX_PLY - 1)
        return eval_evaluate(s->weights, b);

    /* In check there is no standing pat: try every evasion, none means mate. */
    bool in_check = board_in_check(b);
    int best;
    if (in_check) {
        best = -SCORE_MATE + ply;
    } else {
        best = eval_evaluate(s->weights, b);
        if (best >= beta)
            return best;
        if (best > alpha)
            alpha = best;
    }

    MoveList list;
    int64_t scores[MAX_MOVES];
    if (in_check)
        movegen_generate(b, &list);
    else
        movegen_generate_noisy(b, &list);
    score_moves(s, b, &list, scores, 0, ply);

    for (int i = 0; i < list.count; i++) {
        Move m = pick_move(&list, scores, i);
        Board next;
        if (!board_make_move(b, m, &next))
            continue;
        int score = -qsearch(s, &next, -beta, -alpha, ply + 1);
        if (s->stopped)
            return 0;
        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                s->pv[ply][0] = m;
                memcpy(&s->pv[ply][1], s->pv[ply + 1], (size_t)s->pv_len[ply + 1] * sizeof(Move));
                s->pv_len[ply] = s->pv_len[ply + 1] + 1;
                if (score >= beta)
                    break;
            }
        }
    }
    return best;
}

bool search_quiet(Search *s, const Board *b, uint64_t max_nodes, Board *leaf, int *score,
                  uint64_t *nodes) {
    SearchLimits limits;
    search_limits_init(&limits);
    limits.nodes = max_nodes;
    s->limits = &limits;
    s->soft_limit = s->hard_limit = -1;
    s->nodes = 0;
    s->stopped = false;

    int result = qsearch(s, b, -SCORE_INF, SCORE_INF, 0);
    if (nodes)
        *nodes = s->nodes;
    s->limits = NULL;
    *leaf = *b;
    if (s->stopped)
        return false;
    if (score)
        *score = result;

    for (int i = 0; i < s->pv_len[0]; i++) {
        Board next;
        board_make_move(leaf, s->pv[0][i], &next);
        *leaf = next;
    }
    return true;
}

static int negamax(Search *s, const Board *b, int depth, int alpha, int beta, int ply) {
    s->pv_len[ply] = 0;

    bool root = ply == 0;
    if (!root && is_draw(s, b))
        return 0;

    bool in_check = board_in_check(b);
    if (in_check)
        depth++; /* never stop the search while in check */
    if (depth <= 0)
        return qsearch(s, b, alpha, beta, ply);

    s->nodes++;
    if (ply > s->seldepth)
        s->seldepth = ply;

    if (!root) {
        if (ply >= MAX_PLY - 1)
            return eval_evaluate(s->weights, b);
        /* A mate found closer to the root can't be beaten here. */
        if (alpha < -SCORE_MATE + ply)
            alpha = -SCORE_MATE + ply;
        if (beta > SCORE_MATE - ply - 1)
            beta = SCORE_MATE - ply - 1;
        if (alpha >= beta)
            return alpha;
    }
    if (should_stop(s))
        return 0;

    Move hash_move = 0;
    const TTEntry *entry = tt_probe(&s->tt, b->hash);
    if (entry) {
        hash_move = entry->move;
        int score = score_from_tt(entry->score, ply);
        if (!root && entry->depth >= depth
            && (entry->bound == BOUND_EXACT
                || (entry->bound == BOUND_LOWER && score >= beta)
                || (entry->bound == BOUND_UPPER && score <= alpha)))
            return score;
    }

    MoveList list;
    int64_t scores[MAX_MOVES];
    movegen_generate(b, &list);
    score_moves(s, b, &list, scores, hash_move, ply);

    int original_alpha = alpha;
    int best = -SCORE_INF;
    Move best_move = 0;
    int legal = 0;

    /*
     * With a seed, the root picks at random among all moves with the best
     * score. To tell "equal" from "worse", later root moves are searched
     * with alpha one lower, so a tie comes back as an exact score.
     */
    bool random_ties = root && s->limits->seed;
    Move ties[MAX_MOVES];
    int tie_count = 0;

    for (int i = 0; i < list.count; i++) {
        Move m = pick_move(&list, scores, i);
        Board next;
        if (!board_make_move(b, m, &next))
            continue;
        legal++;

        int child_alpha = random_ties && alpha > -SCORE_INF ? alpha - 1 : alpha;
        s->hashes[s->hash_count++] = next.hash;
        int score = -negamax(s, &next, depth - 1, -beta, -child_alpha, ply + 1);
        s->hash_count--;
        if (s->stopped)
            return 0;

        if (random_ties && score == best)
            ties[tie_count++] = m;

        if (score > best) {
            best = score;
            best_move = m;
            ties[0] = m;
            tie_count = 1;
            if (score > alpha) {
                alpha = score;
                s->pv[ply][0] = m;
                memcpy(&s->pv[ply][1], s->pv[ply + 1], (size_t)s->pv_len[ply + 1] * sizeof(Move));
                s->pv_len[ply] = s->pv_len[ply + 1] + 1;

                if (score >= beta) {
                    reward_history(&s->history[b->side][move_from(m)][move_to(m)], depth);
                    if (!move_is_capture(m) && !move_is_promo(m) && s->killers[ply][0] != m) {
                        s->killers[ply][1] = s->killers[ply][0];
                        s->killers[ply][0] = m;
                    }
                    break;
                }
            }
        }
    }

    if (!legal)
        return in_check ? -SCORE_MATE + ply : 0;

    if (tie_count > 1) {
        /*
         * A fresh draw per depth: reusing one index would correlate with the
         * previous pick, because that move is moved to the front as hash move.
         */
        uint64_t rng = s->limits->seed ^ (uint64_t)depth * 0x9E3779B97F4A7C15ULL;
        Move pick = ties[rng_below(&rng, (uint64_t)tie_count)];
        if (pick != best_move) {
            best_move = pick;
            s->pv[0][0] = pick; /* its continuation was not kept */
            s->pv_len[0] = 1;
        }
    }

    int bound = best >= beta ? BOUND_LOWER : best > original_alpha ? BOUND_EXACT : BOUND_UPPER;
    tt_store(&s->tt, b->hash, best_move, score_to_tt(best, ply), depth, bound);
    return best;
}

static void print_info(const Search *s, int depth, int score) {
    double elapsed = now() - s->start;
    uint64_t nps = elapsed > 0 ? (uint64_t)((double)s->nodes / elapsed) : 0;

    char score_text[32];
    int mate = search_mate_moves(score);
    if (mate)
        snprintf(score_text, sizeof score_text, "mate %d", mate);
    else
        snprintf(score_text, sizeof score_text, "cp %d", score);

    char pv[MAX_PLY * 6 + 1] = "";
    size_t len = 0;
    for (int i = 0; i < s->pv_len[0]; i++) {
        char move[6];
        move_to_uci(s->pv[0][i], move);
        len += (size_t)snprintf(pv + len, sizeof pv - len, " %s", move);
    }

    printf("info depth %d seldepth %d score %s nodes %" PRIu64 " nps %" PRIu64
           " hashfull %d time %d pv%s\n",
           depth, s->seldepth, score_text, s->nodes, nps, tt_hashfull(&s->tt),
           (int)(elapsed * 1000), pv);
    fflush(stdout);
}

SearchResult search_run(Search *s, const Game *game, const SearchLimits *limits, bool print) {
    const Board *root = &game->board;
    SearchResult result = { 0, 0, 0, 0 };

    s->limits = limits;
    s->start = now();
    s->nodes = 0;
    s->stopped = false;
    memset(s->killers, 0, sizeof s->killers);
    for (int side = 0; side < 2; side++)
        for (int from = 0; from < 64; from++)
            for (int to = 0; to < 64; to++)
                s->history[side][from][to] /= 2;
    memcpy(s->hashes, game->hashes, (size_t)game->count * sizeof game->hashes[0]);
    s->hash_count = game->count;
    set_time_limits(s, limits, root->side);

    /* Fallback in case the first iteration gets cut off. */
    MoveList list;
    movegen_generate(root, &list);
    for (int i = 0; i < list.count && !result.best; i++) {
        Board next;
        if (board_make_move(root, list.moves[i], &next))
            result.best = list.moves[i];
    }
    if (!result.best) {
        result.score = board_in_check(root) ? -SCORE_MATE : 0;
        return result;
    }

    int max_depth = limits->depth > 0 && limits->depth < MAX_PLY ? limits->depth : MAX_PLY - 1;
    for (int depth = 1; depth <= max_depth; depth++) {
        s->seldepth = 0;
        int score = negamax(s, root, depth, -SCORE_INF, SCORE_INF, 0);
        if (s->stopped)
            break;

        if (s->pv_len[0] > 0)
            result.best = s->pv[0][0];
        result.score = score;
        result.depth = depth;
        if (print)
            print_info(s, depth, score);

        if (s->soft_limit >= 0 && now() - s->start >= s->soft_limit)
            break;
    }

    result.nodes = s->nodes;
    return result;
}
