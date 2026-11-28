#!/bin/bash
# Linux / Steam Deck: runs the ReGPU-V build (build/regpu/fm2.exe, -DFM2_REGPU=ON) through Proton, with its own
# prefix, offline, intro skipped. FM2_EXE, FM2_PROTON and FM2_PREFIX override the paths.
#   tools/regpu/run.sh [fm2 args, e.g. --racenow=race=0,ai=1] ; extra environment passes through (FM2_*, REGPU_*).
# Test defaults (each overridable): 1280x720 windowed, MSAA 2x, uncapped (max_fps 360, vsync off), the debug server on
# port 7343 (ReGPU's: regpuctl.py). Log: out/run/fm2.log. User folder: $PREFIX/pfx/drive_c/users/steamuser/Documents/fm2.
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
PROTON="${FM2_PROTON:-$HOME/.steam/steam/steamapps/common/Proton 11.0}"
PREFIX="${FM2_PREFIX:-$HOME/.local/share/fm2-regpu/prefix}"
EXE="${FM2_EXE:-$ROOT/build/regpu/fm2.exe}"
# Test runs use a profile of their own (FM2_PROFILE overrides). A title-update build re-saves a profile in the TU's
# format, which a disc build then refuses ("requires the latest title update"): keep disc and TU runs on separate ones.
export FM2_PROFILE="${FM2_PROFILE:-Benchmark}"
RUN="$ROOT/out/run"; mkdir -p "$RUN" "$PREFIX"
winpath() { printf 'Z:%s' "$(readlink -f "$1" | tr '/' '\\')"; }
USERD="$PREFIX/pfx/drive_c/users/steamuser/Documents/fm2"
mkdir -p "$USERD"
# Offline (no FM2 Live, no update check) and no intro, every run.
if [ ! -f "$USERD/settings.ini" ]; then printf 'online_server=off\nskip_intro=1\n' > "$USERD/settings.ini"; fi
grep -q '^online_server=off' "$USERD/settings.ini" || sed -i 's/^online_server=.*/online_server=off/' "$USERD/settings.ini"
pgrep -x fm2.exe >/dev/null && { echo "fm2.exe is running: close it first" >&2; exit 1; }
kscreen-doctor --dpms on >/dev/null 2>&1 # (the Deck's screen on, if it dimmed)
[ -f "$RUN/fm2.log" ] && mv "$RUN/fm2.log" "$RUN/fm2.prev.log"
cd "$RUN" && exec env STEAM_COMPAT_DATA_PATH="$PREFIX" STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam" WINEDEBUG=-all \
  PROTON_USE_XALIA=0 FM2_NO_LAUNCHER=1 FM2_NO_UPDATE_CHECK=1 FM2_SKIP_INTRO=1 REGPU_DEBUG=1 REGPU_DEBUG_PORT=7343 \
  ${FM2_DECK_TEST:+FM2_DECK_TEST=1} $( [ -z "${FM2_DECK_TEST:-}" ] && echo FM2_RESOLUTION=${FM2_RESOLUTION:-1280x720} FM2_MSAA=${FM2_MSAA:-2} FM2_MAX_FPS=${FM2_MAX_FPS:-360} FM2_VSYNC=${FM2_VSYNC:-0} ) \
  FM2_FULLSCREEN="${FM2_FULLSCREEN:-0}" \
  "$PROTON/proton" run "$EXE" "$(winpath "$ROOT/game")" "$@"
