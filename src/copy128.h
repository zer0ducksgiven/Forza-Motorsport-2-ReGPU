#pragma once
// memcpy without 256-bit stores, for hot copies on the command processor. The static CRT's
// memcpy moves 33 bytes to 8 KB with 32-byte AVX loads and stores whenever the CPU has AVX
// (crt/src/x64/memcpy.asm); on the FX-8350 (Piledriver) a 256-bit store costs ~17 cycles
// against ~1 for a 128-bit one. Same bytes either way. The buffers must not overlap.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>

// AMD family 15h (Bulldozer, Piledriver): the CPUs whose 256-bit stores are slow. Elsewhere
// the CRT's AVX copy is as fast or faster, so callers keep memcpy there.
inline bool AvxStoresSlow()
{
    int r[4];
    __cpuid(r, 0);
    const bool amd = r[1] == 0x68747541 && r[3] == 0x69746E65 && r[2] == 0x444D4163; // "AuthenticAMD"
    if (!amd || r[0] < 1)
        return false;
    __cpuid(r, 1);
    const uint32_t base = (uint32_t(r[0]) >> 8) & 0xF;
    const uint32_t family = base == 0xF ? base + ((uint32_t(r[0]) >> 20) & 0xFF) : base;
    return family == 0x15;
}

inline void Copy128(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (n <= 32)
    {
        memcpy(d, s, n); // the CRT uses general and 16-byte registers up to 32 bytes
        return;
    }
    const __m128i last = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + n - 16));
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 16));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 32));
        const __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 48));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 16), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 32), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 48), e);
    }
    // The rest (under 64 bytes) without a loop, which clang could turn back into a memcpy call;
    // `last` covers the final 16.
    const size_t rest = n - i;
    if (rest > 16)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i)));
    if (rest > 32)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 16), _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 16)));
    if (rest > 48)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 32), _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 32)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + n - 16), last);
}
