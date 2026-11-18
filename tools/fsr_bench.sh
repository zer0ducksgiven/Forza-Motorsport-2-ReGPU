#!/bin/sh
# FSR study (11 Nov): stock ReGPU (no Hyper), 720p native vs real 900p vs 720p upscaled to 1600x900 by FSR 1, FSR 2.3.4
# and FSR 3.1.5. Anisotropy 16x forced. A warm-up run per temporal config first (motion-vector shaders compile).
# 3 interleaved rounds, parked race, 30 s, screenshot 60 frames after the race view is up. Labels fs-<name>-<round>.
cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="REGPU_HYPER=off FM2_ANISOTROPY=16 FM2_DUMP_AFTER_RACE=60"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80"
cfg() {
  case $1 in
    native720) echo "FM2_UPSCALER=off FM2_RESOLUTION=1280x720";;
    native900) echo "FM2_UPSCALER=off FM2_RESOLUTION=1600x900";;
    fsr1)      echo "FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900";;
    fsr2)      echo "FM2_UPSCALER=fsr3 FM2_FSR_VERSION=1 $T";;
    fsr3)      echo "FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0 $T";;
  esac
}
for n in fsr1 fsr2 fsr3; do sh tools/hyper_bench.sh fw-$n 90 $B $(cfg $n) >/dev/null; done
for r in 1 2 3; do
  for n in native720 fsr1 fsr2 fsr3 native900; do sh tools/hyper_bench.sh fs-$n-$r 30 $B $(cfg $n) >/dev/null; done
done
echo done
