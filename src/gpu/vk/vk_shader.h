#pragma once
// HLSL to SPIR-V for the Vulkan backend, through dxc: dxcompiler.dll in process on Windows when it is available,
// otherwise the dxc executable (FM2_DXC, the bundled dxc.exe, or PATH). Results are cached on disk under a name
// that includes a hash of the final HLSL.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vk
{
// pixel selects ps_6_0/vs_6_0. cacheDir may be empty to skip the disk cache.
bool CompileHlsl(const std::string& hlsl, bool pixel, uint64_t key, const std::filesystem::path& cacheDir,
    std::vector<uint32_t>& spirv, std::string* error = nullptr, bool disableOptimizations = false);
// Whether CompileHlsl would find this shader in the disk cache (no read, no compile).
bool SpirvCached(const std::string& hlsl, uint64_t key, const std::filesystem::path& cacheDir);

// Same, but for a shader that must always be compiled (the renderer's own blit passes).
inline uint64_t HashSource(const std::string& text)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : text)
        h = (h ^ c) * 0x100000001B3ull;
    return h;
}
}
