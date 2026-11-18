#!/bin/sh
# Full stack (14 Nov): Potato (cull 128) plus every other optimisation passed in round 4, with FSR 3 720p->900p.
# Warm-up 15 min (new shader set + FSR 3 motion variants), then fps parked and driving (3 interleaved rounds, 30 s),
# then review clips against stock native: parked uncapped and driving at 8 fps.
cd "$(dirname "$0")/.."
B="REGPU_HYPER=potato FM2_ANISOTROPY=16"
X="REGPU_HYPER_NODISCARD=1 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_ANISO=0 REGPU_HYPER_HELPERS=25 REGPU_HYPER_DECIMATE=10 REGPU_HYPER_FREEZE_SHADOWS=1"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0"
cfg() { case $1 in
  p720) echo "FM2_UPSCALER=off FM2_RESOLUTION=1280x720";;
  pfsr3) echo "$T";;
  max720) echo "$X FM2_UPSCALER=off FM2_RESOLUTION=1280x720";;
  maxfsr3) echo "$X $T";;
esac; }
BENCH_RACENOW="drive=1" sh tools/hyper_bench.sh mx-warm 300 $B $(cfg maxfsr3) >/dev/null
BENCH_RACENOW="drive=1" sh tools/hyper_bench.sh mx-warm2 120 $B $(cfg max720) >/dev/null
for mode in 0 1; do export BENCH_RACENOW="drive=$mode"
  for r in 1 2 3; do for n in p720 pfsr3 max720 maxfsr3; do sh tools/hyper_bench.sh mf$mode-$n-$r 30 $B $(cfg $n) >/dev/null; done; done
done
unset BENCH_RACENOW
S="REGPU_HYPER=off FM2_ANISOTROPY=16 FM2_UPSCALER=off"
for t in 1 2 3; do [ -f out/motion/r6park_maxfsr3/179.png ] || DRIVE=0 CAP=360 sh tools/motion_capture.sh r6park_maxfsr3 240 $B $(cfg maxfsr3); done
for t in 1 2 3; do [ -f out/motion/r6drive_maxfsr3/179.png ] || DRIVE=1 CAP=8 sh tools/motion_capture.sh r6drive_maxfsr3 24 $B $(cfg maxfsr3); done
for t in 1 2 3; do [ -f out/motion/r6drivefree_maxfsr3/179.png ] || DRIVE=1 CAP=360 sh tools/motion_capture.sh r6drivefree_maxfsr3 400 $B $(cfg maxfsr3); done
echo done
