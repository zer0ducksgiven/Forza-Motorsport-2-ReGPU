#!/bin/sh
# Deck benchmark, part 2 (9 Nov): the near-lossless "Turbo" stacks and the two lossless pieces not measured alone in
# deck_bench.sh, each against native 720p. Waits until deck_bench.sh has finished (its last run, dq-extreme-3).
cd "$(dirname "$0")/.."
until grep -q "^dq-extreme-3 " out/bench.txt 2>/dev/null || grep -q "dq-extreme-3: gave up" out/bench_failures.txt 2>/dev/null; do sleep 30; done
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="FM2_UPSCALER=off FM2_ANISOTROPY=16 FM2_DUMP_AT=25"
run() { name=$1; shift; sh tools/hyper_bench.sh "tq-$name-$r" 30 $B "$@"; }
STRICT="REGPU_HYPER_LEAN_FETCH=2 REGPU_HYPER_LEAN_VFETCH=1 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_CULL_PIXELS=0 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_A2M_TEST=1"
for r in 1 2 3; do
    run native
    run lean2   REGPU_HYPER_LEAN_FETCH=2
    run cull0   REGPU_HYPER_CULL_PIXELS=0 REGPU_HYPER_CULL_ALL=1
    run strict  $STRICT
    run turbo   $STRICT REGPU_HYPER_LEAN_FETCH=3
done
