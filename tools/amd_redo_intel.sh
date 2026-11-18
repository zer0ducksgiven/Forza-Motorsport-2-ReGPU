#!/bin/sh
# 15 Nov, AMD laptop: the Intel box's benchmarks redone here with the same settings, in order:
#  1. tools/iq_bench.sh   (labels iq-*)  every speed-up alone vs native, stacks, presets at native
#  2. tools/iq_bench2.sh  (iq2-*)        the Intel Turbo candidates
#  3. ax-*: native, laptop Potato, Intel Potato set (forced by env; hyper.cpp only applies it on vendor 8086), 3 rounds
#  4. tools/preset_bench.sh (preset-*)   the conclusion benchmark
#  5. FSR at 1600x900 with the Intel set: p720 / FSR 1 / FSR 3 / full stack + FSR 3, parked and driving (ah0-*, ah1-*)
cd "$(dirname "$0")/.."
. tools/amd_intel_set.sh
sh tools/iq_bench.sh
sh tools/iq_bench2.sh
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16 FM2_DUMP_AT=25"
sh tools/hyper_bench.sh aw-ipotato 20 $B $IP >/dev/null
for r in 1 2 3; do
  sh tools/hyper_bench.sh ax-native-$r 30 $B >/dev/null
  sh tools/hyper_bench.sh ax-lpotato-$r 30 $B REGPU_HYPER=potato >/dev/null
  sh tools/hyper_bench.sh ax-ipotato-$r 30 $B $IP >/dev/null
  sh tools/hyper_bench.sh ax-native2-$r 30 $B >/dev/null
done
sh tools/preset_bench.sh
A="FM2_ANISOTROPY=16"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0"
F1="FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900"
P7="FM2_UPSCALER=off FM2_RESOLUTION=1280x720"
BENCH_RACENOW="drive=1" sh tools/hyper_bench.sh aw-fsr3 300 $IP $A $T REGPU_HYPER_FREEZE_SHADOWS=1 >/dev/null
BENCH_RACENOW="drive=1" sh tools/hyper_bench.sh aw-fsr1 60 $IP $A $F1 >/dev/null
for mode in 0 1; do export BENCH_RACENOW="drive=$mode"
  for r in 1 2 3; do
    sh tools/hyper_bench.sh ah$mode-stock-$r 30 REGPU_HYPER=off $A $P7 >/dev/null
    sh tools/hyper_bench.sh ah$mode-p720-$r 30 $IP $A $P7 >/dev/null
    sh tools/hyper_bench.sh ah$mode-pfsr1-$r 30 $IP $A $F1 >/dev/null
    sh tools/hyper_bench.sh ah$mode-pfsr3-$r 30 $IP $A $T >/dev/null
    sh tools/hyper_bench.sh ah$mode-maxfsr3-$r 30 $IP $A $T REGPU_HYPER_FREEZE_SHADOWS=1 >/dev/null
  done
done
echo ALLDONE
