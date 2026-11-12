// The game's Lua garbage collection. The game runs about 30 Lua states (one per UI scene) and stops Lua's own
// collector in each; its scene update (TU 825FCB58) then steps each state's collector until a time budget is
// spent: 74812.5 timebase ticks per call in all (the double at 82118900), 1.5 ms at the console's 49.875 MHz,
// split among the states. Being time and not work, it's 1.5 ms whatever the frame rate: 9% of a 60 fps frame
// on the console, a third or more of the game's main thread (guest thread 3) at 180-300 fps here, most of it
// stepping collections that have nothing left to collect.
//
// Default: the budget scales with the time since the last scene update, so the collector gets the console's
// share of each second (1.5 ms per 1/60 s) at any frame rate: 0.3 ms a frame at 300 fps. Capped at the
// console's 1.5 ms (slow frames and loads get no more than before), floored at 5% of it.
// FM2_LUA_GC_BUDGET=<fraction of the console's budget> (e.g. 0.2, or 1 = console) fixes it instead.
// FM2_LUA_GC_LOG=1 logs the Lua memory of all states every 10 s, to see that the collector keeps up
// (its global_State matches Lua 5.1's: totalbytes at +0x44).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address
// built from halves: a 0x82xxxxxx literal here would be translated as a disc address by tools/xexremap.py
static const uint32_t kGcBudget = (0x8211u << 16) | 0x8900u;
static constexpr double kConsoleTicks = 74812.5;     // 1.5 ms of the 49.875 MHz timebase
static constexpr double kConsoleFrame = 49875000.0 / 60.0; // ticks in a 60 fps frame

static void StoreBudget(uint8_t* base, double ticks)
{
    uint64_t bits;
    memcpy(&bits, &ticks, 8);
    PPC_STORE_U64(kGcBudget, bits);
}

// The scene update: sets this call's budget, then runs.
PPC_FUNC_IMPL(TU_IMP(825FCB58));
PPC_FUNC(TU_SUB(825FCB58))
{
    static const char* fixed = getenv("FM2_LUA_GC_BUDGET");
    static const bool once = [base] {
        if (fixed)
        {
            const double ticks = kConsoleTicks * atof(fixed);
            StoreBudget(base, ticks);
            Log("lua gc: budget fixed at %.0f ticks a call (%.2f ms; the console's is 1.5 ms)", ticks, ticks / 49875.0);
        }
        else
            Log("lua gc: budget scales with frame time (the console's 1.5 ms per 1/60 s)");
        return true;
    }();
    (void)once;
    if (!fixed)
    {
        static uint64_t last = 0;
        const uint64_t now = Fm2Timebase();
        if (last)
        {
            const double share = std::clamp(double(now - last) / kConsoleFrame, 0.05, 1.0);
            StoreBudget(base, kConsoleTicks * share);
        }
        last = now;
    }
    TU_IMP(825FCB58)(ctx, base);
}

// singlestep(L) -> work done (lgc.c): only for FM2_LUA_GC_LOG.
PPC_FUNC_IMPL(TU_IMP(824D5DB0));
PPC_FUNC(TU_SUB(824D5DB0))
{
    static const bool logMemory = getenv("FM2_LUA_GC_LOG") != nullptr;
    if (logMemory)
    {
        static std::mutex lock;
        static std::map<uint32_t, uint32_t> bytes; // global_State -> totalbytes
        static auto next = std::chrono::steady_clock::now();
        const uint32_t g = PPC_LOAD_U32(ctx.r3.u32 + 0x10);
        std::lock_guard<std::mutex> hold(lock);
        bytes[g] = PPC_LOAD_U32(g + 0x44);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next)
        {
            next = now + std::chrono::seconds(10);
            uint64_t sum = 0;
            for (const auto& [state, n] : bytes)
                sum += n;
            Log("lua gc: Lua memory %.2f MB in %zu states", sum / 1048576.0, bytes.size());
        }
    }
    TU_IMP(824D5DB0)(ctx, base);
}
#endif
