#pragma once
// Force-included into the recompiled code (see CMakeLists.txt).
#include <cstdint>

struct PPCContext;

#ifdef FM2_COVERAGE
// Which recompiled functions run (src/coverage.cpp). The generated PPC_FUNC_PROLOGUE() is defined after this
// header, unguarded, as a __builtin_assume, so that's what is redefined: each function records its name once.
void Fm2Cover(const char* func);
#define __builtin_assume(x)                \
    do                                     \
    {                                      \
        static bool fm2_seen;              \
        if (!fm2_seen)                     \
        {                                  \
            fm2_seen = true;               \
            Fm2Cover(__func__);            \
        }                                  \
    } while (0)
#endif

#ifdef FM2_STORE_LOG
// The lifted functions' check (src/decomp/lift.cpp): while a lifted version runs, the recompiled code it calls
// reads and writes a private overlay instead of guest memory (g_fm2Shadow); while the original runs, its stores
// are logged (g_fm2StoreLog). Vector loads and stores and dcbz don't come through here.
extern thread_local bool g_fm2StoreLog, g_fm2Shadow;
void Fm2LogStore(uint8_t* base, uint32_t addr, uint32_t size);
uint64_t Fm2ShadowLoad(const uint8_t* base, uint32_t addr, uint32_t size);
void Fm2ShadowStore(uint32_t addr, uint32_t size, uint64_t value);
#define PPC_LOAD_U8(x) (g_fm2Shadow ? uint8_t(Fm2ShadowLoad(base, (x), 1)) : *(volatile uint8_t*)(base + (x)))
#define PPC_LOAD_U16(x) (g_fm2Shadow ? uint16_t(Fm2ShadowLoad(base, (x), 2)) : __builtin_bswap16(*(volatile uint16_t*)(base + (x))))
#define PPC_LOAD_U32(x) (g_fm2Shadow ? uint32_t(Fm2ShadowLoad(base, (x), 4)) : __builtin_bswap32(*(volatile uint32_t*)(base + (x))))
#define PPC_LOAD_U64(x) (g_fm2Shadow ? uint64_t(Fm2ShadowLoad(base, (x), 8)) : __builtin_bswap64(*(volatile uint64_t*)(base + (x))))
#define FM2_STORE(x, y, n, T, swap)                                                                             \
    do                                                                                                          \
    {                                                                                                           \
        const uint32_t fm2_a = (x);                                                                             \
        const T fm2_v = T(y);                                                                                   \
        if (g_fm2Shadow)                                                                                        \
            Fm2ShadowStore(fm2_a, n, fm2_v);                                                                    \
        else                                                                                                    \
        {                                                                                                       \
            if (g_fm2StoreLog)                                                                                  \
                Fm2LogStore(base, fm2_a, n);                                                                    \
            *(volatile T*)(base + fm2_a) = swap(fm2_v);                                                         \
        }                                                                                                       \
    } while (0)
#define FM2_NOSWAP(v) (v)
#define PPC_STORE_U8(x, y) FM2_STORE(x, y, 1, uint8_t, FM2_NOSWAP)
#define PPC_STORE_U16(x, y) FM2_STORE(x, y, 2, uint16_t, __builtin_bswap16)
#define PPC_STORE_U32(x, y) FM2_STORE(x, y, 4, uint32_t, __builtin_bswap32)
#define PPC_STORE_U64(x, y) FM2_STORE(x, y, 8, uint64_t, __builtin_bswap64)
#endif

// MSR[EE] cleared -> take the global interrupt lock; set -> release it.
void PPCMsrChanged(PPCContext& ctx, uint64_t oldMsr);
#define PPC_MSR_CHANGED(ctx, oldMsr)                       \
    do                                                     \
    {                                                      \
        if (((oldMsr) ^ (ctx).msr) & 0x8000)               \
            PPCMsrChanged((ctx), (oldMsr));                \
    } while (0)

// IEEE half -> float (vupkd3d128 FLOAT16 variants).
inline float ppc_half_to_float(uint16_t h)
{
    uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (exp == 0)
    {
        if (man == 0) bits = sign;
        else
        {
            exp = 113;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
        }
    }
    else if (exp == 31) bits = sign | 0x7F800000 | (man << 13);
    else bits = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}

// float -> Xenos half (no inf/NaN: saturates to 0x7FFF, truncates).
inline uint16_t ppc_float_to_half(float f)
{
    uint32_t bits;
    __builtin_memcpy(&bits, &f, 4);
    uint16_t sign = uint16_t((bits >> 16) & 0x8000);
    uint32_t a = bits & 0x7FFFFFFF;
    if (a > 0x477FE000) return sign | 0x7FFF;      // > 65504 or NaN
    if (a < 0x38800000)                            // denormal / zero
    {
        uint32_t e = a >> 23;
        if (e < 103) return sign;
        return sign | uint16_t(((a & 0x7FFFFF) | 0x800000) >> (126 - e));
    }
    return sign | uint16_t((a - 0x38000000) >> 13);
}
