#!/bin/bash
# The pixel side of a frozen-grid A/B (bench.sh with FM2_BENCH_AB): offA vs offB is the noise between two shots of one
# state; offB vs onA is the switch's change (shots ~1 s apart); onA vs onB the noise again.
#   tools/regpu/abdiff.sh <out/bench/run folder>
D="$1"; ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
for p in "offA offB" "offB onA" "onA onB"; do set -- $p; printf "  %-5s vs %-5s " $1 $2
  ${PIXDIFF_PREFIX:-} python3 -I "$ROOT/tools/regpu/pixdiff.py" "$D/$1.png" "$D/$2.png" "$D/diff-$1-$2.png"; done
