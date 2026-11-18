#!/bin/sh
# Review round 4 (13 Nov): every optimisation alone against stock (Hyper off, 16x anisotropy, native 720p), parked and
# driving, same method as round 2 (clean_ab.sh). FSR 1 and FSR 3 render 720p and output 1600x900.
# Parked: uncapped, 180 frames from 240 frames after the race view. Driving: straight line, 8 fps cap, 180 frames.
cd "$(dirname "$0")/.."
base="REGPU_HYPER=off FM2_ANISOTROPY=16 FM2_UPSCALER=off"
T3="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0"
CULL="REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200"
list() {
  echo "stock|"
  echo "stock2|"
  echo "nodiscard|REGPU_HYPER_NODISCARD=1"
  echo "shaderopt|REGPU_HYPER_SHADER_OPT=1"
  echo "plainmul|REGPU_HYPER_PLAIN_MUL=5"
  echo "fetchspec|REGPU_HYPER_FETCHSPEC=1"
  echo "swizzle|REGPU_HYPER_SWIZZLE_SEL=1"
  echo "leanlod|REGPU_HYPER_LEAN_LOD=1"
  echo "tailspec|REGPU_HYPER_TAILSPEC=1"
  echo "lean3|REGPU_HYPER_LEAN_FETCH=3"
  echo "leanv|REGPU_HYPER_LEAN_VFETCH=1"
  echo "r1010|REGPU_HYPER_RESOLVE1010=1"
  echo "bysize|REGPU_HYPER_RESOLVE_BY_SIZE=1"
  echo "aniso0|REGPU_HYPER_ANISO=0"
  echo "help25|REGPU_HYPER_HELPERS=25"
  echo "freeze|REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3"
  echo "dec10|REGPU_HYPER_DECIMATE=10 REGPU_HYPER_VCACHE=2"
  echo "cull64|REGPU_HYPER_CULL_PIXELS=64 $CULL"
  echo "cull256|REGPU_HYPER_CULL_PIXELS=256 $CULL"
  echo "frzshadows|REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3 REGPU_HYPER_FREEZE_SHADOWS=1 REGPU_HYPER_RESOLVE_BY_SIZE=1"
  echo "fsr1|FM2_RESOLUTION=1600x900 FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80"
  echo "fsr3|$T3"
}
# warm-up: shader-changing configs compile once, unmeasured
export BENCH_RACENOW="drive=0"
list | while IFS='|' read n a; do case $n in stock*|r1010|bysize|aniso0|help25|freeze|dec10|cull*|frzshadows) ;; *) sh tools/hyper_bench.sh r4w-$n 20 $base $a >/dev/null;; esac; done
list | while IFS='|' read n a; do
  [ -f out/motion/r4park_$n/179.png ] || DRIVE=0 CAP=360 sh tools/motion_capture.sh r4park_$n 240 $base $a
  [ -f out/motion/r4drive_$n/179.png ] || DRIVE=1 CAP=8 sh tools/motion_capture.sh r4drive_$n 24 $base $a
done
echo done
