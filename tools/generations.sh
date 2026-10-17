#!/bin/sh
# The training loop, one generation at a time:
#   gen N-1 plays itself  ->  tuner learns gen N from all data so far  ->  matches
#
# Usage: tools/generations.sh RUN_DIR GENERATIONS
# Settings (environment, defaults in brackets):
#   GAMES [2000] self-play games per generation    NODES [1500] nodes per move (most strength per CPU, M7)
#   MATCH_GAMES [400] games per match              EPOCHS [400] tuner epochs
#   SEARCH [exact] search options for self-play and matches (see engine match)
#   LAMBDA [0] weight of self-play search scores in the tuner's target (data v2)
#   RESIGN [off] calibrated resignation in self-play (on/off)
#   TUNER_FAST [on] cheaper tuning: stop after 30 epochs without improvement, and resolve
#                   each data file's quiet leaves only once (cached next to it); -37% CPU at the
#                   same strength (M11). Runs started without it [off] continue without it.
#   THREADS [all cores]                            ENGINE, TUNER: binaries
#
# Writes RUN_DIR/generations.csv; logs of every step go to RUN_DIR/logs.
# Running it again skips steps whose log is complete, so an interrupted run
# continues where it stopped. It refuses to continue with other GAMES, NODES,
# MATCH_GAMES, EPOCHS, SEARCH, LAMBDA, RESIGN, TUNER_FAST or engine (search
# fingerprint) than the run was started with (THREADS may change: the results
# do not depend on it). Once a step had to run, every later step runs
# again too, so a redone step never mixes with stale results after it.
#
# val_loss is measured on a validation set that grows with the data (every
# 10th game of all generations so far), so it is not comparable across rows.
# A match won or lost in every game is marked in the notes column: its Elo is
# only a bound.
#
# Cost (x-axis of the curve) = CPU seconds of self-play and tuning; matches
# only measure and are not counted. Elo is measured against the previous
# generation and chained; Elo against gen 0 saturates quickly.
set -eu
export LC_ALL=C # awk must write 0.5, not 0,5

if [ $# -ne 2 ]; then
    sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
run=$1
gens=$2
GAMES=${GAMES:-2000}
NODES=${NODES:-1500}
MATCH_GAMES=${MATCH_GAMES:-400}
EPOCHS=${EPOCHS:-400}
SEARCH=${SEARCH:-exact}
LAMBDA=${LAMBDA:-0}
RESIGN=${RESIGN:-off}
if [ -z "${TUNER_FAST+set}" ] && [ -f "$run/settings.txt" ] && ! grep -q 'tuner_fast=' "$run/settings.txt"; then
    TUNER_FAST=off # the default before M11: such runs continue as they started
fi
TUNER_FAST=${TUNER_FAST:-on}
case "$TUNER_FAST" in
    on) tune_extra="--patience 30 --leaf-cache on" ;;
    off) tune_extra="" ;;
    *) echo "TUNER_FAST must be on or off" >&2; exit 2 ;;
esac
THREADS=${THREADS:-$(nproc)}
ENGINE=${ENGINE:-build/release/engine}
TUNER=${TUNER:-build/release/tuner}

mkdir -p "$run/data" "$run/weights" "$run/logs"
csv="$run/generations.csv"

# The engine's search fingerprint (bench node count) ties a run to the engine behavior that
# produced it: a run cannot be continued by an engine that searches differently.
fingerprint=$("$ENGINE" bench 7 "$SEARCH" | sed -n 's/^\([0-9]*\) nodes.*/\1/p')
settings="games=$GAMES nodes=$NODES match_games=$MATCH_GAMES epochs=$EPOCHS search=$SEARCH engine=$fingerprint"
# Newer settings are only written when used, so older runs keep resuming.
[ "$LAMBDA" = 0 ] || settings="$settings lambda=$LAMBDA"
[ "$RESIGN" = off ] || settings="$settings resign=$RESIGN"
[ "$TUNER_FAST" = off ] || settings="$settings tuner_fast=$TUNER_FAST"
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

# Value of key in a log's "summary" line.
kv() {
    sed -n "s/^summary .* $2=\([^ ]*\).*/\1/p; s/^summary $2=\([^ ]*\).*/\1/p" "$1"
}

# step LOG COMMAND...: runs COMMAND into LOG, unless LOG already has a summary
# and no earlier step had to run.
redo=0
step() {
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

weights() {
    if [ "$1" -eq 0 ]; then echo zero; else echo "$run/weights/gen$1.bin"; fi
}

match() { # match LOG A B
    step "$1" "$ENGINE" match --a "$2" --b "$3" --games "$MATCH_GAMES" --nodes "$NODES" \
        --a-search "$SEARCH" --b-search "$SEARCH" --threads "$THREADS" --seed 7
}

echo "gen,games,positions,selfplay_cpu_s,tune_cpu_s,cum_cpu_s,val_loss,elo_vs_prev,err_vs_prev,elo_chain,err_chain,elo_vs_gen0,err_vs_gen0,pieces,notes" >"$csv"
echo "0,0,0,0,0,0,,0,0,0,0,0,0,," >>"$csv"

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
            --seed $((1000 + p)) --search "$SEARCH" --resign "$RESIGN" --out "$run/data/gen$p.bin"
    else
        step "$selfplay_log" "$ENGINE" selfplay --games "$GAMES" --nodes "$NODES" --threads "$THREADS" \
            --seed $((1000 + p)) --search "$SEARCH" --resign "$RESIGN" --out "$run/data/gen$p.bin" --weights "$(weights "$p")"
    fi
    data="$data --data $run/data/gen$p.bin"

    # Tune on all data so far, starting from (and resolving with) the previous weights.
    tune_log="$run/logs/tune$g.txt"
    if [ "$p" -eq 0 ]; then
        # shellcheck disable=SC2086
        step "$tune_log" "$TUNER" $data --out "$(weights "$g")" --threads "$THREADS" \
            --epochs "$EPOCHS" --lambda "$LAMBDA" --report 0 $tune_extra
    else
        # shellcheck disable=SC2086
        step "$tune_log" "$TUNER" $data --init "$(weights "$p")" --out "$(weights "$g")" \
            --threads "$THREADS" --epochs "$EPOCHS" --lambda "$LAMBDA" --report 0 $tune_extra
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
    cum=$(awk "BEGIN { printf \"%.3f\", $cum + $selfplay_cpu + $tune_cpu }")
    chain=$(awk "BEGIN { printf \"%.1f\", $chain + $elo_prev }")
    chain_var=$(awk "BEGIN { printf \"%.3f\", $chain_var + $err_prev * $err_prev }")
    pieces=$(sed -n 's/^pieces //p' "$tune_log")
    notes=""
    [ "$(kv "$prev_log" saturated)" = 1 ] && notes="vs prev one-sided"
    [ "$p" -ne 0 ] && [ "$(kv "$gen0_log" saturated)" = 1 ] && notes="${notes:+$notes; }vs gen0 one-sided"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s","%s"\n' "$g" "$(kv "$selfplay_log" games)" \
        "$(kv "$selfplay_log" positions)" "$selfplay_cpu" "$tune_cpu" "$cum" "$(kv "$tune_log" val_loss)" \
        "$elo_prev" "$err_prev" "$chain" "$(awk "BEGIN { printf \"%.1f\", sqrt($chain_var) }")" \
        "$(kv "$gen0_log" elo)" "$(kv "$gen0_log" elo_err)" "$pieces" "$notes" >>"$csv"
    tail -n 1 "$csv" >&2
done
