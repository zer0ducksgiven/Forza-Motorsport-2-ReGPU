#pragma once
// The backend's shader service: microcode -> translated HLSL (hlsl.cpp) -> SPIR-V ->
// VkShaderModule, cached on disk by the final HLSL and compilation options, so a translation
// change cannot reuse stale code and an unchanged draw costs a lookup and a cache read.
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "gpu/vk/vk_context.h"

namespace vk
{
struct VkShader
{
    VkShaderModule module = VK_NULL_HANDLE;
    std::string hlsl;        // kept for the renderer's per-shader edits (car body squash, dirt seam fix)
    uint32_t constantsUsed = 0;
    uint32_t usedTextureSlots = 0;
    std::vector<uint32_t> vertexFetchLayout;     // flat uint4s for vc.vfl
    std::vector<uint32_t> vertexFetchConstants;  // which vf[] slots the shader reads
    bool pixel = false;
    bool ok = false;
    std::string error;
};

struct ShaderSet
{
    std::unordered_map<uint64_t, VkShader> shaders;
    std::filesystem::path cacheDir;
    uint32_t compiled = 0, failed = 0, cached = 0; // modules made; failures; of the made, read from the SPIR-V cache
    uint32_t fromCompiler = 0;                      // of the made, compiled by dxc on the render thread
};

void ShaderSetDestroy(Context& context, ShaderSet& set);

// Returns the shader for `hash`, translating and compiling it if this is the first time.
// The microcode must be the bytes the hash was taken over (Fnv over the used dwords, xor the
// shader type) — the renderer's cache key.
const VkShader* ShaderGet(Context& context, ShaderSet& set, uint64_t hash, const uint32_t* microcodeBE, uint32_t dwords,
    bool pixel);

// Starts the background warm-up: low-priority threads that translate and compile the game's shaders into set.cacheDir
// ahead of their first use (see vk_shaders.cpp). ShaderGet stays synchronous. FM2_VK_NO_SHADER_WARM=1 turns it off,
// FM2_VK_SHADER_WARM_THREADS sets the thread count. Call once, after cacheDir is set.
void ShaderWarmStart(const ShaderSet& set);
// The static set's progress, for the first-boot screen: true while it is still being listed or compiled. `compiled`
// counts shaders that really went through the compiler (zero on a boot whose cache is complete).
bool ShaderWarmProgress(uint32_t& done, uint32_t& total, uint32_t& compiled);
// The warm-up's totals, for the compile overlay: shaders compiled, found in the cache, and waiting or being compiled now.
void ShaderWarmStats(uint32_t& compiled, uint32_t& cached, uint32_t& pending);
}
