// FM2_FMOD_FREQ_TRACE=<file>: engine-sound timing trace (engine warble diagnostics). Binary records
// {uint64 qpc, uint32 kind, uint32 object, double a, double b}:
//   kind 1: CCarAudioDeferred::vf7 (TU 824B0478, main thread) as forwarded by audio_cadence.cpp: a = dt, b = rpm
//   kind 2: CCarAudio::vf7 (TU 824B30A0, the game's audio thread, replaying kind 1): a = dt, b = rpm
//   kind 3: FMOD resampler (TU 826C3940, per voice per mix block), only when a voice's phase increment changes:
//           a = increment (frequency / output rate), b = resampler calls of that voice so far
//   kind 4: the engine loops' update (TU 824B13B0, audio thread): a = rpm after the drivetrain spring
//           (params +0x70), b = normalised rpm (params +0x74)
//   kind 5: CAudioEmission::setVolume (TU 825A1690, any sound's loop voice): object = emission, a = volume
// The car snapshot is passed by value: past its first 48 bytes (r5-r10) it sits in the caller's parameter
// area, r1 + 32 at entry, so rpm (snapshot +0x70) is at r1 + 32 + 0x70.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

namespace
{
struct Record
{
    uint64_t qpc;
    uint32_t kind, object;
    double a, b;
};
std::mutex g_lock;
std::unordered_map<uint32_t, uint64_t> g_last;
std::unordered_map<uint32_t, uint32_t> g_calls;

FILE* File()
{
    static FILE* file = [] {
        const char* path = getenv("FM2_FMOD_FREQ_TRACE");
        FILE* f = path ? fopen(path, "wb") : nullptr;
        if (f)
        {
            LARGE_INTEGER hz;
            QueryPerformanceFrequency(&hz);
            Log("fmod trace: engine audio timing -> %s (qpc %lld Hz)", path, hz.QuadPart);
        }
        return f;
    }();
    return file;
}

void Write(uint32_t kind, uint32_t object, double a, double b)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const Record r{ uint64_t(now.QuadPart), kind, object, a, b };
    std::lock_guard<std::mutex> hold(g_lock);
    fwrite(&r, sizeof r, 1, File());
}

float SnapshotRpm(PPCContext& ctx, uint8_t* base)
{
    const uint32_t bits = PPC_LOAD_U32(ctx.r1.u32 + 32 + 0x70);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
}

// Called by audio_cadence.cpp's hook of TU 824B0478 for each car update it forwards.
void FmodTraceCarSend(PPCContext& ctx, uint8_t* base)
{
    if (File())
        Write(1, ctx.r3.u32, ctx.f1.f64, SnapshotRpm(ctx, base));
}

PPC_FUNC_IMPL(TU_IMP(824B30A0));
PPC_FUNC(TU_SUB(824B30A0))
{
    if (File())
        Write(2, ctx.r3.u32, ctx.f1.f64, SnapshotRpm(ctx, base));
    TU_IMP(824B30A0)(ctx, base);
}

static float LoadF32(uint8_t* base, uint32_t address)
{
    const uint32_t bits = PPC_LOAD_U32(address);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

PPC_FUNC_IMPL(TU_IMP(824B13B0));
PPC_FUNC(TU_SUB(824B13B0))
{
    if (File())
        Write(4, ctx.r3.u32, LoadF32(base, ctx.r4.u32 + 0x70), LoadF32(base, ctx.r4.u32 + 0x74));
    TU_IMP(824B13B0)(ctx, base);
}

PPC_FUNC_IMPL(TU_IMP(825A1690));
PPC_FUNC(TU_SUB(825A1690))
{
    if (File())
        Write(5, ctx.r3.u32, ctx.f1.f64, 0);
    TU_IMP(825A1690)(ctx, base);
}

PPC_FUNC_IMPL(TU_IMP(826C3940));
PPC_FUNC(TU_SUB(826C3940))
{
    if (File())
    {
        const uint32_t unit = ctx.r3.u32;
        const uint64_t inc = PPC_LOAD_U64(unit + 272);
        bool changed;
        uint32_t calls;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            calls = ++g_calls[unit];
            auto it = g_last.find(unit);
            changed = it == g_last.end() || it->second != inc;
            if (changed)
                g_last[unit] = inc;
        }
        if (changed)
            Write(3, unit, double(inc) / 4294967296.0, calls);
    }
    TU_IMP(826C3940)(ctx, base);
}
#endif
