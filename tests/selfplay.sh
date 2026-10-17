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

# Training self-play with weights and --search exact is pinned: it must stay byte-identical to
# the engine that produced runs 1 and 4. Update only for an intended change of training play.
timeout 60 "$engine" selfplay --games 6 --nodes 1500 --threads 3 --seed 5 --weights "$tmp/random-weights.bin" \
    --search exact --out "$tmp/pin.bin" >/dev/null 2>&1 || fail "pinned self-play run"
# Compared as version 1 data (header v1, first 32 bytes of every record): the games must be the same.
v1sha=$(python3 -c '
import hashlib, sys
d = open(sys.argv[1], "rb").read()
out = bytearray(b"SCHD" + (1).to_bytes(4, "little") + (32).to_bytes(4, "little") + bytes(4))
for i in range(16, len(d), 36):
    out += d[i:i + 32]
print(hashlib.sha256(out).hexdigest())' "$tmp/pin.bin")
[ "$v1sha" = 10416e78542d270b4a922a96bdd65eda4f83b824a0bc5e71d63ea338bfb5be99 ] \
    || fail "self-play with --search exact changed (training would no longer reproduce runs 1/4)"

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

# Labels must match how each game ended (checked by data-info), for every end reason seen.
labels=$("$engine" data-info "$tmp/four.bin" 0)
case "$labels" in
    *"games=12 "*"label_errors=0 "*) ;;
    *) fail "labels or game count wrong: $labels" ;;
esac

# Calibrated resignation: same data for any thread count, labels consistent, threshold reported.
r1=$(timeout 120 "$engine" selfplay --games 40 --nodes 300 --threads 1 --seed 9 --resign on \
    --weights "$tmp/random-weights-v1.bin" --out "$tmp/resign1.bin" 2>/dev/null) || fail "self-play with resignation"
timeout 120 "$engine" selfplay --games 40 --nodes 300 --threads 3 --seed 9 --resign on \
    --weights "$tmp/random-weights-v1.bin" --out "$tmp/resign3.bin" >/dev/null 2>&1 || fail "resignation, 3 threads"
cmp -s "$tmp/resign1.bin" "$tmp/resign3.bin" || fail "resignation depends on the thread count"
printf '%s\n' "$r1" | grep -q "resign_threshold=" || fail "no resignation report: $r1"
case "$("$engine" data-info "$tmp/resign1.bin" 0)" in
    *"label_errors=0 "*) ;;
    *) fail "labels wrong with resignation" ;;
esac
"$engine" selfplay --games 2 --out "$tmp/x.bin" --resign maybe 2>/dev/null && fail "bad --resign value accepted"

# Bad arguments are rejected instead of being read as some other number.
for args in "--nodes -1" "--nodes 10x" "--seed abc" "--games 0" "--random-plies 41"; do
    # shellcheck disable=SC2086
    timeout 5 "$engine" selfplay --out "$tmp/x.bin" $args 2>/dev/null
    [ $? -eq 2 ] || fail "selfplay accepted $args"
done

# A write error ends the run with an error and no summary line.
if [ -w /dev/full ]; then
    output=$(timeout 30 "$engine" selfplay --games 50 --nodes 200 --out /dev/full 2>/dev/null)
    [ $? -eq 1 ] || fail "write error not reported"
    printf '%s\n' "$output" | grep -q summary && fail "summary printed after a write error"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILED"
    exit 1
fi
echo "selfplay tests: all passed"
