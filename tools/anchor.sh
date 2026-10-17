#!/bin/sh
# Measures every generation of a run against one fixed anchor (weights file), so that runs with
# different settings land on one Elo scale. Writes RUN_DIR/anchor.csv with the run's cumulative
# training CPU next to the Elo against the anchor; tools/plot.py can draw it.
#
# Usage: tools/anchor.sh RUN_DIR ANCHOR_WEIGHTS
# Settings (environment): NODES [5000] nodes per move for both sides, GAMES [1000] per match,
#   SEARCH [exact], THREADS [all cores], ENGINE
# Matches already measured (logs in RUN_DIR/anchor/) are not played again.
set -eu
export LC_ALL=C

if [ $# -ne 2 ]; then
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
run=$1
anchor=$2
NODES=${NODES:-5000}
GAMES=${GAMES:-1000}
SEARCH=${SEARCH:-exact}
THREADS=${THREADS:-$(nproc)}
ENGINE=${ENGINE:-build/release/engine}

mkdir -p "$run/anchor"
out="$run/anchor.csv"
echo "gen,cum_cpu_s,elo_chain,err_chain" >"$out"

kv() {
    sed -n "s/^summary .* $2=\([^ ]*\).*/\1/p; s/^summary $2=\([^ ]*\).*/\1/p" "$1"
}

tail -n +3 "$run/generations.csv" | while IFS=, read -r gen _ _ _ _ cum _; do
    log="$run/anchor/gen$gen-nodes$NODES-games$GAMES.txt"
    if ! [ -f "$log" ] || ! grep -q '^summary ' "$log"; then
        echo "  gen $gen" >&2
        "$ENGINE" match --a "$run/weights/gen$gen.bin" --b "$anchor" --a-search "$SEARCH" --b-search "$SEARCH" \
            --nodes "$NODES" --games "$GAMES" --threads "$THREADS" --seed 47 >"$log.part" 2>/dev/null
        mv "$log.part" "$log"
    fi
    echo "$gen,$cum,$(kv "$log" elo),$(kv "$log" elo_err)" >>"$out"
done
cat "$out"
