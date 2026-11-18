cd "$(dirname "$0")/.."
cmd //c "tools\vcenv.cmd cmake --build build\rel --target fm2 > out\build_ab.log 2>&1" || { echo BUILD FAILED; exit 1; }
echo build ok
base="FM2_UPSCALER=off"
cap() { name=$1; shift; [ -f out/motion/park_$name/179.png ] && return; DRIVE=0 CAP=360 sh tools/motion_capture.sh park_$name 240 $base "$@"; }
F="REGPU_HYPER_FREEZE=120 REGPU_HYPER_FREEZE_REFRESH=3"
C="REGPU_HYPER_CULL_PIXELS=256 REGPU_HYPER_CULL_ALL=1 REGPU_HYPER_CULL_PASSES=1 REGPU_HYPER_CULL_PROBE=1"
cap stock3
cap frzshadows_bysize $F REGPU_HYPER_FREEZE_SHADOWS=1 REGPU_HYPER_RESOLVE_BY_SIZE=1
cap bysize REGPU_HYPER_RESOLVE_BY_SIZE=1
cap cull256hyst150 $C REGPU_HYPER_CULL_HYST=150
cap cull256hyst200 $C REGPU_HYPER_CULL_HYST=200
for n in stock3 frzshadows_bysize bysize cull256probe cull256hyst150 cull256hyst200; do py.exe -3 tools/flicker.py out/motion/park_$n out/motion/park_stock out/flkmap/climb_$n.png; done
