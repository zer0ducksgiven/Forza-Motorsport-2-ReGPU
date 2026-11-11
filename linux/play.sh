#!/bin/sh
# Runs FM2 on Linux through Proton (vkd3d-proton). Verified with Proton 11 / Experimental.
#
#   linux/play.sh                  # windowed, settings from <user>/fm2/settings.ini
#   linux/play.sh --headless       # no window (FM2_HEADLESS=1), for frame dumps
#   FM2_PERF=1 linux/play.sh       # any FM2_* variable passes through
#   FM2_EXE=fm2-vk.exe FM2_RENDERER=vulkan linux/play.sh   # the Vulkan backend
#
# The game folder is $FM2_INSTALL/game (a link to the repository's game/). Wine's built-in
# d3dcompiler_47 is replaced by the Microsoft DLL staged next to the exe, because it cannot
# compile the renderer's bindless HLSL.
set -e
LINUX_DIR=$(cd "$(dirname "$0")" && pwd)
HOME_DIR=${FM2_LINUX_HOME:-$HOME/.fm2-linux}
INSTALL=${FM2_INSTALL:-$HOME_DIR/install}
COMPAT=${FM2_COMPATDATA:-$HOME_DIR/compatdata}
PROTON=${PROTON:-"$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton"}

EXE=${FM2_EXE:-fm2.exe}
[ -f "$INSTALL/$EXE" ] || { echo "linux/play.sh: run linux/build.sh and linux/stage.sh first" >&2; exit 1; }
[ -x "$PROTON" ] || { echo "linux/play.sh: no Proton at $PROTON (set PROTON=...)" >&2; exit 1; }

[ "$1" = "--headless" ] && { export FM2_HEADLESS=1; shift; }
mkdir -p "$COMPAT"

cd "$INSTALL"
STEAM_COMPAT_CLIENT_INSTALL_PATH=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/steam} \
STEAM_COMPAT_DATA_PATH=$COMPAT \
WINEDLLOVERRIDES="d3dcompiler_47=n" \
    "$PROTON" run "$INSTALL/$EXE" "$INSTALL/game" "$@"
