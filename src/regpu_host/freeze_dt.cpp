// The world truly stopped while FM2_FREEZE_TIME is on (the benchmark's frozen grid, kernel/runtime.cpp holds TickCount).
//
// The main loop (base sub_822172E8; TU FUN_82229188, docs/research/ai-framerate.md) turns TickCount's millisecond delta
// into dt and floors it at 1e-4 s, so with TickCount held every iteration still moved the world by 0.1 ms: at the
// vblank rate (720 Hz uncapped) the frozen grid crept on by ~40-70 ms of game time a second (race clock 1.05 -> 1.58 s
// between two screenshots). The loop skips the floor (and the 16/17 ms smoothing) when sub_821E6428 says true (the
// research's unconfirmed predicate FUN_821f64f0), passing the raw dt: 0 with TickCount held. So while frozen it says
// true, but only to the main loop's call (lr 8221749C); its 14 other callers are untouched.
// Title update: main loop 82229188, predicate 821F64F0 called from 82229394 (lr 82229398), the game type update
// (82353718) from the bctrl at 82229590 (lr 82229594): the same instructions around both calls as on the disc. The TU
// addresses are pasted / digit-separated so tools/xexremap.py leaves them be.
#include <cstdlib>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

#if FM2_TITLE_UPDATE
#define FREEZE_SUB(base, tu) sub_##tu
#define FREEZE_IMP(base, tu) __imp__sub_##tu
constexpr uint32_t kPredicateLr = 0x8222'9398, kUpdateLr = 0x8222'9594;
#else
#define FREEZE_SUB(base, tu) sub_##base
#define FREEZE_IMP(base, tu) __imp__sub_##base
constexpr uint32_t kPredicateLr = 0x8221'749C, kUpdateLr = 0x8221'7694; // (separated: no remap)
#endif

PPC_FUNC_IMPL(FREEZE_IMP(821E6428, 821F64F0));
PPC_FUNC(FREEZE_SUB(821E6428, 821F64F0))
{
    static LiveSwitch freeze("FM2_FREEZE_TIME");
    if (ctx.lr == kPredicateLr)
    {
        static uint32_t last = 0;
        const uint32_t now = ctx.r31.u32 ? LoadBE32(ctx.r31.u32 + 8) : 0;
        if (now != last)
        {
            last = now;
            const uint32_t gameType = now;
            Log("freeze: main loop %08X, game type %08X, its update (vtable +40) %08X", ctx.r31.u32, gameType,
                gameType ? LoadBE32(LoadBE32(gameType) + 40) : 0);
        }
    }
    if (freeze && ctx.lr == kPredicateLr)
    {
        ctx.r3.u64 = 1;
        return;
    }
    FREEZE_IMP(821E6428, 821F64F0)(ctx, base);
}

// The race game type's update (base sub_8233A920, vtable +40 of the main loop's game type, called with dt in f1;
// research: CGameType::vf10, TU 82353718): it scales dt and clamps it to at least 1e-5 s, so even dt 0 moved the world
// ~3 ms a second while frozen. Frozen, it isn't run (the frame is still drawn: that happens after it in the main loop)
// and answers what it last answered. FM2_FREEZE_SKIP_UPDATE=0: run as usual (the slow creep, A/B).
PPC_FUNC_IMPL(FREEZE_IMP(8233A920, 82353718));
PPC_FUNC(FREEZE_SUB(8233A920, 82353718))
{
    static LiveSwitch freeze("FM2_FREEZE_TIME");
    static const bool skip = !(getenv("FM2_FREEZE_SKIP_UPDATE") && atoi(getenv("FM2_FREEZE_SKIP_UPDATE")) == 0);
    static uint32_t lastAnswer = 1;
    if (freeze && skip && ctx.lr == kUpdateLr)
    {
        ctx.r3.u64 = lastAnswer;
        return;
    }
    FREEZE_IMP(8233A920, 82353718)(ctx, base);
    if (ctx.lr == kUpdateLr || true)
    {
        static int logged = 0;
        if (ctx.r3.u32 != lastAnswer && logged++ < 8)
            Log("freeze: game update answered %u", ctx.r3.u32);
        lastAnswer = ctx.r3.u32;
    }
}
