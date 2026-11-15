#!/bin/sh
# Cull 256 climb (14 Nov): cars vanish at a distance. Potato, driving, 3 interleaved rounds: cull variants.
cd "$(dirname "$0")/.."
B="REGPU_HYPER=potato FM2_ANISOTROPY=16"
export BENCH_RACENOW="drive=1"
for r in 1 2 3; do
  sh tools/hyper_bench.sh cc-256-$r 30 $B
  sh tools/hyper_bench.sh cc-mx1k-$r 30 $B REGPU_HYPER_CULL_MAXIDX=1024
  sh tools/hyper_bench.sh cc-mx4k-$r 30 $B REGPU_HYPER_CULL_MAXIDX=4096
  sh tools/hyper_bench.sh cc-128-$r 30 $B REGPU_HYPER_CULL_PIXELS=128
  sh tools/hyper_bench.sh cc-off-$r 30 $B REGPU_HYPER_CULL_PIXELS=-1
done
echo done
