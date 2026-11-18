cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16 FM2_DUMP_AT=25"
T2="REGPU_HYPER_NODISCARD=1 REGPU_HYPER_SHADER_OPT=1 REGPU_HYPER_PLAIN_MUL=5 REGPU_HYPER_FETCHSPEC=1 REGPU_HYPER_SWIZZLE_SEL=1 REGPU_HYPER_LEAN_LOD=1 REGPU_HYPER_TAILSPEC=1 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_RESOLVE_BY_SIZE=1"
T3="$T2 REGPU_HYPER_LEAN_FETCH=3 REGPU_HYPER_LEAN_VFETCH=1"
T4="REGPU_HYPER_NODISCARD=1 REGPU_HYPER_SHADER_OPT=1 REGPU_HYPER_LEAN_FETCH=3 REGPU_HYPER_LEAN_VFETCH=1 REGPU_HYPER_SWIZZLE_SEL=1 REGPU_HYPER_LEAN_LOD=1 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_RESOLVE_BY_SIZE=1"
for n in iturbo2 iturbo3 iturbo4; do :; done
sh tools/hyper_bench.sh iw-t2 10 $B $T2 >/dev/null; sh tools/hyper_bench.sh iw-t3 10 $B $T3 >/dev/null; sh tools/hyper_bench.sh iw-t4 10 $B $T4 >/dev/null
for r in 1 2 3; do
  sh tools/hyper_bench.sh iq2-native-$r 30 $B >/dev/null
  sh tools/hyper_bench.sh iq2-iturbo2-$r 30 $B $T2 >/dev/null
  sh tools/hyper_bench.sh iq2-iturbo3-$r 30 $B $T3 >/dev/null
  sh tools/hyper_bench.sh iq2-iturbo4-$r 30 $B $T4 >/dev/null
  sh tools/hyper_bench.sh iq2-native2-$r 30 $B >/dev/null
done
echo done
