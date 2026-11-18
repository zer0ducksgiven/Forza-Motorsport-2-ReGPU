#!/bin/sh
# Deck benchmark (9 Nov): every Hyper speed-up alone against native 720p, plus the stacks and every preset.
# 3 rounds; each round runs native twice (start and middle) so drift is visible. Parked, GPU stats on, a
# screenshot at 25 s per run. Results: out/bench.txt lines labelled dq-<name> (16x anisotropic: ReGPU default)-<round>.
cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="FM2_UPSCALER=off FM2_ANISOTROPY=16 FM2_DUMP_AT=25"
run() { name=$1; shift; sh tools/hyper_bench.sh "dq-$name-$r" 30 $B "$@"; }
# Presets that lower the render scale need their upscaler, so they run without FM2_UPSCALER=off.
runp() { name=$1; shift; sh tools/hyper_bench.sh "dq-$name-$r" 30 FM2_ANISOTROPY=16 FM2_DUMP_AT=25 "$@"; }
for r in 1 2 3; do
    run native
    run aniso2   REGPU_HYPER_ANISO=2
    run aniso0   REGPU_HYPER_ANISO=0
    run help50   REGPU_HYPER_HELPERS=50
    run help25   REGPU_HYPER_HELPERS=25
    run r1010    REGPU_HYPER_RESOLVE1010=1
    run dec10    REGPU_HYPER_DECIMATE=10
    run cull     REGPU_HYPER_CULL_PIXELS=64 REGPU_HYPER_CULL_ALL=1
    run native2
    run a2m      REGPU_HYPER_A2M_TEST=1
    run freeze   REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3
    run lean3    REGPU_HYPER_LEAN_FETCH=3
    run leanv    REGPU_HYPER_LEAN_VFETCH=1
    run potato   REGPU_HYPER=potato
    run everything REGPU_HYPER=potato REGPU_HYPER_A2M_TEST=1 REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3
    runp mild     REGPU_HYPER=mild
    runp low      REGPU_HYPER=low
    runp extreme  REGPU_HYPER=extreme
done
