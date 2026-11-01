// Runs and checks the lifted functions (lift.h).
#include "decomp/lift.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "decomp/decomp.h"

thread_local bool g_fm2StoreLog = false;
thread_local bool g_fm2Shadow = false;
thread_local uint64_t* t_liftBranches = nullptr;

namespace
{
struct Logged
{
    uint32_t addr;
    uint32_t size;
    uint8_t old[16]; // up to a vector
};
thread_local std::vector<Logged>* t_log = nullptr;
thread_local std::unordered_map<uint32_t, uint8_t>* t_overlay = nullptr;
thread_local int t_depth = 0; // inside a check: everything below runs as the original
thread_local uint64_t t_shadowLoads = 0;

// A check's lifted run reaching an import, a hook, or memory it can't see: it's abandoned (thrown out of)
// before anything outside the overlay happens.
struct ShadowEscape
{
};

std::atomic<LiftStats*> g_all{ nullptr };

std::string Hex(uint32_t v, int digits)
{
    char b[16];
    snprintf(b, sizeof(b), "%0*X", digits, v);
    return b;
}

void Writer()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (FILE* f = fopen("out/lift_check.tsv.tmp", "w"))
        {
            fprintf(f, "address\tcalls\twrong\tvoid_r3\twhat\tbranches\tcovered\n");
            for (LiftStats* s = g_all.load(); s; s = s->next)
                if (s->calls)
                    fprintf(f, "%08X\t%u\t%u\t%u\t%s\t%u\t%llX\n", s->address, s->calls.load(), s->wrong.load() + (s->big || s->escaped ? 1 : 0),
                            s->voidR3.load(), s->big ? "big" : s->escaped ? "escaped" : s->what == 1 ? "memory" : s->what == 2 ? "return" : "",
                            s->branches, (unsigned long long)s->covered.load());
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

static std::unordered_map<uint32_t, bool>& Hooked()
{
    static std::unordered_map<uint32_t, bool> h;
    return h;
}

void LiftRegisterHooked(const uint32_t* addresses, size_t n)
{
    for (size_t i = 0; i < n; i++)
        Hooked()[addresses[i]] = true;
}

void LiftShadowGuard(const char*)
{
    throw ShadowEscape{};
}

void Fm2ShadowIndirect(uint32_t target)
{
    if (Hooked().count(target))
        throw ShadowEscape{};
}

// The lifted run of a check, as structured exception handling sees it: an escape (thrown above) or a fault (a wrong
// lifted function can hand recompiled code a bad pointer) ends it, false. Its own function: __try can't share one
// with C++ objects.
static bool RunShadow(PPCFunc* lifted, PPCContext& c, uint8_t* base)
{
    __try
    {
        lifted(c, base);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
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
    // a lifted run that waits on memory it wrote itself (into the overlay, where no other thread can change it)
    // would never finish: give up on it
    if (++t_shadowLoads > 2000000)
        throw ShadowEscape{};
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++)
    {
        const uint32_t a = addr + i;
        auto it = t_overlay->find(a);
        v = v << 8 | (it != t_overlay->end() ? it->second : base[a]);
    }
    return v;
}

void Fm2ShadowRead(const uint8_t* base, uint32_t addr, void* out, uint32_t size)
{
    if (++t_shadowLoads > 2000000)
        throw ShadowEscape{};
    uint8_t* o = static_cast<uint8_t*>(out);
    for (uint32_t i = 0; i < size; i++)
    {
        const uint32_t a = addr + i;
        auto it = t_overlay->find(a);
        o[i] = it != t_overlay->end() ? it->second : base[a];
    }
}

void Fm2ShadowWrite(uint32_t addr, const void* in, uint32_t size)
{
    const uint8_t* b = static_cast<const uint8_t*>(in);
    for (uint32_t i = 0; i < size; i++)
        (*t_overlay)[addr + i] = b[i];
}

void Fm2ShadowStore(uint32_t addr, uint32_t size, uint64_t value)
{
    for (uint32_t i = 0; i < size; i++)
        (*t_overlay)[addr + i] = uint8_t(value >> (8 * (size - 1 - i)));
}

LiftStats::LiftStats(uint32_t a, LiftReturn r, uint32_t n) : address(a), ret(r), branches(n)
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
    // checked enough: 64 calls once every branch has been seen, else up to 1,024 waiting for the rest
    const uint64_t all = s.branches >= 64 ? ~0ull : (1ull << s.branches) - 1;
    if ((s.calls >= 64 && (s.covered & all) == all) || s.calls >= 1024 || s.big || s.escaped)
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
    t_shadowLoads = 0;
    uint64_t branches = 0;
    t_liftBranches = &branches;
    const bool escaped = !RunShadow(lifted, c, base);
    t_liftBranches = nullptr;
    g_fm2Shadow = false;
    t_overlay = nullptr;
    if (escaped)
    {
        // it reached an import or a hook, or faulted: not something a check can pass, so it isn't kept
        t_depth--;
        original(ctx, base);
        s.escaped = true;
        s.calls++;
        return;
    }

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
    // FM2_LIFT_TRACE=<hex address>: everything a check of that function compared, the first few times it differs
    static const uint32_t trace = getenv("FM2_LIFT_TRACE") ? uint32_t(strtoul(getenv("FM2_LIFT_TRACE"), nullptr, 16)) : 0;
    if (trace == s.address && !memSame && s.wrong < 2)
    {
        std::map<uint32_t, uint8_t> sorted(overlay.begin(), overlay.end());
        std::string line;
        for (const auto& [a, v] : sorted)
            if (!dead(a))
                line += Hex(a, 8) + "=" + Hex(v, 2) + "/" + Hex(base[a], 2) + " ";
        Log("lift trace %08X: overlay (lifted/real) %s", s.address, line.c_str());
        line.clear();
        for (const Logged& e : log)
            if (!dead(e.addr))
                line += Hex(e.addr, 8) + ":" + std::to_string(e.size) + " ";
        Log("lift trace %08X: original stored %s", s.address, line.c_str());
    }
    if (!(memSame && retSame))
    {
        uint8_t none = 0;
        s.what.compare_exchange_strong(none, memSame ? 2 : 1);
        if (++s.wrong <= 3)
            Log("lift: %08X differs (%s): at %08X original %02X lifted %02X; r3 %08X / %08X, f1 %g / %g; sp %08X",
                s.address, memSame ? "return register" : "memory", at, was, now, ctx.r3.u32, c.r3.u32, ctx.f1.f64, c.f1.f64, sp);
    }
    if (memSame && retSame)
        s.covered |= branches;
    s.calls++;
    // a function that writes this much is slow to check (an overlay entry a byte): it isn't checked again, and
    // lift.py doesn't keep it
    if (overlay.size() > 65536 || log.size() > 16384)
        s.big = true;
#endif
}
