#!/bin/sh
# UCI smoke test: drives the engine through a pipe and checks the replies.
# Every run has a timeout, so a search that ignores its limits fails instead of hanging.
# Usage: tests/uci.sh <engine binary>
engine="$1"
failures=0

run() {
    timeout 5 "$engine"
}

expect() {
    if ! printf '%s\n' "$output" | grep -q "$1"; then
        echo "FAIL uci $2: expected '$1'"
        failures=$((failures + 1))
    fi
}

output=$(printf 'uci\nisready\nquit\n' | run)
expect '^uciok$' "handshake"
expect '^readyok$' "isready"

output=$(printf 'position fen 6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1\ngo depth 4\n' | run)
expect '^bestmove a1a8$' "mate in 1"
expect 'score mate 1' "mate score"

output=$(printf 'position startpos moves e2e4 e7e5 g1f3\nd\nquit\n' | run)
expect 'rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2' "position with moves"

output=$(printf 'position fen 4k3/8/8/8/8/8/8/R3K3 w Q - 0 1 moves e1c1\nd\nquit\n' | run)
expect '4k3/8/8/8/8/8/8/2KR4 b - - 1 1' "position fen with moves"

output=$(printf 'position startpos moves e2e4\nposition fen garbage\nd\nquit\n' | run)
expect 'invalid position' "invalid fen reported"
expect 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1' "invalid fen keeps old position"

output=$(printf 'position startpos moves e2e4 e2e4\nquit\n' | run)
expect 'illegal move e2e4' "illegal move"

output=$(printf 'position fen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\ngo depth 3\n' | run)
expect '^bestmove 0000$' "no legal move"

# bestmove must come only after stop, so readyok (sent before stop) comes first.
output=$( (printf 'position startpos\ngo infinite\n'; sleep 0.3; printf 'isready\n'; sleep 0.2; printf 'stop\nquit\n') | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "go infinite + stop"
order=$(printf '%s\n' "$output" | grep -E '^(readyok|bestmove)' | tr '\n' ' ')
case "$order" in
    "readyok bestmove"*) ;;
    *) echo "FAIL uci go infinite: bestmove before stop ($order)"; failures=$((failures + 1)) ;;
esac

# Limits must end the search on their own (end of input waits for a finite search).
output=$(printf 'position startpos\ngo wtime 300 btime 300 winc 10 binc 10\n' | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "clock time"

output=$(printf 'position startpos moves e2e4\ngo wtime 300\n' | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "only the opponent's clock"

output=$(printf 'position startpos\ngo movetime 100\n' | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "movetime"

output=$(printf 'position startpos\ngo nodes 5000\n' | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "nodes"

output=$(printf 'position startpos\ngo depth 0\n' | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "depth 0"

output=$(printf 'position startpos\ngo movetime 0\n' | run)
expect '^bestmove [a-h][1-8][a-h][1-8]' "movetime 0"

output=$(printf 'setoption name Hash value 64\nisready\nquit\n' | run)
expect '^readyok$' "setoption"

# Playing style: with contempt a dead draw scores against the searching side; knight love raises the
# score of an extra knight (pawns on the board, so it is no material draw); bad values are refused.
output=$(printf 'setoption name Contempt value 50\nposition fen 8/8/8/4k3/8/8/8/4K3 w - - 0 1\ngo depth 3\n' \
    | (cat; sleep 1; echo quit) | run)
expect 'depth 3 .*score cp -50 ' "contempt scores a draw as -50"
score_with() { # KnightLove -> score at depth 1 with an extra white knight
    printf 'setoption name KnightLove value %s\nposition fen 4k3/p7/8/8/8/8/P7/1N2K3 w - - 0 1\ngo depth 1\n' "$1" \
        | (cat; sleep 1; echo quit) | run | sed -n 's/^info depth 1 .*score cp \(-\{0,1\}[0-9]*\) .*/\1/p'
}
[ $(($(score_with 100) - $(score_with 0))) -eq 100 ] 2>/dev/null \
    || { echo "FAIL uci KnightLove: $(score_with 0) -> $(score_with 100)"; failures=$((failures + 1)); }
output=$(printf 'setoption name Contempt value -5\nsetoption name KnightLove value 999\nquit\n' | run)
[ "$(printf '%s\n' "$output" | grep -c 'could not set')" -eq 2 ] || { echo "FAIL uci style values not checked"; failures=$((failures + 1)); }

# Search fingerprint with all weights 0. If a search change is meant to alter it,
# update the number here (and say so in the commit).
bench=$(timeout 60 "$engine" bench | sed -n 's/^\([0-9]*\) nodes.*/\1/p')
# "exact" is the training search: it must keep matching the engine of runs 1 and 4.
exact=$(timeout 60 "$engine" bench 7 exact | sed -n 's/^\([0-9]*\) nodes.*/\1/p')
[ "$exact" = 14201976 ] || { echo "FAIL uci bench exact: $exact nodes, expected 14201976"; failures=$((failures + 1)); }
[ "$bench" = 10195125 ] || { echo "FAIL uci bench: $bench nodes, expected 10195125"; failures=$((failures + 1)); }

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILED"
    exit 1
fi
echo "uci tests: all passed"
