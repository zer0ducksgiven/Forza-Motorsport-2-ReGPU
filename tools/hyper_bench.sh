#!/bin/sh
# ReGPU_Hyper A/B benchmark: one --racenow race (Nissan Speedway, auto-drive), uncapped, measured over a fixed
# window once the race runs. Usage: tools/hyper_bench.sh <label> <seconds> [NAME=VALUE ...]
# BENCH_RACENOW="drive=0" (environment): a --racenow payload. BENCH_PERF=1: also FM2_PERF and FM2_GPU_STATS (the GPU and
# renderer columns), which cost frame time themselves; without it only fps is measured.
# Appends one line to out/bench.txt: label, average fps, 1% low (fps meter), GPU and renderer ms per frame.
set -e
cd "$(dirname "$0")/.."
label=$1; secs=$2; shift 2
log=out/bench_$label.err
taskkill //F //IM fm2.exe >/dev/null 2>&1 || true
sleep 2
perf=""; if [ -n "$BENCH_PERF" ]; then perf="FM2_PERF=1 FM2_GPU_STATS=1"; fi
# The race has started when the fps meter logs. A run that crashes or never gets there within 180 s is retried
# (up to 3 attempts) so an unattended batch never hangs.
for attempt in 1 2 3; do
    env FM2_MAX_FPS=360 $perf "$@" ./build/rel/fm2.exe game --racenow $BENCH_RACENOW > /dev/null 2> "$log" &
    waited=0
    until grep -q "fps meter" "$log" 2>/dev/null; do
        sleep 2; waited=$((waited + 2))
        if grep -q "CRASH:" "$log" 2>/dev/null || [ $waited -ge 180 ]; then break; fi
    done
    grep -q "fps meter" "$log" 2>/dev/null && break
    echo "$label: attempt $attempt failed (crash or timeout), retrying" | tee -a out/bench_failures.txt
    cp -f "$log" "out/bench_${label}_fail$attempt.err"
    taskkill //F //IM fm2.exe >/dev/null 2>&1 || true
    sleep 3
done
grep -q "fps meter" "$log" || { echo "$label: gave up after 3 attempts" | tee -a out/bench_failures.txt; exit 1; }
start=$(grep -m1 "fps meter" "$log" | sed 's/^\[ *\([0-9.]*\)\].*/\1/')
lines=$(( secs / 10 + 1 ))
waited=0
until [ "$(grep -c 'fps meter' "$log")" -ge "$lines" ]; do
    sleep 2; waited=$((waited + 2))
    if grep -q "CRASH:" "$log" || [ $waited -ge $((secs + 120)) ]; then
        echo "$label: crashed or stalled during measurement" | tee -a out/bench_failures.txt; taskkill //F //IM fm2.exe >/dev/null 2>&1 || true; exit 1
    fi
done
taskkill //F //IM fm2.exe >/dev/null 2>&1 || true
# FM2_DUMP_AT=<seconds> in the arguments: the screenshot it writes is kept as out/bench_<label>.png
if [ -f frame.png ]; then mv -f frame.png "out/bench_$label.png"; fi
meter=$(grep "fps meter" "$log" | tail -1 | sed 's/.*now, \([0-9.]*\) average, \([0-9.]*\) 1% low.*/\1 \2/')
gpu=$(awk -v s="$start" '{t=substr($0,2,index($0,"]")-2)+0} t>s+5 && /GPU time [0-9.]+ ms\/frame/ {match($0,/GPU time [0-9.]+/); g+=substr($0,RSTART+9,RLENGTH-9); n++} END{printf "%.2f", n?g/n:0}' "$log")
draw=$(awk -v s="$start" '{t=substr($0,2,index($0,"]")-2)+0} t>s+5 && /renderer draw [0-9.]+ ms/ {match($0,/renderer draw [0-9.]+/); g+=substr($0,RSTART+14,RLENGTH-14); n++} END{printf "%.2f", n?g/n:0}' "$log")
skipped=$(awk -v s="$start" '{t=substr($0,2,index($0,"]")-2)+0} t>s && /compiled in/ {n++} END{print n+0}' "$log")
# Race fps: the average of the meter's "fps now" readings after the race started (the meter's own average also
# counts the loading screens, which run far faster: it read ~80 when the race ran at ~52).
racefps=$(awk -v s="$start" '{t=substr($0,2,index($0,"]")-2)+0} t>s+1 && /fps meter/ {match($0,/meter: [0-9.]+/); g+=substr($0,RSTART+7,RLENGTH-7); n++} END{printf "%.1f", n?g/n:0}' "$log")
perffps=$(awk -v s="$start" '{t=substr($0,2,index($0,"]")-2)+0} t>s+5 && /perf: [0-9.]+ fps/ {match($0,/perf: [0-9.]+/); g+=substr($0,RSTART+6,RLENGTH-6); n++} END{printf "%.1f", n?g/n:0}' "$log")
# A run stuck on the loading screen (race fps > 250: the race never drew) is rerun once, as a failure. [laptop]
if [ "${racefps%.*}" -gt 250 ] && [ -z "$BENCH_RERUN" ]; then
    echo "$label: stuck on the loading screen (race fps $racefps), rerunning" | tee -a out/bench_failures.txt
    cp -f "$log" "out/bench_${label}_stuck.err"
    BENCH_RERUN=1 exec sh "$0" "$label" "$secs" "$@"
fi
# Median fps (Intel box): the median of the per-second perf readings from 10 s after the race start. Race fps averages
# only ~3 meter readings (0.67 fps steps on a ~20 fps box) and the perf mean can include loading frames; the median
# of ~20 readings is steady.
medfps=$(awk -v s="$start" '{t=substr($0,2,index($0,"]")-2)+0} t>s+10 && /perf: [0-9.]+ fps/ {match($0,/perf: [0-9.]+/); print substr($0,RSTART+6,RLENGTH-6)+0}' "$log" | sort -n | awk '{a[NR]=$1} END{if(NR==0)print "0.0"; else if(NR%2)printf "%.1f", a[(NR+1)/2]; else printf "%.1f", (a[NR/2]+a[NR/2+1])/2}')
set -- $meter
printf "%-14s race %5s fps (perf %5s)   meter avg %6s (incl. loading)   1%% low %6s   GPU %6s ms/frame   renderer %6s ms/frame   shaders compiled during race %s   median %s fps\n" \
    "$label" "$racefps" "$perffps" "$1" "$2" "$gpu" "$draw" "$skipped" "$medfps" | tee -a out/bench.txt
