#!/bin/sh
# Benchmark consistency (16 Nov 2026): the game's own AI driving (racenow ai=1) against our vision bot (drive=1),
# 3 interleaved runs each, Potato, 720p native, 40 s from the race start. Labels aic-ai-<n>, aic-bot-<n>.
cd "$(dirname "$0")/.."
B="FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16 REGPU_HYPER=potato"
for r in 1 2 3; do
    BENCH_RACENOW="drive=0,ai=1" BENCH_PERF=1 sh tools/hyper_bench.sh aic-ai-$r 40 $B > /dev/null 2>&1
    BENCH_RACENOW="drive=1" BENCH_PERF=1 sh tools/hyper_bench.sh aic-bot-$r 40 $B > /dev/null 2>&1
done
echo AICDONE
