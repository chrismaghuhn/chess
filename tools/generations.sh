#!/bin/sh
# The training loop, one generation at a time:
#   gen N-1 plays itself  ->  tuner learns gen N from all data so far  ->  matches
#
# Usage: tools/generations.sh RUN_DIR GENERATIONS
# Settings (environment, defaults in brackets):
#   GAMES [2000] self-play games per generation    NODES [5000] nodes per move
#   MATCH_GAMES [400] games per match              EPOCHS [400] tuner epochs
#   THREADS [all cores]                            ENGINE, TUNER: binaries
#
# Writes RUN_DIR/generations.csv; logs of every step go to RUN_DIR/logs.
# Running it again skips steps whose log is complete, so an interrupted run
# continues where it stopped (with the settings it was started with).
#
# Cost (x-axis of the curve) = CPU seconds of self-play and tuning; matches
# only measure and are not counted. Elo is measured against the previous
# generation and chained; Elo against gen 0 saturates quickly.
set -eu
export LC_ALL=C # awk must write 0.5, not 0,5

if [ $# -ne 2 ]; then
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
run=$1
gens=$2
GAMES=${GAMES:-2000}
NODES=${NODES:-5000}
MATCH_GAMES=${MATCH_GAMES:-400}
EPOCHS=${EPOCHS:-400}
THREADS=${THREADS:-$(nproc)}
ENGINE=${ENGINE:-build/release/engine}
TUNER=${TUNER:-build/release/tuner}

mkdir -p "$run/data" "$run/weights" "$run/logs"
csv="$run/generations.csv"

# Value of key in a log's "summary" line.
kv() {
    sed -n "s/^summary .* $2=\([^ ]*\).*/\1/p; s/^summary $2=\([^ ]*\).*/\1/p" "$1"
}

# step LOG COMMAND...: runs COMMAND into LOG, unless LOG already has a summary.
step() {
    log=$1
    shift
    if [ -f "$log" ] && grep -q '^summary ' "$log"; then
        return
    fi
    echo "  $(basename "$log" .txt)" >&2
    "$@" >"$log.part" 2>"$log.err"
    mv "$log.part" "$log"
}

weights() {
    if [ "$1" -eq 0 ]; then echo zero; else echo "$run/weights/gen$1.bin"; fi
}

match() { # match LOG A B
    step "$1" "$ENGINE" match --a "$2" --b "$3" --games "$MATCH_GAMES" --nodes "$NODES" \
        --threads "$THREADS" --seed 7
}

echo "gen,games,positions,selfplay_cpu_s,tune_cpu_s,cum_cpu_s,val_loss,elo_vs_prev,err_vs_prev,elo_chain,err_chain,elo_vs_gen0,err_vs_gen0,pieces" >"$csv"
echo "0,0,0,0,0,0,,0,0,0,0,0,0," >>"$csv"

cum=0
chain=0
chain_var=0
data=""
for g in $(seq 1 "$gens"); do
    p=$((g - 1))
    echo "generation $g" >&2

    # Self-play with the previous generation.
    selfplay_log="$run/logs/selfplay$p.txt"
    if [ "$p" -eq 0 ]; then
        step "$selfplay_log" "$ENGINE" selfplay --games "$GAMES" --nodes "$NODES" --threads "$THREADS" \
            --seed $((1000 + p)) --out "$run/data/gen$p.bin"
    else
        step "$selfplay_log" "$ENGINE" selfplay --games "$GAMES" --nodes "$NODES" --threads "$THREADS" \
            --seed $((1000 + p)) --out "$run/data/gen$p.bin" --weights "$(weights "$p")"
    fi
    data="$data --data $run/data/gen$p.bin"

    # Tune on all data so far, starting from (and resolving with) the previous weights.
    tune_log="$run/logs/tune$g.txt"
    if [ "$p" -eq 0 ]; then
        # shellcheck disable=SC2086
        step "$tune_log" "$TUNER" $data --out "$(weights "$g")" --threads "$THREADS" \
            --epochs "$EPOCHS" --report 0
    else
        # shellcheck disable=SC2086
        step "$tune_log" "$TUNER" $data --init "$(weights "$p")" --out "$(weights "$g")" \
            --threads "$THREADS" --epochs "$EPOCHS" --report 0
    fi

    # Measure.
    prev_log="$run/logs/match$g-vs-$p.txt"
    match "$prev_log" "$(weights "$g")" "$(weights "$p")"
    gen0_log="$prev_log"
    if [ "$p" -ne 0 ]; then
        gen0_log="$run/logs/match$g-vs-0.txt"
        match "$gen0_log" "$(weights "$g")" zero
    fi

    selfplay_cpu=$(kv "$selfplay_log" cpu_s)
    tune_cpu=$(kv "$tune_log" cpu_s)
    elo_prev=$(kv "$prev_log" elo)
    err_prev=$(kv "$prev_log" elo_err)
    cum=$(awk "BEGIN { print $cum + $selfplay_cpu + $tune_cpu }")
    chain=$(awk "BEGIN { print $chain + $elo_prev }")
    chain_var=$(awk "BEGIN { print $chain_var + $err_prev * $err_prev }")
    pieces=$(sed -n 's/^pieces //p' "$tune_log")

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' "$g" "$(kv "$selfplay_log" games)" \
        "$(kv "$selfplay_log" positions)" "$selfplay_cpu" "$tune_cpu" "$cum" "$(kv "$tune_log" val_loss)" \
        "$elo_prev" "$err_prev" "$chain" "$(awk "BEGIN { printf \"%.1f\", sqrt($chain_var) }")" \
        "$(kv "$gen0_log" elo)" "$(kv "$gen0_log" elo_err)" "$pieces" >>"$csv"
    tail -n 1 "$csv" >&2
done
