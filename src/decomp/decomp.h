// Decompiled game functions: shared pieces.
//
// Addresses here are TU4 addresses, as in the Ghidra project (tools/decomp/ghidra), not the disc addresses the
// rest of src/ uses: many TU functions have no disc counterpart. TU_SUB/TU_IMP build the names by token pasting,
// so tools/xexremap.py (which translates sub_8XXXXXXX literals from disc to TU) leaves them alone.
//
// DecompCheck runs a decompiled function against XenonRecomp's translation of the original when FM2_DECOMP_CHECK=1:
// the original runs first on the same registers, the memory regions the function may write are saved and put
// back, then the decompiled version runs, and the return registers and those regions are compared. Only for
// functions whose effects stay inside the given regions (no allocation, no globals, no calls with effects):
// anything else would happen twice.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "log.h"

#include "kernel/imports.h"

#define TU_SUB(a) sub_##a
#define TU_IMP(a) __imp__sub_##a

struct DecompRegion
{
    uint32_t addr, size;
};

bool DecompChecking();
bool DecompOff(); // FM2_DECOMP_OFF=1: the recompiled originals run instead (A/B tests, and a way out)

// The counts behind the log: a line for the first few differences of each function, and a summary at powers of two.
struct DecompStats
{
    const char* name;
    std::atomic<uint32_t> calls{ 0 }, wrong{ 0 };
};
void DecompReport(DecompStats& s, bool same, uint32_t r3, uint32_t original, uint32_t decompiled, const char* what);

// Turn 10's name hash (name_hash.cpp) on a guest string.
uint32_t DecompHashName(const uint8_t* base, uint32_t s);
// A HashedName start-up constant: *global = hash of the string (hashed_names.cpp; the replacements that call it are
// generated from the image by tools/decomp/gen_hashed_names.py).
void HashedNameInit(PPCContext& ctx, uint8_t* base, PPCFunc* original, uint32_t string, uint32_t global);

// original: runs the recompiled function on ctx; decompiled: the C++ version. regions(ctx) lists what may change.
template <typename Original, typename Decompiled, typename Regions>
void DecompCheck(DecompStats& stats, PPCContext& ctx, uint8_t* base, Original original, Decompiled decompiled, Regions regions)
{
    if (!DecompChecking())
    {
        if (DecompOff())
            original(ctx, base);
        else
            decompiled(ctx, base);
        return;
    }
    const std::vector<DecompRegion> where = regions(ctx);
    std::vector<std::vector<uint8_t>> before;
    for (const DecompRegion& r : where)
        before.emplace_back(base + r.addr, base + r.addr + r.size);

    PPCContext o = ctx;
    original(o, base);
    std::vector<std::vector<uint8_t>> after;
    for (size_t i = 0; i < where.size(); i++)
    {
        after.emplace_back(base + where[i].addr, base + where[i].addr + where[i].size);
        memcpy(base + where[i].addr, before[i].data(), before[i].size());
    }

    decompiled(ctx, base);
    // r3 as 32 bits: the game's ints and pointers are 32-bit, and the original's upper half is whatever its 64-bit
    // arithmetic left there (the name hash's is sign-extended), which no caller reads
    bool same = ctx.r3.u32 == o.r3.u32 && ctx.f1.u64 == o.f1.u64;
    const char* what = same ? "" : "return value";
    for (size_t i = 0; i < where.size() && same; i++)
        if (memcmp(base + where[i].addr, after[i].data(), after[i].size()) != 0)
            same = false, what = "memory";
    DecompReport(stats, same, ctx.r3.u32, o.r3.u32, ctx.r3.u32, what);
}
