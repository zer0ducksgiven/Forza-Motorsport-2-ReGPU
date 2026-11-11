#pragma once
// FNV-1a over the generated HLSL text.
//
// Part of the SPIR-V cache key: the guest shader hash alone does not change when the translator does. Header-only so
// a unit test can exercise the hash without linking the SPIR-V builder.
#include <cstdint>
#include <string>

inline uint64_t HlslContentHash(const std::string& hlsl)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : hlsl)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
