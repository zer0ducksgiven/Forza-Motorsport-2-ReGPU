#!/bin/sh
# Race-start hang test (16 Nov 2026): launches --racenow N times per arm at a 12 fps cap (the condition where about 4 in
# 10 launches stuck) and counts launches that never start loading the track. A launch is good when the log shows
# "wheel: the game says" within 40 s of "racenow: launching"; otherwise stuck. Arms are interleaved.
# Usage: sh tools/stuck_test.sh <rounds> <label>=<ENV...> [<label>=<ENV...> ...]   (use "," between variables)
cd "$(dirname "$0")/.."
rounds=$1; shift
for r in $(seq 1 $rounds); do
    for arm in "$@"; do
        label=${arm%%=*}; vars=$(echo "${arm#*=}" | tr ',' ' ')
        taskkill //F //IM fm2.exe >/dev/null 2>&1; sleep 2
        log=out/stuck_$label-$r.err
        env FM2_MAX_FPS=12 FM2_UPSCALER=off FM2_FSR_SCALE=100 $vars ./build/rel/fm2.exe game --racenow drive=1 > /dev/null 2> $log &
        waited=0; res=stuck
        while [ $waited -lt 120 ]; do
            sleep 2; waited=$((waited + 2))
            if grep -q "wheel: the game says" $log 2>/dev/null; then res=ok; break; fi
            if grep -q "CRASH:" $log 2>/dev/null; then res=crash; break; fi
            if grep -q "racenow: launching" $log 2>/dev/null; then
                t=$(grep -m1 "racenow: launching" $log | sed 's/^\[ *\([0-9]*\).*/\1/'); now=$(tail -1 $log | sed 's/^\[ *\([0-9]*\).*/\1/')
                [ $((now - t)) -ge 40 ] 2>/dev/null && break
            fi
        done
        echo "$label round $r: $res (after ${waited}s)" | tee -a out/stuck_test.txt
    done
done
taskkill //F //IM fm2.exe >/dev/null 2>&1
echo STUCKDONE | tee -a out/stuck_test.txt
