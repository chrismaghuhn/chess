#!/bin/sh
# Tuner, match and generation loop tests.
# Usage: tests/tuner.sh <engine> <tuner> <tmp dir>  (selfplay.sh must have run first)
engine="$1"
tuner="$2"
tmp="$3"
failures=0
export LC_ALL=C

fail() {
    echo "FAIL tuner: $1"
    failures=$((failures + 1))
}

kv() {
    printf '%s\n' "$1" | sed -n "s/^summary .* $2=\([^ ]*\).*/\1/p; s/^summary $2=\([^ ]*\).*/\1/p"
}

data="$tmp/tune-data.bin"
timeout 60 "$engine" selfplay --games 60 --nodes 400 --threads 4 --seed 11 --out "$data" >/dev/null 2>&1 \
    || fail "self-play for tuner data"

# Tuning must lower the loss and give the same weights for any thread count. The data is too
# small for a meaningful validation split (early stopping would keep the start weights), so
# these runs train on everything.
one=$(timeout 60 "$tuner" --data "$data" --out "$tmp/tuned1.bin" --epochs 60 --threads 1 --report 0 \
    --val-every 0) || fail "tuner run"
timeout 60 "$tuner" --data "$data" --out "$tmp/tuned3.bin" --epochs 60 --threads 3 --report 0 \
    --val-every 0 >/dev/null || fail "tuner run with 3 threads"
cmp -s "$tmp/tuned1.bin" "$tmp/tuned3.bin" || fail "tuned weights depend on the thread count"
awk "BEGIN { exit !($(kv "$one" train_loss) < $(kv "$one" start_train_loss)) }" \
    || fail "training loss did not go down: $one"

# With validation, the tuner keeps the weights of the best validation epoch.
val=$(timeout 60 "$tuner" --data "$data" --out "$tmp/tuned-val.bin" --epochs 60 --report 0)
awk "BEGIN { exit !($(kv "$val" val_loss) <= $(kv "$val" start_val_loss)) }" \
    || fail "validation loss of the kept weights is worse than at the start: $val"
[ "$(kv "$one" positions)" = "$(kv "$("$engine" data-info "$data" 0 | sed 's/^/summary /')" positions)" ] \
    || fail "tuner did not read every position"

# Starting from earlier weights and resolving with them works too.
timeout 60 "$tuner" --data "$data" --init "$tmp/tuned1.bin" --out "$tmp/tuned-init.bin" --epochs 5 \
    --report 0 >/dev/null || fail "tuner with --init"
"$tuner" --data "$tmp/missing.bin" --out "$tmp/x.bin" 2>/dev/null && fail "missing data file accepted"
"$tuner" --data "$data" --out "$tmp/x.bin" --lr -1 2>/dev/null && fail "negative learning rate accepted"

# Matches are reproducible and add up.
m1=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b zero --games 20 --nodes 300 --threads 1 2>/dev/null)
m4=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b zero --games 20 --nodes 300 --threads 4 2>/dev/null)
[ "$(kv "$m1" a_wins)/$(kv "$m1" draws)/$(kv "$m1" b_wins)" = "$(kv "$m4" a_wins)/$(kv "$m4" draws)/$(kv "$m4" b_wins)" ] \
    || fail "match result depends on the thread count"
[ $(($(kv "$m1" a_wins) + $(kv "$m1" draws) + $(kv "$m1" b_wins))) -eq 20 ] || fail "match games do not add up"
"$engine" match --games 3 2>/dev/null && fail "odd number of match games accepted"

# The generation loop runs end to end and resumes without redoing anything.
run="$tmp/generations"
rm -rf "$run"
GAMES=12 NODES=200 MATCH_GAMES=6 EPOCHS=10 THREADS=4 ENGINE="$engine" TUNER="$tuner" \
    timeout 120 sh tools/generations.sh "$run" 2 2>/dev/null || fail "generation loop"
[ "$(wc -l <"$run/generations.csv")" -eq 4 ] || fail "generations.csv should have a header and 3 rows"
redo=$(GAMES=12 NODES=200 MATCH_GAMES=6 EPOCHS=10 THREADS=4 ENGINE="$engine" TUNER="$tuner" \
    sh tools/generations.sh "$run" 2 2>&1 | grep -c '^  ')
[ "$redo" -eq 0 ] || fail "resume redid $redo steps"
python3 tools/plot.py "$run/generations.csv" "$run/curve.svg" >/dev/null || fail "plot"

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILED"
    exit 1
fi
echo "tuner tests: all passed"
