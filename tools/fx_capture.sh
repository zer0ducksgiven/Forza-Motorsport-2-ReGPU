#!/bin/sh
# Per-effect flicker captures (laptop, 15 Nov 2026): every individual speed-up alone, at native 720p with 16x anisotropy.
#   static : parked on the grid, 60 consecutive frames from 30 s in     -> $D/static/<name>/000..059.png
#   drive  : straight-line auto-drive at a 12 fps cap, 180 frames       -> $D/drive/<name>/000..179.png
# native and native2 are the reference and the noise floor. Resumable: a finished capture is skipped.
# Usage: sh tools/fx_capture.sh [static|drive|both] [name-regex]
cd "$(dirname "$0")/.."
what=${1:-both}; only=${2:-.}
B="FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16"
CULL256="REGPU_HYPER_CULL_PIXELS=256 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200"
D=${FX_DIR:-out/fx}
mkdir -p $D/static $D/drive
one() {
    name=$1; shift
    echo "$name" | grep -Eq "$only" || return 0
    if [ "$what" != drive ] && [ ! -f $D/static/$name/059.png ]; then
        rm -rf out/sweep $D/static/$name
        BENCH_RACENOW="drive=0" BENCH_PERF= sh tools/hyper_bench.sh fxs-$name 40 $B FM2_DUMP_AT=30 FM2_SWEEP=0,0,60 "$@" >/dev/null
        sleep 2
        if [ -f out/sweep/059.png ]; then mv out/sweep $D/static/$name; echo "static $name ok"; else echo "static $name FAILED" | tee -a out/bench_failures.txt; fi
    fi
    if [ "$what" != static ] && [ ! -f $D/drive/$name/179.png ]; then
        rm -rf out/motion/fx-$name $D/drive/$name
        # As tools/motion_capture.sh, but a launch that sticks on the loading screen (no frame dumped after 100 s; about
        # 4 launches in 10 at this frame cap) is killed and retried instead of waiting 8 minutes.
        for attempt in 1 2 3 4 5; do
            taskkill //F //IM fm2.exe >/dev/null 2>&1; sleep 2; rm -rf out/sweep
            env FM2_MAX_FPS=12 FM2_AUTODRIVE_STRAIGHT=1 FM2_DUMP_AFTER_RACE=60 FM2_SWEEP=0,0,180 $B "$@" ./build/rel/fm2.exe game --racenow drive=1 > /dev/null 2> out/motion_fx-$name.err &
            waited=0
            until [ -f out/sweep/179.png ] || [ $waited -ge 160 ]; do
                sleep 2; waited=$((waited + 2))
                if [ $waited -ge 100 ] && [ ! -f out/sweep/000.png ]; then break; fi
            done
            [ -f out/sweep/179.png ] && break
            echo "drive $name: attempt $attempt stuck, retrying" | tee -a out/bench_failures.txt
        done
        sleep 6; taskkill //F //IM fm2.exe >/dev/null 2>&1; sleep 3
        if [ -f out/sweep/179.png ]; then mv out/sweep $D/drive/$name; echo "drive $name ok"; else echo "drive $name FAILED" | tee -a out/bench_failures.txt; fi
    fi
}
if [ -n "$FX_STACKS" ]; then
# Stacks (16 Nov 2026): FX_STACKS=1 FX_DIR=out/fxs. Full Potato, Potato with one part switched off at a time, the three
# groups alone and in pairs, the older laptop set, upscaler variants and a 30 fps driving capture.
P="REGPU_HYPER=potato"
SURF="REGPU_HYPER_HELPERS=25 REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_RESOLVE_BY_SIZE=1"
GEOM="REGPU_HYPER_CULL_PIXELS=128 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200 REGPU_HYPER_DECIMATE=10 REGPU_HYPER_VCACHE=2"
SHAD="REGPU_HYPER_SHADER_OPT=1 REGPU_HYPER_PLAIN_MUL=5 REGPU_HYPER_FETCHSPEC=1 REGPU_HYPER_SWIZZLE_SEL=1 REGPU_HYPER_LEAN_LOD=1 REGPU_HYPER_TAILSPEC=1 REGPU_HYPER_NODISCARD=1"
one potato        $P
one c128          REGPU_HYPER_CULL_PIXELS=128 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200
one c128-help25   REGPU_HYPER_CULL_PIXELS=128 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200 REGPU_HYPER_HELPERS=25
one c128-help25-pin REGPU_HYPER_CULL_PIXELS=128 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1 REGPU_HYPER_CULL_HYST=200 REGPU_HYPER_HELPERS=25
one potato-pin    $P
one potato-pin0   $P REGPU_HYPER_CULL_PIN=0
one p-nocull      $P REGPU_HYPER_CULL_PIXELS=-1
one p-nofreeze    $P REGPU_HYPER_FREEZE=0
one p-help100     $P REGPU_HYPER_HELPERS=100
one p-nodec       $P REGPU_HYPER_DECIMATE=0
one p-aniso16     $P REGPU_HYPER_ANISO=16
one p-noshader    $P REGPU_HYPER_SHADER_OPT=-1 REGPU_HYPER_PLAIN_MUL=0 REGPU_HYPER_FETCHSPEC=0 REGPU_HYPER_SWIZZLE_SEL=0 REGPU_HYPER_LEAN_LOD=0 REGPU_HYPER_TAILSPEC=0
one p-discard     $P REGPU_HYPER_NODISCARD=0
one p-noresolve   $P REGPU_HYPER_RESOLVE1010=0 REGPU_HYPER_RESOLVE_BY_SIZE=0
one p-novcache    $P REGPU_HYPER_VCACHE=0
one g-surf        $SURF
one g-geom        $GEOM
one g-shader      $SHAD
one g-surf-geom   $SURF $GEOM
one g-surf-shader $SURF $SHAD
one g-geom-shader $GEOM $SHAD
one potato-laptop $P REGPU_HYPER_POTATO_SET=laptop
one potato-30fps  $P FM2_MAX_FPS=30
one potato-fsr1   $P FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900
one potato-fsr3   $P FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3 FM2_FSR_VERSION=0
one potato2       $P
else
one native
one aniso2     REGPU_HYPER_ANISO=2
one aniso0     REGPU_HYPER_ANISO=0
one help50     REGPU_HYPER_HELPERS=50
one help25     REGPU_HYPER_HELPERS=25
one r1010      REGPU_HYPER_RESOLVE1010=1
one dec10      REGPU_HYPER_DECIMATE=10
one cull64     REGPU_HYPER_CULL_PIXELS=64 REGPU_HYPER_CULL_ALL=1
one cull0      REGPU_HYPER_CULL_PIXELS=0 REGPU_HYPER_CULL_ALL=1
one cull256fix $CULL256
one a2m        REGPU_HYPER_A2M_TEST=1
one freeze     REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3
one lean2      REGPU_HYPER_LEAN_FETCH=2
one lean3      REGPU_HYPER_LEAN_FETCH=3
one leanv      REGPU_HYPER_LEAN_VFETCH=1
one nodiscard  REGPU_HYPER_NODISCARD=1
one shaderopt  REGPU_HYPER_SHADER_OPT=1
one plainmul   REGPU_HYPER_PLAIN_MUL=5
one fetchspec  REGPU_HYPER_FETCHSPEC=1
one swizzle    REGPU_HYPER_SWIZZLE_SEL=1
one leanlod    REGPU_HYPER_LEAN_LOD=1
one tailspec   REGPU_HYPER_TAILSPEC=1
one bysize     REGPU_HYPER_RESOLVE_BY_SIZE=1
one native2
fi
echo FXDONE
