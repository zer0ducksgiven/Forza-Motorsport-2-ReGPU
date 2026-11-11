#!/bin/sh
# Boots straight into a race and holds the throttle: skips the intro videos, drives the menus,
# then measures. This is benchmark.cmd's route with the 100-second intro wait removed, which is
# the difference between ~2.5 minutes and ~50 seconds to the grid.
#
#   linux/race.sh                 # the native Vulkan backend (default)
#   linux/race.sh d3d12           # Direct3D 12 (use FM2_D3D_DXC=1 with the staged Windows dxc)
#   linux/race.sh vulkan          # explicit, same as the default
#   FM2_RACE_SECONDS=40 linux/race.sh vulkan
#
# Ask for d3d12 explicitly to measure Direct3D 12 with the same route.
set -e
LINUX_DIR=$(cd "$(dirname "$0")" && pwd)
BACKEND=${1:-vulkan}
if [ $# -gt 0 ]; then shift; fi
HOLD=${FM2_RACE_SECONDS:-25}

# Each button press lasts 150 ms, followed by a 600 ms gap. IDLE waits for shader compilation.
# Four LEFT presses avoid locked events; moving RIGHT selects a locked track. Press A on Nissan
# Speedway, C class, Eclipse, Accept Assists, and Start Race, with a capture after each screen.
# After loading, press A once more to continue into driving. CAPTURE must show the fresh race HUD.
FM2_TOUR=${FM2_TOUR:-WAIT:25,START,IDLE,A,IDLE,A,IDLE,DOWN,IDLE,A,IDLE,DOWN,IDLE,A,IDLE,A,IDLE,LEFT,IDLE,LEFT,IDLE,LEFT,IDLE,LEFT,IDLE,CAPTURE,A,IDLE,WAIT:35,CAPTURE,A,IDLE,CAPTURE,A,IDLE,CAPTURE,A,IDLE,WAIT:20,CAPTURE,A,IDLE,WAIT:20,A,IDLE,CAPTURE,RT:${HOLD},QUIT}
export FM2_TOUR
export FM2_SKIP_INTRO=${FM2_SKIP_INTRO:-1}
export FM2_NO_LAUNCHER=${FM2_NO_LAUNCHER:-1}
export FM2_PERF=${FM2_PERF:-1}
export FM2_VSYNC=${FM2_VSYNC:-0}
export FM2_MAX_FPS=${FM2_MAX_FPS:-360}
if [ "$BACKEND" = vulkan ]; then
    export FM2_EXE=${FM2_EXE:-fm2-vk.exe}
    export FM2_RENDERER=vulkan
    export FM2_VK_DRAWS=${FM2_VK_DRAWS:-1}
fi
exec "$LINUX_DIR/play.sh" "$@"