// Lifted functions: game functions turned into C++ by tools/decomp/lift.py from Ghidra's decompiled C, into
// out/lift (never committed: it's the game's code). This is what that code builds against: guest memory loads and
// stores (big-endian, 32-bit addresses), and LiftRun, which runs a lifted function or, with FM2_DECOMP_CHECK=1 in
// an FM2_STORE_LOG build, checks it against the recompiled original.
//
// The check runs the lifted version first against an overlay: its stores, and those of the recompiled code it
// calls, go to a private copy, and its loads read that copy first, so guest memory isn't touched. Then the original
// runs for real, its stores logged, and what each left (outside the dead stack around r1) and the return register
// are compared. The game always gets the original's results, so a check run plays like the original. Results go
// to out/lift_check.tsv, which lift.py reads to keep only exact functions. Our own C++ hooks and imports bypass the
// overlay: a run that reaches one is abandoned (lift.cpp), and lift.py doesn't lift functions that call hooks.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "kernel/imports.h"

extern thread_local bool g_fm2StoreLog, g_fm2Shadow;
void Fm2LogStore(uint8_t* base, uint32_t addr, uint32_t size);
uint64_t Fm2ShadowLoad(const uint8_t* base, uint32_t addr, uint32_t size);
void Fm2ShadowStore(uint32_t addr, uint32_t size, uint64_t value);

namespace lift
{
template <typename T> inline T Load(const uint8_t* base, uint32_t a)
{
    T v;
    if (g_fm2Shadow)
    {
        const uint64_t raw = Fm2ShadowLoad(base, a, sizeof(T));
        if constexpr (sizeof(T) == 8)
            memcpy(&v, &raw, 8);
        else
        {
            const auto small = static_cast<std::conditional_t<sizeof(T) == 4, uint32_t, std::conditional_t<sizeof(T) == 2, uint16_t, uint8_t>>>(raw);
            memcpy(&v, &small, sizeof(T));
        }
        return v;
    }
    // volatile, as the recompiled code's are: guest memory is shared with other threads and the GPU, and a loop
    // waiting on a flag must read it each time
    using U = std::conditional_t<sizeof(T) == 8, uint64_t, std::conditional_t<sizeof(T) == 4, uint32_t, std::conditional_t<sizeof(T) == 2, uint16_t, uint8_t>>>;
    U u = *(const volatile U*)(base + a);
    if constexpr (sizeof(T) == 2)
        u = __builtin_bswap16(u);
    else if constexpr (sizeof(T) == 4)
        u = __builtin_bswap32(u);
    else if constexpr (sizeof(T) == 8)
        u = __builtin_bswap64(u);
    memcpy(&v, &u, sizeof(T));
    return v;
}

template <typename T> inline void Store(uint8_t* base, uint32_t a, T v)
{
    if (g_fm2Shadow)
    {
        uint64_t raw = 0;
        memcpy(&raw, &v, sizeof(T));
        Fm2ShadowStore(a, sizeof(T), raw);
        return;
    }
    if (g_fm2StoreLog)
        Fm2LogStore(base, a, sizeof(T));
    using U = std::conditional_t<sizeof(T) == 8, uint64_t, std::conditional_t<sizeof(T) == 4, uint32_t, std::conditional_t<sizeof(T) == 2, uint16_t, uint8_t>>>;
    U u;
    memcpy(&u, &v, sizeof(T));
    if constexpr (sizeof(T) == 2)
        u = __builtin_bswap16(u);
    else if constexpr (sizeof(T) == 4)
        u = __builtin_bswap32(u);
    else if constexpr (sizeof(T) == 8)
        u = __builtin_bswap64(u);
    *(volatile U*)(base + a) = u;
}

// Ghidra's p-code helpers as they appear in its C
inline uint32_t Lzcount32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
inline uint32_t Lzcount64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
template <typename T> inline uint32_t Popcount(T v) { return __builtin_popcountll(uint64_t(v)); }
} // namespace lift

enum class LiftReturn : uint8_t
{
    None,
    Int,
    Float,
};

struct LiftStats
{
    uint32_t address;
    LiftReturn ret;
    std::atomic<uint32_t> calls{ 0 }, wrong{ 0 }, voidR3{ 0 };
    std::atomic<uint8_t> what{ 0 }; // first difference: 1 memory, 2 return register
    std::atomic<bool> big{ false }; // too much written to check
    std::atomic<bool> escaped{ false }; // reached an import or a hook, which a check can't follow
    LiftStats* next = nullptr;
    LiftStats(uint32_t a, LiftReturn r);
};

// the functions a check's lifted run can't follow a call through a pointer into (lift.py lists them): the port's
// own C++ hooks, vector and block stores (they bypass the overlay), and their callers
void LiftRegisterHooked(const uint32_t* addresses, size_t n);

void LiftRun(LiftStats& s, PPCContext& ctx, uint8_t* base, PPCFunc* original, PPCFunc* lifted);
