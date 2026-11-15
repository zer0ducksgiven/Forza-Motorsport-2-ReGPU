#!/bin/sh
# Potato + FSR while driving (13 Nov). fps: auto-drive (racenow drive=1), 3 interleaved rounds, 30 s.
# Clips: straight-line drive, uncapped, 180 consecutive frames from 400 frames after the race view (past the start ghost).
cd "$(dirname "$0")/.."
B="REGPU_HYPER=potato FM2_ANISOTROPY=16"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3"
cfg() { case $1 in
  p720) echo "FM2_UPSCALER=off FM2_RESOLUTION=1280x720";;
  pfsr1) echo "FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900";;
  pfsr2) echo "$T FM2_FSR_VERSION=1";;
  pfsr3) echo "$T FM2_FSR_VERSION=0";;
esac; }
export BENCH_RACENOW="drive=1" BENCH_PERF=1
for r in 1 2 3; do for n in p720 pfsr1 pfsr2 pfsr3; do sh tools/hyper_bench.sh fpd-$n-$r 30 $B $(cfg $n) >/dev/null; done; done
for n in p720 pfsr1 pfsr2 pfsr3; do DRIVE=1 CAP=360 sh tools/motion_capture.sh fpdrive_$n 400 $B $(cfg $n); done
echo done
