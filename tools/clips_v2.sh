cd "$(dirname "$0")/.."
for n in stock2 aniso0 aniso2 helpers25 helpers50 freeze decimate10 decimate25 cull64 cull64probe cull256probe frzshadows; do
  py.exe -3 tools/clip_mp4.py out/motion/park_stock out/motion/park_$n ../_decide/v2/park_$n.mp4 30 0 "Stock" "Stock + $n"
  py.exe -3 tools/clip_mp4.py out/motion/drive_stock out/motion/drive_$n ../_decide/v2/drive_$n.mp4 8 1 "Stock" "Stock + $n"
done
