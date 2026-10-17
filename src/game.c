#include "game.h"

#include <string.h>

#include "movegen.h"

void game_init(Game *g, const Board *b) {
    g->board = *b;
    g->hashes[0] = b->hash;
    g->count = 1;
}

bool game_find_move(const Board *b, const char *uci, Move *out) {
    MoveList list;
    movegen_generate(b, &list);
    for (int i = 0; i < list.count; i++) {
        char name[6];
        move_to_uci(list.moves[i], name);
        Board next;
        if (strcmp(name, uci) == 0 && board_make_move(b, list.moves[i], &next)) {
            *out = list.moves[i];
            return true;
        }
    }
    return false;
}

bool game_play(Game *g, Move m) {
    Board next;
    if (!board_make_move(&g->board, m, &next))
        return false;
    g->board = next;

    /* Positions before a capture or pawn move can never repeat. */
    if (next.halfmove == 0) {
        g->count = 0;
    } else if (g->count == GAME_MAX_HISTORY) {
        int drop = GAME_MAX_HISTORY / 2;
        memmove(g->hashes, g->hashes + drop, (size_t)(g->count - drop) * sizeof g->hashes[0]);
        g->count -= drop;
    }
    g->hashes[g->count++] = next.hash;
    return true;
}

GameResult game_result(const Game *g) {
    const Board *b = &g->board;

    MoveList list;
    movegen_generate(b, &list);
    bool has_move = false;
    for (int i = 0; i < list.count && !has_move; i++) {
        Board next;
        has_move = board_make_move(b, list.moves[i], &next);
    }
    if (!has_move) {
        if (!board_in_check(b))
            return GAME_DRAW;
        return b->side == WHITE ? GAME_BLACK_WINS : GAME_WHITE_WINS;
    }

    if (b->halfmove >= 100 || board_insufficient_material(b))
        return GAME_DRAW;

    int repetitions = 1, last = g->count - 1;
    for (int i = last - 2; i >= 0 && last - i <= b->halfmove; i -= 2)
        if (g->hashes[i] == b->hash)
            repetitions++;
    return repetitions >= 3 ? GAME_DRAW : GAME_ONGOING;
}
