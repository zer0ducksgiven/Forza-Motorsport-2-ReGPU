// The UI's per-frame "onUpdate" at the console's rate. Each scene update (UI::SceneManager::Update, TU 825FCB58)
// ticks every active presentation (Anark Gameface) through TU 827FBBF0, which hands each one to its event
// dispatcher (TU 827FA6B8 -> 8280B8C8) with mode 1: drain the queued events, then append the manager's onUpdate
// event and dispatch it to every behaviour, which runs all the HUD and menu Lua onUpdate handlers. That is a fixed
// amount of Lua per rendered frame: 60 times a second on the console, 150-300 here, about 5% of the game's main
// thread (guest thread 3) in a race.
//
// Default: the onUpdate is delivered 60 times a second (paced on the guest timebase, decided once per pass so
// every presentation agrees). Between them the pass still runs with mode 2, which drains and dispatches the
// queued events (input, one-off events) and runs the presentations' timelines and drawing as before, only
// without onUpdate. Below 60 fps nothing changes. FM2_NO_UI_UPDATE_PACING=1 (live switch): every frame.
#include <cstdint>

#include "kernel/imports.h"
#include "live_switch.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

static constexpr uint64_t kPeriod = 49875000 / 60; // guest timebase ticks between onUpdates
static thread_local bool t_skipUpdate = false;     // this pass delivers no onUpdate

// The pass over the active presentations: decides whether this one delivers onUpdate.
PPC_FUNC_IMPL(TU_IMP(827FBBF0));
PPC_FUNC(TU_SUB(827FBBF0))
{
    static LiveSwitch off("FM2_NO_UI_UPDATE_PACING");
    static uint64_t next = 0;
    bool skip = false;
    if (!off)
    {
        const uint64_t now = Fm2Timebase();
        if (now < next)
            skip = true;
        else
            next = now - next < kPeriod ? next + kPeriod : now + kPeriod; // keeps 60 a second; resyncs after a stall
    }
    t_skipUpdate = skip;
    TU_IMP(827FBBF0)(ctx, base);
    t_skipUpdate = false;
}

// A presentation's event dispatch: mode 1 (the per-frame pass) without its onUpdate becomes mode 2.
PPC_FUNC_IMPL(TU_IMP(827FA6B8));
PPC_FUNC(TU_SUB(827FA6B8))
{
    if (t_skipUpdate && ctx.r6.u32 == 1)
        ctx.r6.u64 = 2;
    TU_IMP(827FA6B8)(ctx, base);
}
#endif
