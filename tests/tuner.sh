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

# With validation, every 10th game is held out and the split adds up.
val=$(timeout 60 "$tuner" --data "$data" --out "$tmp/tuned-val.bin" --epochs 60 --report 0)
[ "$(kv "$val" validation)" -gt 0 ] && \
    [ $(($(kv "$val" train) + $(kv "$val" validation))) -eq "$(kv "$val" positions)" ] \
    || fail "validation split: $val"
[ "$(kv "$one" positions)" = "$(kv "$("$engine" data-info "$data" 0 | sed 's/^/summary /')" positions)" ] \
    || fail "tuner did not read every position"

# Leaf cache: a second run reads every leaf from the cache and gives the same weights; a changed
# data file invalidates its cache. Early stopping with patience ends before the epoch limit.
cp "$data" "$tmp/cache-data.bin"
rm -f "$tmp/cache-data.bin.leaves"
c1=$(timeout 60 "$tuner" --data "$tmp/cache-data.bin" --out "$tmp/cache1.bin" --epochs 30 --report 0 --leaf-cache on)
c2=$(timeout 60 "$tuner" --data "$tmp/cache-data.bin" --out "$tmp/cache2.bin" --epochs 30 --report 0 --leaf-cache on)
[ -f "$tmp/cache-data.bin.leaves" ] || fail "no leaf cache written"
[ "$(kv "$c1" cached)" = 0 ] && [ "$(kv "$c2" cached)" = "$(kv "$c2" positions)" ] \
    || fail "leaf cache not used: $(kv "$c1" cached) then $(kv "$c2" cached)"
cmp -s "$tmp/cache1.bin" "$tmp/cache2.bin" || fail "cached leaves give other weights"
timeout 60 "$tuner" --data "$tmp/cache-data.bin" --out "$tmp/cache0.bin" --epochs 30 --report 0 >/dev/null
cmp -s "$tmp/cache1.bin" "$tmp/cache0.bin" || fail "leaf cache changes the first run"
python3 -c '
import sys
p = sys.argv[1]; d = bytearray(open(p, "rb").read())
d[16 + 28] = (d[16 + 28] + 1) % 3   # another valid result for the first position
open(p, "wb").write(d)' "$tmp/cache-data.bin"
c3=$(timeout 60 "$tuner" --data "$tmp/cache-data.bin" --out "$tmp/cache3.bin" --epochs 30 --report 0 --leaf-cache on)
[ "$(kv "$c3" cached)" = 0 ] || fail "stale leaf cache used for a changed data file"
p=$(timeout 60 "$tuner" --data "$data" --out "$tmp/patience.bin" --epochs 400 --patience 5 --report 0)
[ "$(kv "$p" epochs)" -lt 400 ] && [ "$(kv "$p" epochs)" -le $(($(kv "$p" best_epoch) + 5)) ] \
    || fail "patience did not stop early: $p"

# Starting from earlier weights (and resolving positions with them) is deterministic too.
timeout 60 "$tuner" --data "$data" --init "$tmp/tuned1.bin" --out "$tmp/tuned-init1.bin" --epochs 5 \
    --threads 1 --report 0 >/dev/null || fail "tuner with --init"
timeout 60 "$tuner" --data "$data" --init "$tmp/tuned1.bin" --out "$tmp/tuned-init3.bin" --epochs 5 \
    --threads 3 --report 0 >/dev/null || fail "tuner with --init and 3 threads"
cmp -s "$tmp/tuned-init1.bin" "$tmp/tuned-init3.bin" || fail "resolving with --init depends on the thread count"
"$tuner" --data "$tmp/missing.bin" --out "$tmp/x.bin" 2>/dev/null && fail "missing data file accepted"
"$tuner" --data "$data" --out "$tmp/x.bin" --lr -1 2>/dev/null && fail "negative learning rate accepted"

# Matches are reproducible and add up.
m1=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b zero --games 20 --nodes 300 --threads 1 2>/dev/null)
m4=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b zero --games 20 --nodes 300 --threads 4 2>/dev/null)
[ "$(kv "$m1" a_wins)/$(kv "$m1" draws)/$(kv "$m1" b_wins)" = "$(kv "$m4" a_wins)/$(kv "$m4" draws)/$(kv "$m4" b_wins)" ] \
    || fail "match result depends on the thread count"
[ $(($(kv "$m1" a_wins) + $(kv "$m1" draws) + $(kv "$m1" b_wins))) -eq 20 ] || fail "match games do not add up"
"$engine" match --games 3 2>/dev/null && fail "odd number of match games accepted"

# Per-side node counts win over --nodes in any order.
m_a=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --games 4 --a-nodes 300 --nodes 600 --threads 2 --seed 3 2>/dev/null)
m_b=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --games 4 --nodes 600 --a-nodes 300 --threads 2 --seed 3 2>/dev/null)
[ "$(kv "$m_a" a_wins)/$(kv "$m_a" b_wins)" = "$(kv "$m_b" a_wins)/$(kv "$m_b" b_wins)" ] \
    || fail "match node flags depend on their order"

# plot.py never writes over a CSV.
python3 tools/plot.py "$tmp/x.csv" "$tmp/y.csv" 2>/dev/null && fail "plot accepted two CSVs without -o"

# A one-sided match still gives a finite Elo and error, flagged as only a bound.
side=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b "$tmp/tuned1.bin" --games 2 --nodes 300 2>/dev/null)
for key in elo elo_err; do
    case "$(kv "$side" $key)" in
        *inf* | *nan* | "") fail "match $key not finite: $side" ;;
    esac
done
one_sided=$(
    timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b zero --games 2 --nodes 300 2>/dev/null)
if [ "$(kv "$one_sided" a_wins)" = 2 ]; then
    [ "$(kv "$one_sided" saturated)" = 1 ] || fail "one-sided match not flagged: $one_sided"
    case "$(kv "$one_sided" elo_err)" in *inf* | *nan* | "") fail "one-sided match error: $one_sided" ;; esac
fi

# Against an external UCI engine (here: this engine itself), with a fixed time per move.
peer=$(timeout 60 "$engine" match --a "$tmp/tuned1.bin" --b-uci "$engine" --movetime 10 --games 2 --threads 2 2>&1)
[ $(($(kv "$peer" a_wins) + $(kv "$peer" draws) + $(kv "$peer" b_wins))) -eq 2 ] 2>/dev/null \
    || fail "match against a UCI engine: $peer"
"$engine" match --b-uci "$engine" --games 2 2>/dev/null && fail "UCI opponent without --movetime accepted"
"$engine" match --b-uci /nonexistent --movetime 10 --games 2 2>/dev/null && fail "missing UCI engine accepted"

# Network trainer: it can overfit a few games, and gives the same network for any thread count.
nnue_train="$(dirname "$tuner")/nnue-train"
fit=$(timeout 120 "$nnue_train" --data "$data" --resolve "$tmp/tuned1.bin" --out "$tmp/net1.bin" --hidden 16 \
    --train-games 10 --epochs 80 --batch 256 --lr 0.01 --wd 0 --threads 1 2>&1) || fail "nnue-train run"
first=$(printf '%s\n' "$fit" | sed -n 's/^epoch 1  train \([0-9.]*\)$/\1/p')
last=$(printf '%s\n' "$fit" | sed -n 's/^epoch 80  train \([0-9.]*\)$/\1/p')
awk "BEGIN { exit !($last < 0.5 * $first) }" 2>/dev/null || fail "nnue-train cannot overfit 10 games ($first -> $last)"
timeout 120 "$nnue_train" --data "$data" --resolve "$tmp/tuned1.bin" --out "$tmp/net3.bin" --hidden 16 \
    --train-games 10 --epochs 80 --batch 256 --lr 0.01 --wd 0 --threads 3 >/dev/null 2>&1 || fail "nnue-train 3 threads"
cmp -s "$tmp/net1.bin" "$tmp/net3.bin" || fail "network depends on the thread count"

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

# Every generation measured against one anchor, on one Elo scale.
GAMES=4 NODES=200 THREADS=4 ENGINE="$engine" timeout 120 sh tools/anchor.sh "$run" "$run/weights/gen2.bin" \
    >/dev/null 2>&1 || fail "anchor measurement"
[ "$(wc -l <"$run/anchor.csv")" -eq 3 ] || fail "anchor.csv should have a header and 2 rows"
python3 tools/plot.py -o "$run/anchor.svg" "$run/anchor.csv" "$run/generations.csv" >/dev/null || fail "plot of several runs"

# Other settings for an existing run are refused.
GAMES=14 NODES=200 MATCH_GAMES=6 EPOCHS=10 THREADS=4 ENGINE="$engine" TUNER="$tuner" \
    sh tools/generations.sh "$run" 2 2>/dev/null && fail "resume with other settings accepted"

# Redoing a step redoes everything after it: tune2, match2-vs-1, match2-vs-0.
rm "$run/logs/tune2.txt"
redo=$(GAMES=12 NODES=200 MATCH_GAMES=6 EPOCHS=10 THREADS=4 ENGINE="$engine" TUNER="$tuner" \
    sh tools/generations.sh "$run" 2 2>&1 | grep -c '^  ')
[ "$redo" -eq 3 ] || fail "redo after a changed step ran $redo steps instead of 3"

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILED"
    exit 1
fi
echo "tuner tests: all passed"
