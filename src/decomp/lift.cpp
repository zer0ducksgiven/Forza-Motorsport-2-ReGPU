// Runs and checks the lifted functions (lift.h).
#include "decomp/lift.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "decomp/decomp.h"

thread_local bool g_fm2StoreLog = false;
thread_local bool g_fm2Shadow = false;

namespace
{
struct Logged
{
    uint32_t addr;
    uint32_t size;
    uint8_t old[8];
};
thread_local std::vector<Logged>* t_log = nullptr;
thread_local std::unordered_map<uint32_t, uint8_t>* t_overlay = nullptr;
thread_local int t_depth = 0; // inside a check: everything below runs as the original

std::atomic<LiftStats*> g_all{ nullptr };

void Writer()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (FILE* f = fopen("out/lift_check.tsv.tmp", "w"))
        {
            fprintf(f, "address\tcalls\twrong\tvoid_r3\twhat\n");
            for (LiftStats* s = g_all.load(); s; s = s->next)
                if (s->calls)
                    fprintf(f, "%08X\t%u\t%u\t%u\t%s\n", s->address, s->calls.load(), s->wrong.load() + (s->big ? 1 : 0),
                            s->voidR3.load(), s->big ? "big" : s->what == 1 ? "memory" : s->what == 2 ? "return" : "");
            fclose(f);
            MoveFileExA("out/lift_check.tsv.tmp", "out/lift_check.tsv", MOVEFILE_REPLACE_EXISTING);
        }
    }
}
} // namespace

// FM2_LIFT_ENABLE=<file of hex addresses>: only those lifted functions run lifted (for bisecting a bad one)
static bool LiftEnabled(uint32_t address)
{
    static const std::unordered_map<uint32_t, bool>* const only = []() -> const std::unordered_map<uint32_t, bool>* {
        const char* path = getenv("FM2_LIFT_ENABLE");
        if (!path)
            return nullptr;
        auto* m = new std::unordered_map<uint32_t, bool>;
        if (FILE* f = fopen(path, "r"))
        {
            unsigned a;
            while (fscanf(f, "%x", &a) == 1)
                (*m)[a] = true;
            fclose(f);
        }
        Log("lift: %zu functions enabled from %s", m->size(), path);
        return m;
    }();
    return !only || only->count(address);
}

bool LiftInCheck()
{
    return t_depth > 0;
}

void Fm2LogStore(uint8_t* base, uint32_t addr, uint32_t size)
{
    if (!t_log)
        return;
    Logged e{ addr, size, {} };
    for (uint32_t i = 0; i < size; i++)
        e.old[i] = base[uint32_t(addr + i)];
    t_log->push_back(e);
}

uint64_t Fm2ShadowLoad(const uint8_t* base, uint32_t addr, uint32_t size)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++)
    {
        const uint32_t a = addr + i;
        auto it = t_overlay->find(a);
        v = v << 8 | (it != t_overlay->end() ? it->second : base[a]);
    }
    return v;
}

void Fm2ShadowStore(uint32_t addr, uint32_t size, uint64_t value)
{
    for (uint32_t i = 0; i < size; i++)
        (*t_overlay)[addr + i] = uint8_t(value >> (8 * (size - 1 - i)));
}

LiftStats::LiftStats(uint32_t a, LiftReturn r) : address(a), ret(r)
{
    next = g_all.load();
    while (!g_all.compare_exchange_weak(next, this))
        ;
}

void LiftRun(LiftStats& s, PPCContext& ctx, uint8_t* base, PPCFunc* original, PPCFunc* lifted)
{
    if (t_depth > 0)
    {
        original(ctx, base);
        return;
    }
    if (!LiftChecking())
    {
        (DecompOff() || !LiftEnabled(s.address) ? original : lifted)(ctx, base);
        return;
    }
    // enough to judge a function; after that it runs as the original, so a check run keeps a playable speed
    if (s.calls >= 64 || s.big)
    {
        original(ctx, base);
        return;
    }
#ifndef FM2_STORE_LOG
    static bool said = (Log("lift: FM2_DECOMP_CHECK needs an FM2_STORE_LOG build; running the originals"), true);
    (void)said;
    original(ctx, base);
#else
    static std::once_flag once;
    std::call_once(once, [] { std::thread(Writer).detach(); });
    const uint32_t sp = ctx.r1.u32;

    // the lifted version, against the overlay
    t_depth++;
    std::unordered_map<uint32_t, uint8_t> overlay;
    t_overlay = &overlay;
    g_fm2Shadow = true;
    PPCContext c = ctx;
    lifted(c, base);
    g_fm2Shadow = false;
    t_overlay = nullptr;

    // the original, for real
    std::vector<Logged> log;
    t_log = &log;
    g_fm2StoreLog = true;
    original(ctx, base);
    g_fm2StoreLog = false;
    t_log = nullptr;
    t_depth--;

    // dead on return: the stack below r1, and the parameter save area above it, which is the callee's to use
    auto dead = [sp](uint32_t a) { return a < sp + 0x60 && a >= sp - 0x10000; };
    bool memSame = true;
    uint32_t at = 0, was = 0, now = 0; // the first difference, for the log
    for (const auto& [a, v] : overlay)
        if (!dead(a) && base[a] != v)
        {
            memSame = false, at = a, was = base[a], now = v;
            break;
        }
    if (memSame)
    {
        // stored by the original only: it must have left the value from before its first store there
        std::unordered_map<uint32_t, uint8_t> before;
        for (const Logged& e : log)
            for (uint32_t i = 0; i < e.size; i++)
                before.emplace(e.addr + i, e.old[i]);
        for (const auto& [a, old] : before)
            if (!dead(a) && !overlay.count(a) && base[a] != old)
            {
                memSame = false, at = a, was = base[a], now = old;
                break;
            }
    }
    bool retSame = true;
    if (s.ret == LiftReturn::Int)
        retSame = ctx.r3.u32 == c.r3.u32;
    else if (s.ret == LiftReturn::Float)
        retSame = ctx.f1.u64 == c.f1.u64;
    else if (ctx.r3.u32 != c.r3.u32)
        s.voidR3++;
    if (!(memSame && retSame))
    {
        uint8_t none = 0;
        s.what.compare_exchange_strong(none, memSame ? 2 : 1);
        if (++s.wrong <= 3)
            Log("lift: %08X differs (%s): at %08X original %02X lifted %02X; r3 %08X / %08X, f1 %g / %g; sp %08X",
                s.address, memSame ? "return register" : "memory", at, was, now, ctx.r3.u32, c.r3.u32, ctx.f1.f64, c.f1.f64, sp);
    }
    s.calls++;
    // a function that writes this much is slow to check (an overlay entry a byte): it isn't checked again, and
    // lift.py doesn't keep it
    if (overlay.size() > 65536 || log.size() > 16384)
        s.big = true;
#endif
}
