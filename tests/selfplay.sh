#!/bin/sh
# Self-play tests: reproducible output, data that reads back, weights that matter.
# Usage: tests/selfplay.sh <engine binary> <tmp dir>  (eval-test must have run first)
engine="$1"
tmp="$2"
failures=0

fail() {
    echo "FAIL selfplay: $1"
    failures=$((failures + 1))
}

selfplay() {
    out="$1"
    shift
    timeout 60 "$engine" selfplay --games 12 --nodes 400 --out "$tmp/$out" "$@" 2>/dev/null
}

summary=$(selfplay one.bin --threads 1 --seed 7) || fail "run with 1 thread"
selfplay four.bin --threads 4 --seed 7 >/dev/null || fail "run with 4 threads"
cmp -s "$tmp/one.bin" "$tmp/four.bin" || fail "output depends on the thread count"

selfplay other.bin --threads 4 --seed 8 >/dev/null || fail "run with another seed"
cmp -s "$tmp/one.bin" "$tmp/other.bin" && fail "different seeds give identical data"

selfplay weighted.bin --threads 4 --seed 7 --weights "$tmp/random-weights.bin" >/dev/null \
    || fail "run with weights"
cmp -s "$tmp/one.bin" "$tmp/weighted.bin" && fail "weights do not change the games"

"$engine" selfplay --games 1 --out "$tmp/x.bin" --weights "$tmp/missing.bin" 2>/dev/null \
    && fail "missing weight file accepted"

# data-info must read back exactly what selfplay reported.
positions=$(printf '%s\n' "$summary" | sed -n 's/.*summary .*positions=\([0-9]*\).*/\1/p')
info=$("$engine" data-info "$tmp/one.bin" 0) || fail "data-info reports invalid records"
case "$info" in
    *"positions=$positions "*"invalid=0"*) ;;
    *) fail "data-info disagrees with selfplay ($positions positions): $info" ;;
esac
[ "${positions:-0}" -gt 0 ] || fail "no positions written"

# The UCI option loads the same weights.
output=$(printf 'setoption name Weights value %s\nposition startpos moves e2e4\ngo depth 3\n' \
    "$tmp/random-weights.bin" | timeout 5 "$engine")
printf '%s\n' "$output" | grep -q 'loaded weights' || fail "uci Weights option"
printf '%s\n' "$output" | grep -q '^bestmove ' || fail "uci search with weights"
output=$(printf 'setoption name Weights value %s\nquit\n' "$tmp/missing.bin" | timeout 5 "$engine")
printf '%s\n' "$output" | grep -q 'cannot open' || fail "uci reports a missing weight file"

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILED"
    exit 1
fi
echo "selfplay tests: all passed"
