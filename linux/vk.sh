#!/bin/sh
# Builds the Vulkan backend's native test tools (out/vk_*_test) and runs the ones that need no
# arguments against the real GPU:
#   linux/vk.sh                # writes out/vk_present.png
#
# Needs the Vulkan loader, the Vulkan headers (fetched once into $FM2_LINUX_HOME/vkinclude)
# and dxc on PATH or in FM2_DXC. Nothing here touches the Windows build.
set -e
LINUX_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$LINUX_DIR")
HOME_DIR=${FM2_LINUX_HOME:-$HOME/.fm2-linux}
INCLUDE=$HOME_DIR/vkinclude
OUT=$ROOT/out

if [ ! -f "$INCLUDE/vulkan/vulkan.h" ]; then
    echo "== fetching the Vulkan headers"
    mkdir -p "$HOME_DIR" "$INCLUDE"
    curl -sL -o "$HOME_DIR/vkheaders.tar.gz" \
        https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    tar xzf "$HOME_DIR/vkheaders.tar.gz" -C "$HOME_DIR"
    cp -r "$HOME_DIR"/Vulkan-Headers-*/include/* "$INCLUDE/"
fi

if [ -z "$FM2_DXC" ]; then
    command -v dxc > /dev/null || {
        echo "linux/vk.sh: set FM2_DXC=/path/to/dxc (the DirectXShaderCompiler release binary)" >&2
        exit 1
    }
fi

mkdir -p "$OUT"
g++ -O2 -std=c++20 -g -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_present_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_shader.cpp" \
    "$ROOT/src/gpu/vk/vk_blit.cpp" "$ROOT/src/png.cpp" \
    -ldl -o "$OUT/vk_present_test"
g++ -O2 -std=c++20 -g -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_shaders_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_shader.cpp" \
    "$ROOT/src/gpu/vk/vk_shaders.cpp" "$ROOT/src/gpu/hlsl.cpp" "$ROOT/src/gpu/ucode.cpp" \
    "$ROOT/src/live_switch.cpp" "$ROOT/src/log.cpp" -ldl -o "$OUT/vk_shaders_test"

g++ -O2 -std=c++20 -g -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_surface_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_shader.cpp" \
    "$ROOT/src/gpu/vk/vk_surface.cpp" "$ROOT/src/gpu/vk/vk_resolve.cpp" "$ROOT/src/gpu/vk/vk_texture.cpp" "$ROOT/src/live_switch.cpp" "$ROOT/src/log.cpp" -ldl -o "$OUT/vk_surface_test"

g++ -O2 -std=c++20 -g -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_draw_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_shader.cpp" \
    "$ROOT/src/gpu/vk/vk_texture.cpp" "$ROOT/src/gpu/vk/vk_state.cpp" "$ROOT/src/gpu/vk/vk_draw.cpp" \
    "$ROOT/src/gpu/texture.cpp" \
    "$ROOT/src/live_switch.cpp" "$ROOT/src/log.cpp" \
    -ldl -o "$OUT/vk_draw_test"

g++ -O2 -std=c++20 -g -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_pipelines_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_state.cpp" \
    "$ROOT/src/log.cpp" -ldl -o "$OUT/vk_pipelines_test"

g++ -O2 -std=c++20 -g -ffunction-sections -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_texture_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_shader.cpp" \
    "$ROOT/src/gpu/vk/vk_texture.cpp" "$ROOT/src/gpu/texture.cpp" -Wl,--gc-sections -ldl -o "$OUT/vk_texture_test"

# The block-compressed upload path: needs no shader, no game and no dxc, so it can be checked
# before the renderer ever uses it.
g++ -O2 -std=c++20 -g -ffunction-sections -I "$ROOT/src" -I "$INCLUDE" \
    "$ROOT/tools/vk_bc_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" "$ROOT/src/gpu/vk/vk_texture.cpp" \
    "$ROOT/src/gpu/texture.cpp" -Wl,--gc-sections -ldl -o "$OUT/vk_bc_test"

# The vertex-upload dedupe rule and the SPIR-V cache key need no device.
g++ -O2 -std=c++20 -I "$ROOT/src" -o "$OUT/vk_dedupe_test" "$ROOT/tools/vk_dedupe_test.cpp"
g++ -O2 -std=c++20 -I "$ROOT/src" -o "$OUT/vk_cache_key_test" "$ROOT/tools/vk_cache_key_test.cpp"
# The same dedupe rule against changing memory: a rewrite must be noticed, or stale geometry renders silently.
g++ -O2 -std=c++20 -I "$ROOT/src" -I "$INCLUDE" -o "$OUT/vk_dedupe_rewrite_test" \
    "$ROOT/tools/vk_dedupe_rewrite_test.cpp" "$ROOT/src/gpu/vk/vk_loader.cpp" "$ROOT/src/gpu/vk/vk_context.cpp" \
    "$ROOT/src/log.cpp" "$ROOT/src/live_switch.cpp" -ldl

echo "== running"
# Runs one test and requires BOTH a clean exit and its own success line, printing what it said: a test that
# exits 0 without reaching its assertions must not count as a pass.
run_check() {
    name=$1; needle=$2; shift 2
    out=$("$@") || { printf '%s\n' "$out"; echo "$name: FAILED (exit status)"; exit 1; }
    printf '%s\n' "$out"
    printf '%s' "$out" | grep -q "$needle" || { echo "$name: PASSED WITHOUT '$needle'"; exit 1; }
}
run_check vk_dedupe_test 'vk_dedupe_test: ok' "$OUT/vk_dedupe_test"
# The SPIR-V cache key must respond to the HLSL text, not only to the guest shader hash.
run_check vk_cache_key_test 'hash tracks the HLSL text' "$OUT/vk_cache_key_test"
FM2_VK_DEVICE=${FM2_VK_DEVICE:-RADV} run_check vk_dedupe_rewrite_test 'vk_dedupe_rewrite_test: ok' "$OUT/vk_dedupe_rewrite_test"
run_check vk_bc_test 'differ' "$OUT/vk_bc_test"
FM2_DXC=${FM2_DXC:-dxc} run_check vk_present_test 'pixels differ' "$OUT/vk_present_test" "$OUT/vk_present.png" "${FM2_SPV_DIR:-$OUT/spv}" "${FM2_SPV_COUNT:-200}"
run_check vk_surface_test 'surface cache: ok' "$OUT/vk_surface_test"
echo "wrote $OUT/vk_present.png"
