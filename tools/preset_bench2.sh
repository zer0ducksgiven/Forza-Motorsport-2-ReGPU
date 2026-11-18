#!/bin/sh
# Built-in presets of the Hyper Settings window as it starts them (16 Nov 2026): stock, Lossless, Turbo, Potato.
# One warm-up run each (shader variants), then 3 interleaved rounds, parked, 30 s, 720p native. Labels pb-<preset>-<n>.
cd "$(dirname "$0")/.."
B="FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16 REGPU_HYPER=off REGPU_HYPER_SCENE=0"
H=REGPU_HYPER
LOSSLESS="${H}_HELPERS=100 ${H}_ANISO=16 ${H}_RESOLVE1010=1 ${H}_RESOLVE_BY_SIZE=1 ${H}_NODISCARD=1 ${H}_SHADER_OPT=1 ${H}_PLAIN_MUL=5 ${H}_FETCHSPEC=1 ${H}_TAILSPEC=1 ${H}_SWIZZLE_SEL=1 ${H}_LEAN_LOD=1"
CUTS="${H}_HELPERS=25 ${H}_ANISO=0 ${H}_RESOLVE1010=1 ${H}_RESOLVE_BY_SIZE=1 ${H}_FREEZE=120 ${H}_FREEZE_REFRESH=3 ${H}_CULL_ALL=1 ${H}_CULL_PASSES=1 ${H}_CULL_PROBE=1 ${H}_CULL_HYST=200 ${H}_CULL_PIN=1 ${H}_DECIMATE=10 ${H}_VCACHE=2 ${H}_NODISCARD=1 ${H}_SHADER_OPT=1 ${H}_PLAIN_MUL=5 ${H}_FETCHSPEC=1 ${H}_TAILSPEC=1 ${H}_SWIZZLE_SEL=1 ${H}_LEAN_LOD=1"
TURBO="$CUTS ${H}_CULL_PIXELS=0 ${H}_LEAN_VFETCH=1 ${H}_LEAN_FETCH=3"
POTATO="$CUTS ${H}_CULL_PIXELS=128 ${H}_LEAN_VFETCH=0 ${H}_LEAN_FETCH=0"
run() { BENCH_RACENOW="drive=0" BENCH_PERF=1 sh tools/hyper_bench.sh "$@" > /dev/null 2>&1 || echo "$1 failed" >> out/bench_failures.txt; }
for p in lossless turbo potato; do
    case $p in lossless) V=$LOSSLESS;; turbo) V=$TURBO;; potato) V=$POTATO;; esac
    run pbw-$p 60 $B $V
done
for r in 1 2 3; do
    run pb-stock-$r 30 $B
    run pb-lossless-$r 30 $B $LOSSLESS
    run pb-turbo-$r 30 $B $TURBO
    run pb-potato-$r 30 $B $POTATO
done
echo PBDONE
