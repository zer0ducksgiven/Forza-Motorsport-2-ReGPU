#!/bin/sh
# Phase 7 check: stock vs the explicit Intel stack vs the one-switch presets (potato, auto), two runs each, interleaved.
cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
STACK="FM2_UPSCALER=off REGPU_HYPER_ANISO=0 REGPU_HYPER_HELPERS=25 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_DECIMATE=10 REGPU_HYPER_NODISCARD=1 REGPU_HYPER_SHADER_OPT=1 REGPU_HYPER_VCACHE=2 REGPU_HYPER_PLAIN_MUL=5 REGPU_HYPER_FETCHSPEC=1 REGPU_HYPER_SWIZZLE_SEL=1 REGPU_HYPER_CULL_PIXELS=64 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3 REGPU_HYPER_LEAN_LOD=1"
sh tools/hyper_bench.sh p7-warm 30 REGPU_HYPER=potato >/dev/null
for i in 1 2; do
  sh tools/hyper_bench.sh p7-stock-$i 30 FM2_UPSCALER=off
  sh tools/hyper_bench.sh p7-stack-$i 30 $STACK
  sh tools/hyper_bench.sh p7-potato-$i 30 REGPU_HYPER=potato
  sh tools/hyper_bench.sh p7-auto-$i 30 REGPU_HYPER=auto
done
grep -h "hyper:" out/bench_p7-auto-1.err out/bench_p7-potato-1.err | head -12
