#!/bin/sh
# Rebuild ReXenon (our XenonRecomp fork) and regenerate out/ppc from game/default.xex.
set -e
cd "$(dirname "$0")/.."
cmd //c "tools\vcenv.cmd ninja -C build/tools xenonrecomp/XenonRecomp/XenonRecomp.exe" | tail -1
rm -rf out/ppc && mkdir -p out/ppc
build/tools/xenonrecomp/XenonRecomp/XenonRecomp.exe config/fm2.toml thirdparty/ReXenon/XenonUtils/ppc_context.h > out/recomp.log 2>&1
grep -v "^Recompiling" out/recomp.log || true
py -3 -I tools/genstubs.py out/ppc/ppc_recomp_shared.h src/kernel/stubs.cpp
