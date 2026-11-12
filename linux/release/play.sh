#!/bin/sh
# Starts Forza Motorsport 2 ReGPU through Proton (vkd3d-proton). Any FM2_* variable passes through.
#
#   ./play.sh
#   PROTON="/path/to/proton" ./play.sh
#
# Wine's own d3dcompiler_47 can't compile the renderer's shaders (black screen), so the
# Microsoft DLL shipped beside fm2.exe is preferred with WINEDLLOVERRIDES.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HOME_DIR=${FM2_LINUX_HOME:-$HOME/.fm2-linux}
COMPAT=${FM2_COMPATDATA:-$HOME_DIR/compatdata}

if [ -z "$PROTON" ]; then
    for dir in "$HOME/.local/share/Steam/steamapps/common" "$HOME/.steam/steam/steamapps/common" \
               "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common"; do
        for name in "Proton - Experimental" "Proton 11.0" "Proton 10.0"; do
            [ -x "$dir/$name/proton" ] && { PROTON="$dir/$name/proton"; break 2; }
        done
    done
fi
[ -n "$PROTON" ] && [ -x "$PROTON" ] || { echo "play.sh: no Proton found; install Proton Experimental in Steam or set PROTON=/path/to/proton" >&2; exit 1; }
[ -f "$HERE/game/default.xex" ] || { echo "play.sh: no game files yet: run ./setup.sh <disc.iso> <title update> first" >&2; exit 1; }

mkdir -p "$COMPAT"
cd "$HERE"
STEAM_COMPAT_CLIENT_INSTALL_PATH=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/steam} \
STEAM_COMPAT_DATA_PATH=$COMPAT \
WINEDLLOVERRIDES="d3dcompiler_47=n${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}" \
    "$PROTON" run "$HERE/fm2.exe" "$HERE/game" "$@"
