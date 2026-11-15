# Conclusion benchmark: stock and every preset, 3 interleaved runs each, a screenshot on the first.
cd "$(dirname "$0")/.."
export BENCH_RACENOW=drive=0 BENCH_PERF=1
: > out/presets.txt
for i in 1 2 3; do
  for p in off mild low extreme potato auto; do
    shot=""; [ $i = 1 ] && shot="FM2_DUMP_AT=25"
    if [ $p = off ]; then env_="REGPU_HYPER=off FM2_UPSCALER=off"; else env_="REGPU_HYPER=$p REGPU_HYPER_SCENE=100 REGPU_HYPER_UPSCALE=0 FM2_UPSCALER=off FM2_FSR_SCALE=100"; fi
    sh tools/hyper_bench.sh preset-$p-$i 30 $env_ $shot > /dev/null
    line=$(grep "preset-$p-$i" out/bench.txt | tail -1)
    echo "$p $i $line" >> out/presets.txt
  done
done
