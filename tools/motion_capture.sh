#!/bin/sh
# Motion clip: straight-line auto-drive at a fixed 12 fps cap, 120 frames from <at> seconds, kept in out/motion/<name>.
# Usage: tools/motion_capture.sh <name> <frames after the road is first seen> [NAME=VALUE ...]
cd "$(dirname "$0")/.."
name=$1; at=$2; shift 2
taskkill //F //IM fm2.exe >/dev/null 2>&1; sleep 2
rm -rf out/sweep
env FM2_MAX_FPS=${CAP:-8} FM2_AUTODRIVE_STRAIGHT=1 FM2_DUMP_AFTER_RACE=$at FM2_SWEEP=0,0,180 "$@" ./build/rel/fm2.exe game --racenow drive=${DRIVE:-1} > /dev/null 2> out/motion_$name.err &
waited=0
until [ -f out/sweep/179.png ] || [ $waited -ge 480 ]; do sleep 2; waited=$((waited + 2)); done
sleep 6; taskkill //F //IM fm2.exe >/dev/null 2>&1
mkdir -p out/motion; rm -rf out/motion/$name
sleep 3; if [ -f out/sweep/179.png ]; then mv out/sweep out/motion/$name; echo "$name ok"; else echo "$name FAILED"; fi
