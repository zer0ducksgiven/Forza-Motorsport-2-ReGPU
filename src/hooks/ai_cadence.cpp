// AI updates at the console's rate (AI cars turning and reacting strangely above 60 fps).
//
// The AI system update (TU 82485EA0, called once per main-loop iteration from 8223D470) gets the frame's dt, but
// a lot of what it runs counts updates rather than time:
//   - contact accumulators CAICar+0x8F0..0x8FC gain 1 per update while the contact flags 0x894..0x897 are set;
//     "stuck" (over 6 at low speed) starts crash recovery: 100 ms of contact at 60 updates a second, 9 ms at 700
//   - the MetaAI state machine's frame counter (thresholds 31, 301, 601, 1801 frames)
//   - passing patience (CPassingDriver+0x70, 5 per update against 1000) and the brake ramp (25% per update)
// On the 360 the loop ran 60 times a second. On PC it runs at twice the fps cap (the fake vblank), so ~120 at a 60
// cap and ~700 at 360, and AI cars went into crash recovery mid-race after a touch, committed to and gave up
// passes too fast and slammed the brakes (docs/research/ai-framerate.md).
//
// Fix: the AI system update runs about FM2_AI_HZ times a second (default 60; 0 = every frame) of game time, with
// the dt of the frames held back added up. Between updates the cars keep the controls the AI last set
// (SetControls writes car+0x11C0, the physics step latches them every substep), as on the console. The event
// bytes the car refreshes every frame (0x878..0x87C state-change events, 0x894..0x8A3 contact flags) are OR-ed
// over the held-back frames and written back before an update, so none is missed. The AI's one-shot requests
// (0x914..0x91A: read after the update and in the next frame's pre-physics) are cleared on the first frame it
// doesn't run, where the update itself would clear them, so each is seen once, as at 60. FM2_AI_EVERY_FRAME (a
// live switch) restores the old behaviour.
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

namespace
{
constexpr int kMaxCars = 16;
constexpr uint32_t kEventsA = 0x878, kEventsALen = 5;   // 0x878..0x87C
constexpr uint32_t kEventsB = 0x894, kEventsBLen = 16;  // 0x894..0x8A3 (not 0x8A4: state)
constexpr uint32_t kRequests = 0x914, kRequestsLen = 7; // 0x914..0x91A

double Period()
{
    static const double period = [] {
        const char* v = getenv("FM2_AI_HZ");
        const double hz = v ? atof(v) : 60.0;
        if (hz > 0)
            Log("ai cadence: AI updates about %d times a second", int(hz));
        else
            Log("ai cadence: AI updates every frame");
        return hz > 0 ? 1.0 / hz : 0.0;
    }();
    return period;
}

struct State
{
    bool valid = false;
    uint32_t system = 0;
    uint32_t count = 0;
    double clock = 0; // game time, the sum of the dt of every call
    double next = 0;  // game time of the next update
    double held = 0;  // dt of the calls since the last update
    double raceTime = 0;
    uint32_t cars[kMaxCars] = {};
    uint8_t events[kMaxCars][kEventsALen + kEventsBLen] = {};
} g;
}

PPC_FUNC_IMPL(TU_IMP(82485EA0));

// AI system update: r3 = the AI system (+0x10 car count, +0x14 CAICar* array), r4 = &{double race time;
// float dt at +8}.
PPC_FUNC(TU_SUB(82485EA0))
{
    static LiveSwitch everyFrame("FM2_AI_EVERY_FRAME");
    const double period = Period();
    const uint32_t system = ctx.r3.u32, args = ctx.r4.u32;
    if (period <= 0 || everyFrame || !system || !args)
    {
        g.valid = false;
        TU_IMP(82485EA0)(ctx, base);
        return;
    }
    const uint32_t count = std::min<uint32_t>(PPC_LOAD_U32(system + 0x10), kMaxCars);
    const float frameDt = std::bit_cast<float>(PPC_LOAD_U32(args + 8));
    const double dt = std::max(0.0f, frameDt);
    const double raceTime = std::bit_cast<double>(PPC_LOAD_U64(args));
    bool cars = g.valid && g.system == system && g.count == count && raceTime >= g.raceTime;
    g.raceTime = raceTime;
    for (uint32_t i = 0; cars && i < count; i++)
        cars = g.cars[i] == PPC_LOAD_U32(system + 0x14 + 4 * i);
    if (!cars)
    {
        // A new race, a changed field or race time going back: update now and start the cadence from here.
        g = State{};
        g.valid = true;
        g.raceTime = raceTime;
        g.system = system;
        g.count = count;
        for (uint32_t i = 0; i < count; i++)
            g.cars[i] = PPC_LOAD_U32(system + 0x14 + 4 * i);
        g.next = period;
        TU_IMP(82485EA0)(ctx, base);
        return;
    }
    g.clock += dt;
    g.held += dt;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t car = g.cars[i];
        if (!car)
            continue;
        uint8_t* events = g.events[i];
        for (uint32_t k = 0; k < kEventsALen; k++)
            events[k] |= PPC_LOAD_U8(car + kEventsA + k);
        for (uint32_t k = 0; k < kEventsBLen; k++)
            events[kEventsALen + k] |= PPC_LOAD_U8(car + kEventsB + k);
    }
    if (g.clock + dt / 2 < g.next)
    {
        // Held back: the AI's requests from its last update have been seen once already.
        for (uint32_t i = 0; i < count; i++)
            if (const uint32_t car = g.cars[i])
                for (uint32_t k = 0; k < kRequestsLen; k++)
                    PPC_STORE_U8(car + kRequests + k, 0);
        return;
    }
    g.next += period;
    if (g.next < g.clock) // fell behind (a long frame): start again from now
        g.next = g.clock + period;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t car = g.cars[i];
        if (!car)
            continue;
        uint8_t* events = g.events[i];
        for (uint32_t k = 0; k < kEventsALen; k++)
            PPC_STORE_U8(car + kEventsA + k, events[k]);
        for (uint32_t k = 0; k < kEventsBLen; k++)
            PPC_STORE_U8(car + kEventsB + k, events[kEventsALen + k]);
        memset(events, 0, kEventsALen + kEventsBLen);
    }
    const float held = float(g.held);
    g.held = 0;
    PPC_STORE_U32(args + 8, std::bit_cast<uint32_t>(held));
    TU_IMP(82485EA0)(ctx, base);
    PPC_STORE_U32(args + 8, std::bit_cast<uint32_t>(frameDt));
}
#endif
