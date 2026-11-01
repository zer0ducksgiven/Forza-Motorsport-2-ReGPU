// The game's Lua garbage collection. The game runs about 30 Lua states (one per UI scene) and stops Lua's own
// collector in each; its scene update (TU 825FCB58) then steps each state's collector until a time budget is
// spent: 74812.5 timebase ticks a frame in all (the double at 82118900), 1.5 ms at the console's 49.875 MHz,
// split among the states. Being time and not work, it's 1.5 ms whatever the frame rate: 9% of a 60 fps frame
// on the console, about a third of the game's main thread (guest thread 3) at 180 fps here, most of it spent
// stepping collections that have nothing left to collect.
//
// FM2_LUA_GC_BUDGET=<fraction of the console's budget> (e.g. 0.2) sets that budget. Its global_State matches
// Lua 5.1's (totalbytes at +0x44), and every 10 s the Lua memory of all states is logged, to see that the
// collector keeps up.
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include "decomp/decomp.h"

#if FM2_TITLE_UPDATE
// built from halves: a 0x82xxxxxx literal here would be translated as a disc address by tools/xexremap.py
static const uint32_t kGcBudget = (0x8211u << 16) | 0x8900u;

// singlestep(L) -> work done (lgc.c)
PPC_FUNC_IMPL(TU_IMP(824D5DB0));
PPC_FUNC(TU_SUB(824D5DB0))
{
    static std::once_flag once;
    std::call_once(once, [base] {
        if (const char* v = getenv("FM2_LUA_GC_BUDGET"))
        {
            const double ticks = 74812.5 * atof(v);
            uint64_t bits;
            memcpy(&bits, &ticks, 8);
            PPC_STORE_U64(kGcBudget, bits);
            Log("decomp: Lua GC budget %.0f ticks a frame (%.2f ms; the console's is 1.5 ms)", ticks, ticks / 49875.0);
        }
    });

    static std::mutex lock;
    static std::map<uint32_t, uint32_t> bytes; // global_State -> totalbytes
    static auto next = std::chrono::steady_clock::now();
    const uint32_t g = PPC_LOAD_U32(ctx.r3.u32 + 0x10);
    {
        std::lock_guard<std::mutex> hold(lock);
        bytes[g] = PPC_LOAD_U32(g + 0x44);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next)
        {
            next = now + std::chrono::seconds(10);
            uint64_t sum = 0;
            for (const auto& [state, n] : bytes)
                sum += n;
            Log("decomp: Lua memory %.2f MB in %zu states", sum / 1048576.0, bytes.size());
        }
    }
    TU_IMP(824D5DB0)(ctx, base);
}
#endif
