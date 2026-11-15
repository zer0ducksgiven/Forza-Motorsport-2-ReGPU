#!/bin/sh
# 14 Nov: why Potato + FSR 3 flickers parked (6.35%), and which extra flickers on top of Potato at 720p (0.35%).
cd "$(dirname "$0")/.."
B="REGPU_HYPER=potato FM2_ANISOTROPY=16"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0"
N="FM2_UPSCALER=off FM2_RESOLUTION=1280x720"
run() { n=$1; shift; for t in 1 2; do [ -f out/motion/r6park_$n/179.png ] || DRIVE=0 CAP=360 sh tools/motion_capture.sh r6park_$n 240 "$@"; done
  python tools/flicker.py out/motion/r6park_$n out/motion/r4park_stock out/flk4/park_$n.png; }
run f3nocull $B $T REGPU_HYPER_CULL_PIXELS=-1
run f3nofreeze $B $T REGPU_HYPER_FREEZE=0
run f3nospec $B $T REGPU_HYPER_FETCHSPEC=0 REGPU_HYPER_TAILSPEC=0 REGPU_HYPER_SWIZZLE_SEL=0
run p720 $B $N
for x in NODISCARD=1 RESOLVE1010=1 ANISO=0 HELPERS=25 DECIMATE=10 FREEZE_SHADOWS=1; do run px_$(echo $x | tr -d '=_' | tr A-Z a-z) $B $N REGPU_HYPER_$x; done
echo done
