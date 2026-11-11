#pragma once
// The vertex-upload dedupe rule, shared by the renderer and tools/vk_dedupe_test.cpp so the two cannot drift apart.
//
// Sampled mode compares 64 bytes at the head, and optionally the tail and middle, instead of every byte: a full
// memcmp of each vertex buffer costs milliseconds a frame in guest-memory cache misses. The known limitation,
// asserted in the test, is that a rewrite confined to an unsampled byte is missed.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vk
{
// True when `now` can be served from the earlier copy at `before`. `spans` is 1 (head only), 2 (head+tail) or 3 (all).
inline bool UploadUnchanged(const uint8_t* now, const uint8_t* before, size_t bytes, bool sampled, long spans)
{
    if (!sampled || bytes <= 128)
        return ::memcmp(now, before, bytes) == 0;
    const size_t span = 64;
    if (::memcmp(now, before, span) != 0)
        return false;
    if (spans >= 2 && ::memcmp(now + bytes - span, before + bytes - span, span) != 0)
        return false;
    if (spans >= 3 && ::memcmp(now + bytes / 2, before + bytes / 2, span) != 0)
        return false;
    return true;
}
}
