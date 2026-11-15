#!/bin/sh
# Clean A/B clips (review round 2): stock against stock plus ONE setting, parked (uncapped, flicker) and driving
# (straight line, 8 fps cap, aligned by the race timer). Output: out/motion/<mode>_<name>.
cd "$(dirname "$0")/.."
base="FM2_UPSCALER=off"
cap() { # mode name args...
    mode=$1; name=$2; shift 2
    [ -f out/motion/${mode}_$name/179.png ] && return
    if [ $mode = park ]; then DRIVE=0 CAP=360 sh tools/motion_capture.sh ${mode}_$name 240 $base "$@"
    else DRIVE=1 CAP=8 sh tools/motion_capture.sh ${mode}_$name 24 $base "$@"; fi
}
for mode in park drive; do
    cap $mode stock
    cap $mode stock2
    cap $mode aniso0 REGPU_HYPER_ANISO=0
    cap $mode aniso2 REGPU_HYPER_ANISO=2
    cap $mode helpers25 REGPU_HYPER_HELPERS=25
    cap $mode helpers50 REGPU_HYPER_HELPERS=50
    cap $mode freeze REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3
    cap $mode decimate10 REGPU_HYPER_DECIMATE=10 REGPU_HYPER_VCACHE=2
    cap $mode decimate25 REGPU_HYPER_DECIMATE=25 REGPU_HYPER_VCACHE=2
    cap $mode cull64 REGPU_HYPER_CULL_PIXELS=64 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1
    cap $mode cull64probe REGPU_HYPER_CULL_PIXELS=64 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1
    cap $mode cull256probe REGPU_HYPER_CULL_PIXELS=256 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1
    cap $mode frzshadows REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3 REGPU_HYPER_FREEZE_SHADOWS=1
done
