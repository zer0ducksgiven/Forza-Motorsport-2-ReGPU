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
// are logged (g_fm2StoreLog). The accesses tools/patch_mem.py routes through macros do the same.
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
// calls through pointers stay in the overlay, unless they reach one of the port's own C++ hooks
void Fm2ShadowIndirect(uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x)                  \
    do                                             \
    {                                              \
        const uint32_t fm2_t = (x);                \
        if (g_fm2Shadow)                           \
            Fm2ShadowIndirect(fm2_t);              \
        (PPC_LOOKUP_FUNC(base, fm2_t))(ctx, base); \
    } while (0)

// raw bytes, in memory order, for the accesses below
void Fm2ShadowRead(const uint8_t* base, uint32_t addr, void* out, uint32_t size);
void Fm2ShadowWrite(uint32_t addr, const void* in, uint32_t size);
#define FM2_GUEST(p) uint32_t((const uint8_t*)(p) - base)
#define FM2_VLOAD(p)                                         \
    ([&] {                                                   \
        simde__m128i fm2_v;                                  \
        if (g_fm2Shadow)                                     \
            Fm2ShadowRead(base, FM2_GUEST(p), &fm2_v, 16);   \
        else                                                 \
            fm2_v = simde_mm_load_si128(p);                  \
        return fm2_v;                                        \
    }())
#define FM2_VSTORE(p, v)                                     \
    do                                                       \
    {                                                        \
        simde__m128i* const fm2_p = (p);                     \
        const simde__m128i fm2_v = (v);                      \
        if (g_fm2Shadow)                                     \
            Fm2ShadowWrite(FM2_GUEST(fm2_p), &fm2_v, 16);    \
        else                                                 \
        {                                                    \
            if (g_fm2StoreLog)                               \
                Fm2LogStore(base, FM2_GUEST(fm2_p), 16);     \
            simde_mm_store_si128(fm2_p, fm2_v);              \
        }                                                    \
    } while (0)
#define FM2_MEMSET(p, c, n)                                                 \
    do                                                                      \
    {                                                                       \
        uint8_t* const fm2_p = (uint8_t*)(p);                               \
        if (g_fm2Shadow)                                                    \
        {                                                                   \
            uint8_t fm2_z[128];                                             \
            memset(fm2_z, (c), sizeof(fm2_z));                              \
            for (uint32_t fm2_i = 0; fm2_i < uint32_t(n); fm2_i += 128)     \
                Fm2ShadowWrite(FM2_GUEST(fm2_p) + fm2_i, fm2_z, 128);       \
        }                                                                   \
        else                                                                \
        {                                                                   \
            if (g_fm2StoreLog)                                              \
                for (uint32_t fm2_i = 0; fm2_i < uint32_t(n); fm2_i += 16)  \
                    Fm2LogStore(base, FM2_GUEST(fm2_p) + fm2_i, 16);        \
            memset(fm2_p, (c), (n));                                        \
        }                                                                   \
    } while (0)
template <typename T> inline T Fm2Reserve(const uint8_t* base, T* p)
{
    T v;
    if (g_fm2Shadow)
        Fm2ShadowRead(base, FM2_GUEST(p), &v, sizeof(T));
    else
        v = *p;
    return v;
}
template <typename T, typename A, typename B> inline bool Fm2Cas(uint8_t* base, T* p, A expected, B desired)
{
    if (g_fm2Shadow)
    {
        T cur;
        Fm2ShadowRead(base, FM2_GUEST(p), &cur, sizeof(T));
        if (cur != T(expected))
            return false;
        const T d = T(desired);
        Fm2ShadowWrite(FM2_GUEST(p), &d, sizeof(T));
        return true;
    }
    if (g_fm2StoreLog)
        Fm2LogStore(base, FM2_GUEST(p), sizeof(T));
    return __sync_bool_compare_and_swap(p, T(expected), T(desired));
}
#define FM2_RESERVE32(p) Fm2Reserve(base, (p))
#define FM2_RESERVE64(p) Fm2Reserve(base, (p))
#define FM2_CAS(p, expected, desired) Fm2Cas(base, (p), (expected), (desired))
// setjmp/longjmp (Lua's error handling): a lifted run may jump only to a setjmp of its own; a longjmp to one from
// before the check would leave the check mid-way, so the run is abandoned there instead. setjmp writes the host
// registers into the guest buffer directly, which is on the guest stack below the caller, where the original's
// own setjmp writes next
void Fm2NoteSetjmp(uint32_t buffer);
void Fm2CheckLongjmp(uint32_t buffer);
#define FM2_SETJMP(buf) (Fm2NoteSetjmp(FM2_GUEST(&(buf))), setjmp(buf))
#define FM2_LONGJMP(buf, v)                     do                                          {                                               Fm2CheckLongjmp(FM2_GUEST(&(buf)));         longjmp((buf), (v));                    } while (0)
#else
// tools/patch_mem.py's macros, as XenonRecomp wrote them
#define FM2_VLOAD(p) simde_mm_load_si128(p)
#define FM2_VSTORE(p, v) simde_mm_store_si128((p), (v))
#define FM2_MEMSET(p, c, n) memset((p), (c), (n))
#define FM2_RESERVE32(p) (*(p))
#define FM2_RESERVE64(p) (*(p))
#define FM2_CAS(p, expected, desired) __sync_bool_compare_and_swap((p), (expected), (desired))
#define FM2_SETJMP(buf) setjmp(buf)
#define FM2_LONGJMP(buf, v) longjmp((buf), (v))
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
