#!/bin/sh
# Round 4 review clips: stock (left) vs stock + one optimisation (right), parked 30 fps, driving 8 fps timer-aligned.
cd "$(dirname "$0")/.."; mkdir -p ../_decide/v4
for n in ${@:-stock2 nodiscard shaderopt plainmul fetchspec swizzle leanlod tailspec lean3 leanv r1010 bysize aniso0 help25 freeze dec10 cull64 cull256 frzshadows fsr1 fsr3}; do
  for m in park drive; do
    [ -f ../_decide/v4/${m}_$n.mp4 ] && continue; [ -f out/motion/r4${m}_$n/179.png ] || continue
    if [ $m = park ]; then f=30; al=0; else f=8; al=1; fi
    py.exe -3 tools/clip_mp4.py out/motion/r4${m}_stock out/motion/r4${m}_$n ../_decide/v4/${m}_$n.mp4 $f $al "Stock" "Stock + $n"
  done
done
