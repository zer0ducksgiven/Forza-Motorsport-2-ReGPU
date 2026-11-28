#!/bin/bash
# Linux / Steam Deck (Proton): the ReGPU-V build's benchmark (src/regpu_host/bench.cpp): boots into an Exhibition race with the AI driving the player's car
# (--racenow=race=N,ai=1), freezes the start grid, screenshots it (pixel comparisons: tools/regpu/pixdiff.py), measures
# the frozen grid, lets the race go on and measures it, then the game closes itself.
#   tools/regpu/bench.sh <label> [race seconds=60] [VAR=value ...]
# Defaults (run.sh): 1280x720, MSAA 2x, uncapped. FM2_BENCH_RACE=N picks the race (0, Nissan Speedway), FM2_BENCH_GRID
# the frozen-grid seconds (15). Results: out/bench/<label>-<time>/ (fm2.log, grid.png, result.txt) and a line in
# out/bench/runs.txt.
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
LABEL="$1"; SECS="${2:-60}"; shift; [ $# -gt 0 ] && shift
CTL="${REGPU_CTL:-$ROOT/thirdparty/ReGPU/tools/regpuctl.py}"
DIR="$ROOT/out/bench/$LABEL-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$DIR" "$ROOT/out/run"
LOG="$ROOT/out/run/fm2.log"
winpath() { printf 'Z:%s' "$(readlink -f "$1" | tr '/' '\\')"; }
( env FM2_AI_BENCH="$SECS" "$@" "$ROOT/tools/regpu/run.sh" --racenow=race=${FM2_BENCH_RACE:-0},ai=1 ) </dev/null >"$ROOT/out/run/proton.out" 2>&1 &
sleep 3
declare -A shots
for i in $(seq 1 $((SECS * 2 + 900))); do
  for name in $(grep -oE "bench: shot [A-Za-z0-9]+" "$LOG" 2>/dev/null | awk '{print $3}'); do
    if [ -z "${shots[$name]:-}" ]; then
      REGPU_DEBUG_PORT=7343 timeout 30 python3 -I "$CTL" shot "$(winpath "$DIR")\\$name.png" >/dev/null 2>&1; shots[$name]=1
    fi
  done
  grep -q "bench: done" "$LOG" 2>/dev/null && ! pgrep -x fm2.exe >/dev/null && break
  pgrep -x fm2.exe >/dev/null || { [ $i -gt 20 ] && break; }
  sleep 0.5
done
for i in $(seq 1 60); do pgrep -x fm2.exe >/dev/null || break; sleep 1; done
pkill -x fm2.exe 2>/dev/null && sleep 2
cp "$LOG" "$DIR/"
RES=$(grep -E "bench: (grid|race|ab .*) frames" "$LOG" | sed -E 's/^\[ *[0-9.]+\] bench: //' | sed -E 's/frames [0-9]+ in [0-9.]+ s, //; s/stutters: //')
echo "$RES" > "$DIR/result.txt"
{ echo "$(date +%H:%M) $LABEL ($*):"; echo "$RES" | sed 's/^/    /'; } | tee -a "$ROOT/out/bench/runs.txt"
grep -qE "CRASH|FATAL" "$LOG" && echo "    !! crash/fatal in the log" | tee -a "$ROOT/out/bench/runs.txt"
