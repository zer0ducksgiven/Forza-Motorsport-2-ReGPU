#!/bin/sh
# Intel research (11 Nov), after the laptop's deck_bench.sh: every speed-up alone against TRUE native 720p
# (FM2_ANISOTROPY=16 forced: settings.ini has anisotropy=0), the laptop's Turbo stacks, Intel Turbo, and the presets at
# native. 3 rounds, native twice per round, parked, GPU stats on, screenshot at 25 s. Labels iq-<name>-<round>.
# A warm-up round (iw-*) first compiles every shader variant so no measured run pays for compiles.
cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16 FM2_DUMP_AT=25"
LSTRICT="REGPU_HYPER_LEAN_FETCH=2 REGPU_HYPER_LEAN_VFETCH=1 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_CULL_PIXELS=0 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_A2M_TEST=1"
ITURBO="REGPU_HYPER_NODISCARD=1 REGPU_HYPER_SHADER_OPT=1 REGPU_HYPER_PLAIN_MUL=5 REGPU_HYPER_FETCHSPEC=1 REGPU_HYPER_SWIZZLE_SEL=1 REGPU_HYPER_LEAN_LOD=1 REGPU_HYPER_TAILSPEC=1 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_RESOLVE_BY_SIZE=1 REGPU_HYPER_CULL_PIXELS=0 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PROBE=1"
NAT="REGPU_HYPER_SCENE=100 REGPU_HYPER_UPSCALE=0"
CULL256="REGPU_HYPER_CULL_PIXELS=256 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200"
all() {
    run native
    run aniso2     REGPU_HYPER_ANISO=2
    run aniso0     REGPU_HYPER_ANISO=0
    run help50     REGPU_HYPER_HELPERS=50
    run help25     REGPU_HYPER_HELPERS=25
    run r1010      REGPU_HYPER_RESOLVE1010=1
    run dec10      REGPU_HYPER_DECIMATE=10
    run cull64     REGPU_HYPER_CULL_PIXELS=64 REGPU_HYPER_CULL_ALL=1
    run cull0      REGPU_HYPER_CULL_PIXELS=0 REGPU_HYPER_CULL_ALL=1
    run a2m        REGPU_HYPER_A2M_TEST=1
    run freeze     REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3
    run lean2      REGPU_HYPER_LEAN_FETCH=2
    run lean3      REGPU_HYPER_LEAN_FETCH=3
    run leanv      REGPU_HYPER_LEAN_VFETCH=1
    run native2
    run nodiscard  REGPU_HYPER_NODISCARD=1
    run shaderopt  REGPU_HYPER_SHADER_OPT=1
    run plainmul   REGPU_HYPER_PLAIN_MUL=5
    run fetchspec  REGPU_HYPER_FETCHSPEC=1
    run swizzle    REGPU_HYPER_SWIZZLE_SEL=1
    run leanlod    REGPU_HYPER_LEAN_LOD=1
    run tailspec   REGPU_HYPER_TAILSPEC=1
    run bysize     REGPU_HYPER_RESOLVE_BY_SIZE=1
    run cull256fix $CULL256
    run lstrict    $LSTRICT
    run lturbo     $LSTRICT REGPU_HYPER_LEAN_FETCH=3
    run iturbo     $ITURBO
    run potato     REGPU_HYPER=potato
    run mild       REGPU_HYPER=mild $NAT
    run low        REGPU_HYPER=low $NAT
    run extreme    REGPU_HYPER=extreme $NAT
}
# warm-up: shader-changing configs only, short, not measured
run() { case "$1" in native*|aniso*|help*|r1010|dec10|cull*|a2m|freeze|bysize) return;; esac; n=$1; shift; sh tools/hyper_bench.sh "iw-$n" 10 $B "$@" >/dev/null; }
all
for r in 1 2 3; do
    run() { n=$1; shift; sh tools/hyper_bench.sh "iq-$n-$r" 30 $B "$@" >/dev/null; }
    all
done
echo done
