#!/bin/sh
# UCI smoke test: drives the engine through a pipe and checks the replies.
# Usage: tests/uci.sh <engine binary>
engine="$1"
failures=0

expect() {
    if ! printf '%s\n' "$output" | grep -q "$1"; then
        echo "FAIL uci $2: expected '$1'"
        failures=$((failures + 1))
    fi
}

output=$(printf 'uci\nisready\nquit\n' | "$engine")
expect '^uciok$' "handshake"
expect '^readyok$' "isready"

output=$(printf 'position fen 6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1\ngo depth 4\n' | "$engine")
expect '^bestmove a1a8$' "mate in 1"
expect 'score mate 1' "mate score"

output=$(printf 'position startpos moves e2e4 e7e5 g1f3\nd\nquit\n' | "$engine")
expect 'rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2' "position with moves"

output=$(printf 'position startpos moves e2e4 e2e4\nquit\n' | "$engine")
expect 'illegal move e2e4' "illegal move"

output=$(printf 'position fen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\ngo depth 3\n' | "$engine")
expect '^bestmove 0000$' "no legal move"

output=$( (printf 'position startpos\ngo infinite\n'; sleep 0.3; printf 'stop\nquit\n') | "$engine")
expect '^bestmove [a-h][1-8][a-h][1-8]' "go infinite + stop"

output=$(printf 'position startpos\ngo wtime 1000 btime 1000 winc 10 binc 10\n' | "$engine")
expect '^bestmove [a-h][1-8][a-h][1-8]' "clock time"

output=$(printf 'setoption name Hash value 64\nisready\nquit\n' | "$engine")
expect '^readyok$' "setoption"

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILED"
    exit 1
fi
echo "uci tests: all passed"
