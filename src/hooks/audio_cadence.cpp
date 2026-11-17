// Engine sound updates at high frame rates (engine "warble" while revving).
//
// Every main-loop iteration the game records three per-frame audio updates as droppable commands into a packet
// that its audio thread (CAudioThread) replays:
//   CCarAudioDeferred::vf7      (TU 824B0478) frame dt + a snapshot of the car (rpm, throttle, ...), per car
//   CAudioManagerDeferred::vf5  (TU 821E3FA0) dt + "a game mode is running": FMOD's per-frame update
//   CAudioManagerDeferred::vf6  (TU 821E4080) dt + the camera/listener snapshot
// The audio thread sleeps 5 ms when it has nothing to do, and when 2 or more packets are waiting it replays them
// in "catch-up" mode, which skips the droppable commands (TU 8259EB78 -> 82478248(queue, 1)). At 60 fps (120
// iterations a second) that almost never happens; at a 360 cap (~700 iterations a second) it does most of the
// time: 70% of the car updates were dropped, so the engine model saw 11 s of every 37, its rpm moved in uneven
// jumps and the drivetrain spring that wobbles the rpm (some cars) ran on the wrong clock.
//
// Fixes:
// - The audio thread never skips them: catch-up replays run them too (FM2_AUDIO_CATCHUP_DROP=1: old behaviour).
// - The main thread sends each of them about FM2_AUDIO_UPDATE_HZ times a second at most (default 120, what a
//   60 fps cap gives; 0 = every frame) with the dt of the frames held back added up, so every update carries
//   the time that passed and the audio thread does about the work it does at 60 fps. A call with dt <= 0 (the
//   pause toggle pushes the pause state through with one) and a listener update that starts its one-shot event
//   always go straight through.
#include <atomic>
#include <cstdlib>
#include <string>
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
double UpdatePeriod()
{
    static const double period = [] {
        const char* v = getenv("FM2_AUDIO_UPDATE_HZ");
        const double hz = v ? atof(v) : 120.0;
        if (hz > 0)
            Log("audio cadence: game audio updates at most about %d a second", int(hz));
        else
            Log("audio cadence: game audio updates every frame");
        return hz > 0 ? 1.0 / hz : 0.0;
    }();
    return period;
}

struct Pending
{
    double dt = 0;    // the game's dt of the calls held back
    int64_t last = 0; // QPC of the last forwarded call
};

// Per deferred object (main thread only). Returns false while the call should be held back; true with dt
// replaced by the game's dt summed since the last forwarded call. The cadence is wall-clock time (the game's
// dt is whole milliseconds): a call goes through once 3/4 of the period has passed since the last one, so a
// game running at the update rate or slower (a 60 fps cap gives 120 a second) has every call go through.
bool Coalesce(std::unordered_map<uint32_t, Pending>& pending, uint32_t object, double& dt, bool force = false)
{
    static const double period = UpdatePeriod();
    if (period <= 0)
        return true;
    static const double ticksPerSecond = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    Pending& p = pending[object];
    p.dt += dt;
    if (!force && dt > 0 && p.last && double(now.QuadPart - p.last) < 0.75 * period * ticksPerSecond)
        return false;
    dt = p.dt;
    p.dt = 0;
    p.last = now.QuadPart;
    return true;
}

// Struct arguments passed by value: past their first 48 bytes (r5-r10) they sit in the caller's parameter
// area, r1 + 32 at entry.
uint8_t StructByte(PPCContext& ctx, uint8_t* base, uint32_t offset)
{
    return PPC_LOAD_U8(ctx.r1.u32 + 32 + offset);
}
}

void FmodTraceCarSend(PPCContext& ctx, uint8_t* base); // fmod_trace.cpp (FM2_FMOD_FREQ_TRACE)

// CCarAudioDeferred::vf7(this, dt in f1, car snapshot by value)
PPC_FUNC_IMPL(TU_IMP(824B0478));
PPC_FUNC(TU_SUB(824B0478))
{
    static std::unordered_map<uint32_t, Pending> pending;
    double dt = ctx.f1.f64;
    if (!Coalesce(pending, ctx.r3.u32, dt))
        return;
    ctx.f1.f64 = dt;
    FmodTraceCarSend(ctx, base);
    TU_IMP(824B0478)(ctx, base);
}

// CAudioManagerDeferred::vf5(this, dt in f1, "a game mode is running" in r5: a level, applied every update)
PPC_FUNC_IMPL(TU_IMP(821E3FA0));
PPC_FUNC(TU_SUB(821E3FA0))
{
    static std::unordered_map<uint32_t, Pending> pending;
    double dt = ctx.f1.f64;
    if (!Coalesce(pending, ctx.r3.u32, dt))
        return;
    ctx.f1.f64 = dt;
    TU_IMP(821E3FA0)(ctx, base);
}

// CAudioManagerDeferred::vf6(this, dt in f1, 0xE0-byte listener snapshot by value); snapshot byte 0xD0 set
// makes CAudioManager::vf6 (TU 821E5168) start an FMOD event, so such a call is never held back.
PPC_FUNC_IMPL(TU_IMP(821E4080));
PPC_FUNC(TU_SUB(821E4080))
{
    static std::unordered_map<uint32_t, Pending> pending;
    double dt = ctx.f1.f64;
    if (!Coalesce(pending, ctx.r3.u32, dt, StructByte(ctx, base, 0xD0) != 0))
        return;
    ctx.f1.f64 = dt;
    TU_IMP(821E4080)(ctx, base);
}

// The audio thread's loop body (CAudioThread in r3; its command queue is at +0x18).
static std::atomic<uint32_t> g_audioQueue{ 0 };
PPC_FUNC_IMPL(TU_IMP(8259EB78));
PPC_FUNC(TU_SUB(8259EB78))
{
    g_audioQueue.store(ctx.r3.u32 + 0x18, std::memory_order_relaxed);
    TU_IMP(8259EB78)(ctx, base);
}

// Replays the command packet at the head of a queue (any queue); r4 = 1 in catch-up mode skips the droppable
// commands. Only the audio thread's queue is changed.
PPC_FUNC_IMPL(TU_IMP(82478248));
PPC_FUNC(TU_SUB(82478248))
{
    static const bool drop = getenv("FM2_AUDIO_CATCHUP_DROP") != nullptr;
    if (!drop && ctx.r3.u32 == g_audioQueue.load(std::memory_order_relaxed))
        ctx.r4.u64 = 0;
    TU_IMP(82478248)(ctx, base);
}
#endif
