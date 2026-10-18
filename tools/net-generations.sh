#!/bin/sh
# The training loop for the network (milestone 15), one generation at a time:
#   net N-1 plays itself  ->  nnue-train continues net N-1 on the recent data  ->  net N  ->  matches
#
# Usage: tools/net-generations.sh RUN_DIR GENERATIONS
# Settings (environment, defaults in brackets):
#   NET0 [runs/nnue-offline/h32-s10-e60.bin] starting network (generation 0)
#   WEIGHTS [runs/nnue-offline/linear-all.bin] linear weights: move ordering, quiet-leaf resolution, and the
#            anchor every generation is measured against (natively, without a network)
#   GAMES [2000] self-play games per generation   NODES [1500] nodes per move in self-play
#   WINDOW [4] generations of data the network trains on   EPOCHS [4] per generation
#   SMOOTH [10] quiet-move penalty (nnue-train --smooth)   LR [0.001] network learning rate
#   LAMBDA [0] weight of the self-play search score in the target (nnue-train --lambda)
#   MATCH_GAMES [400] games against the previous generation, MATCH_NODES [1500]
#   ANCHOR_GAMES [1000] games against WEIGHTS, ANCHOR_NODES [5000]
#   SEARCH [exact] self-play search options; matches use the engine's default search
#   THREADS [all cores]   ENGINE, TRAIN: binaries
#
# Writes RUN_DIR/generations.csv; logs of every step go to RUN_DIR/logs. Running it again skips
# steps whose log is complete; once a step had to run, every later step runs again too. It refuses
# to continue with other settings than the run was started with (THREADS may change).
#
# Cost = CPU seconds of self-play and training; matches only measure. Elo vs the anchor is at equal
# nodes, which flatters the network (it is ~1.3x slower); check the end result at equal time.
set -eu
export LC_ALL=C

if [ $# -ne 2 ]; then
    sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
run=$1
gens=$2
NET0=${NET0:-runs/nnue-offline/h32-s10-e60.bin}
WEIGHTS=${WEIGHTS:-runs/nnue-offline/linear-all.bin}
GAMES=${GAMES:-2000}
NODES=${NODES:-1500}
WINDOW=${WINDOW:-4}
EPOCHS=${EPOCHS:-4}
SMOOTH=${SMOOTH:-10}
LR=${LR:-0.001}
LAMBDA=${LAMBDA:-0}
MATCH_GAMES=${MATCH_GAMES:-400}
MATCH_NODES=${MATCH_NODES:-1500}
ANCHOR_GAMES=${ANCHOR_GAMES:-1000}
ANCHOR_NODES=${ANCHOR_NODES:-5000}
SEARCH=${SEARCH:-exact}
THREADS=${THREADS:-$(nproc)}
ENGINE=${ENGINE:-build/release/engine}
TRAIN=${TRAIN:-build/release/nnue-train}

mkdir -p "$run/data" "$run/nets" "$run/logs"
csv="$run/generations.csv"

fingerprint=$("$ENGINE" bench 7 "$SEARCH" | sed -n 's/^\([0-9]*\) nodes.*/\1/p')
hidden=$(od -An -tu4 -j12 -N4 "$NET0" | tr -d ' ')
net0_sum=$(cksum <"$NET0" | cut -d' ' -f1)
weights_sum=$(cksum <"$WEIGHTS" | cut -d' ' -f1)
settings="net0=$net0_sum weights=$weights_sum games=$GAMES nodes=$NODES window=$WINDOW epochs=$EPOCHS"
settings="$settings smooth=$SMOOTH lr=$LR match=$MATCH_GAMES/$MATCH_NODES anchor=$ANCHOR_GAMES/$ANCHOR_NODES"
settings="$settings search=$SEARCH engine=$fingerprint"
[ "$LAMBDA" = 0 ] || settings="$settings lambda=$LAMBDA" # only when used, so older runs keep resuming
if [ -f "$run/settings.txt" ]; then
    if [ "$(cat "$run/settings.txt")" != "$settings" ]; then
        echo "$run was started with: $(cat "$run/settings.txt")" >&2
        echo "now asked for:  $settings" >&2
        echo "continue with the same settings, or use a new RUN_DIR" >&2
        exit 1
    fi
else
    echo "$settings" >"$run/settings.txt"
fi
cp "$NET0" "$run/nets/gen0.bin"

kv() {
    sed -n "s/^summary .* $2=\([^ ]*\).*/\1/p; s/^summary $2=\([^ ]*\).*/\1/p" "$1"
}

redo=0
step() { # step LOG COMMAND...: runs COMMAND into LOG unless LOG is complete and nothing before had to run
    log=$1
    shift
    if [ "$redo" -eq 0 ] && [ -f "$log" ] && grep -q '^summary ' "$log"; then
        return
    fi
    redo=1
    echo "  $(basename "$log" .txt)" >&2
    "$@" >"$log.part" 2>"$log.err"
    mv "$log.part" "$log"
}

net() { echo "$run/nets/gen$1.bin"; }

anchor() { # anchor LOG GEN
    step "$1" "$ENGINE" match --a "$WEIGHTS" --a-net "$(net "$2")" --b "$WEIGHTS" --games "$ANCHOR_GAMES" \
        --nodes "$ANCHOR_NODES" --threads "$THREADS" --seed 47
}

echo "gen,games,positions,selfplay_cpu_s,train_cpu_s,cum_cpu_s,val_loss,quiet_change,elo_vs_prev,err_vs_prev,elo_vs_anchor,err_vs_anchor" >"$csv"
anchor "$run/logs/anchor0.txt" 0
echo "0,0,0,0,0,0,,,0,0,$(kv "$run/logs/anchor0.txt" elo),$(kv "$run/logs/anchor0.txt" elo_err)" >>"$csv"

cum=0
for g in $(seq 1 "$gens"); do
    p=$((g - 1))
    echo "generation $g" >&2

    selfplay_log="$run/logs/selfplay$p.txt"
    step "$selfplay_log" "$ENGINE" selfplay --games "$GAMES" --nodes "$NODES" --threads "$THREADS" \
        --seed $((2000 + p)) --search "$SEARCH" --weights "$WEIGHTS" --net "$(net "$p")" --out "$run/data/gen$p.bin"

    # Continue the previous network on the last WINDOW generations of data.
    data=""
    first=$((p - WINDOW + 1))
    [ "$first" -ge 0 ] || first=0
    for d in $(seq "$first" "$p"); do data="$data --data $run/data/gen$d.bin"; done
    train_log="$run/logs/train$g.txt"
    # shellcheck disable=SC2086
    step "$train_log" "$TRAIN" $data --resolve "$WEIGHTS" --init-net "$(net "$p")" --hidden "$hidden" \
        --epochs "$EPOCHS" --lr "$LR" --smooth "$SMOOTH" --lambda "$LAMBDA" --threads "$THREADS" --out "$(net "$g")"

    prev_log="$run/logs/match$g-vs-$p.txt"
    step "$prev_log" "$ENGINE" match --a "$WEIGHTS" --a-net "$(net "$g")" --b "$WEIGHTS" --b-net "$(net "$p")" \
        --games "$MATCH_GAMES" --nodes "$MATCH_NODES" --threads "$THREADS" --seed 7
    anchor_log="$run/logs/anchor$g.txt"
    anchor "$anchor_log" "$g"

    selfplay_cpu=$(kv "$selfplay_log" cpu_s)
    train_cpu=$(kv "$train_log" cpu_s)
    cum=$(awk "BEGIN { printf \"%.3f\", $cum + $selfplay_cpu + $train_cpu }")
    echo "$g,$GAMES,$(kv "$selfplay_log" positions),$selfplay_cpu,$train_cpu,$cum,$(kv "$train_log" val),$(kv "$train_log" quiet_change),$(kv "$prev_log" elo),$(kv "$prev_log" elo_err),$(kv "$anchor_log" elo),$(kv "$anchor_log" elo_err)" >>"$csv"
    tail -1 "$csv" >&2
done
