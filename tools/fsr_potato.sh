#!/bin/sh
# Potato + upscaling to 1600x900 (13 Nov): Potato at native 720p vs Potato rendering 720p and upscaling to 900p with
# FSR 1, FSR 2.3.4 and FSR 3.1.5. Anisotropy 16x forced (Potato caps it). Warm-up per temporal config, 3 rounds.
cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="REGPU_HYPER=potato FM2_ANISOTROPY=16 FM2_DUMP_AFTER_RACE=60"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3"
cfg() {
  case $1 in
    p720) echo "FM2_UPSCALER=off FM2_RESOLUTION=1280x720";;
    pfsr1) echo "FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900";;
    pfsr2) echo "$T FM2_FSR_VERSION=1";;
    pfsr3) echo "$T FM2_FSR_VERSION=0";;
  esac
}
# Long warm-up: Potato + FSR 2/3 builds a motion version of every per-draw shader variant (seconds each, plus long
# driver pipeline builds), so each temporal config first runs 15 minutes unmeasured, until the cache is complete.
warm() { n=$1; taskkill //F //IM fm2.exe >/dev/null 2>&1; sleep 2
  env FM2_MAX_FPS=360 $B $(cfg $n) ./build/rel/fm2.exe game --racenow drive=0 > /dev/null 2> out/fpw-long-$n.err &
  sleep 900; taskkill //F //IM fm2.exe >/dev/null 2>&1; sleep 3
  echo "warm $n: $(grep -c 'compiled in' out/fpw-long-$n.err) compiles, last at $(grep 'compiled in' out/fpw-long-$n.err | tail -1 | cut -c2-11) s"; }
warm pfsr2; warm pfsr3
sh tools/hyper_bench.sh fpw-pfsr1 60 $B $(cfg pfsr1) >/dev/null
for r in 1 2 3; do for n in p720 pfsr1 pfsr2 pfsr3; do sh tools/hyper_bench.sh fpq-$n-$r 30 $B $(cfg $n) >/dev/null; done; done
echo done
