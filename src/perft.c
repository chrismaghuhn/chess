#include "perft.h"

#include <assert.h>
#include <inttypes.h>

#include "movegen.h"

uint64_t perft(const Board *b, int depth) {
    if (depth == 0)
        return 1;

    MoveList list;
    movegen_generate(b, &list);

    uint64_t nodes = 0;
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (!board_make_move(b, list.moves[i], &next))
            continue;
        assert(board_is_consistent(&next));
        nodes += depth == 1 ? 1 : perft(&next, depth - 1);
    }
    return nodes;
}

uint64_t perft_divide(const Board *b, int depth, FILE *out) {
    MoveList list;
    movegen_generate(b, &list);

    uint64_t total = 0;
    for (int i = 0; i < list.count; i++) {
        Board next;
        if (!board_make_move(b, list.moves[i], &next))
            continue;
        uint64_t nodes = depth <= 1 ? 1 : perft(&next, depth - 1);
        char uci[6];
        move_to_uci(list.moves[i], uci);
        fprintf(out, "%s: %" PRIu64 "\n", uci, nodes);
        total += nodes;
    }
    fprintf(out, "\nNodes: %" PRIu64 "\n", total);
    return total;
}
