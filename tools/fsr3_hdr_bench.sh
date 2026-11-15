#!/bin/sh
# 14 Nov: FSR 3 speed with the upscaler correctly in HDR mode (10:10:10 off under temporal upscaling, by setting).
cd "$(dirname "$0")/.."
B="REGPU_HYPER=potato FM2_ANISOTROPY=16"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0"
F1="FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900"
for mode in 0 1; do export BENCH_RACENOW="drive=$mode"
  for r in 1 2 3; do
    sh tools/hyper_bench.sh fh$mode-pfsr3-$r 30 $B $T >/dev/null
    sh tools/hyper_bench.sh fh$mode-maxfsr3-$r 30 $B $T REGPU_HYPER_FREEZE_SHADOWS=1 >/dev/null
    sh tools/hyper_bench.sh fh$mode-pfsr1-$r 30 $B $F1 >/dev/null
  done
done
echo done
