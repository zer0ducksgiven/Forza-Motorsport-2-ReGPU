// Runs and checks the lifted functions (lift.h).
#include "decomp/lift.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
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
// real memory a lifted run read, each byte as it was read (repeats and all: a map of first reads made checks too
// slow to boot)
struct ReadByte
{
    uint32_t addr;
    uint8_t value;
};
thread_local std::vector<ReadByte>* t_reads = nullptr;
thread_local std::vector<uint32_t>* t_jmpbufs = nullptr; // setjmp buffers the current lifted run set
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
            fprintf(f, "address\tcalls\twrong\tvoid_r3\twhat\tbranches\tcovered\tescaped_by\traced\tunchecked\n");
            for (LiftStats* s = g_all.load(); s; s = s->next)
                if (s->calls)
                    fprintf(f, "%08X\t%u\t%u\t%u\t%s\t%u\t%llX\t%s\t%u\t%u\n", s->address, s->calls.load(), s->wrong.load() + (s->big || s->escaped ? 1 : 0),
                            s->voidR3.load(), s->big ? "big" : s->escaped ? "escaped" : s->what == 1 ? "memory" : s->what == 2 ? "return" : s->what == 3 ? "register" : "",
                            s->branches, (unsigned long long)s->covered.load(), s->escapedBy ? s->escapedBy : "", s->raced.load(), s->unchecked.load());
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

thread_local const char* t_escapedBy = nullptr; // why the current lifted run was abandoned

void LiftShadowGuard(const char* what)
{
    t_escapedBy = what;
    throw ShadowEscape{};
}

void Fm2ShadowIndirect(uint32_t target)
{
    if (Hooked().count(target))
    {
        t_escapedBy = "a hook";
        throw ShadowEscape{};
    }
}

// The lifted run of a check, as structured exception handling sees it: an escape (thrown above) or a fault (a wrong
// lifted function can hand recompiled code a bad pointer) ends it, false. Its own function: __try can't share one
// with C++ objects. (A filter that logged the fault took the check run down with it: keep it this simple.)
// the last fault in a lifted run, for the log: only copied in the filter (logging there crashed check runs)
thread_local uint32_t t_faultCode = 0;
thread_local uint64_t t_faultRip = 0, t_faultAddress = 0;

static int NoteFault(EXCEPTION_POINTERS* e)
{
    t_faultCode = e->ExceptionRecord->ExceptionCode;
    t_faultRip = uint64_t(e->ExceptionRecord->ExceptionAddress);
    t_faultAddress = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

// C++ exceptions other than the escapes, by what they say (std::exception) or "unknown"
thread_local const char* t_cppException = nullptr;
thread_local std::string t_cppWhat;

static bool RunShadowCpp(PPCFunc* lifted, PPCContext& c, uint8_t* base)
{
    try
    {
        lifted(c, base);
        return true;
    }
    catch (const ShadowEscape&)
    {
        return false;
    }
    catch (const std::exception& e)
    {
        t_cppWhat = e.what();
        t_cppException = t_cppWhat.c_str();
        return false;
    }
    catch (...)
    {
        t_cppException = "unknown";
        return false;
    }
}

static bool RunShadow(PPCFunc* lifted, PPCContext& c, uint8_t* base)
{
    t_cppException = nullptr;
    t_faultCode = 0, t_faultRip = 0, t_faultAddress = 0;
    __try
    {
        return RunShadowCpp(lifted, c, base);
    }
    __except (NoteFault(GetExceptionInformation()))
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
    {
        t_escapedBy = "spinning";
        throw ShadowEscape{};
    }
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++)
    {
        const uint32_t a = addr + i;
        auto it = t_overlay->find(a);
        uint8_t b;
        if (it != t_overlay->end())
            b = it->second;
        else
            t_reads->push_back({ a, b = base[a] });
        v = v << 8 | b;
    }
    return v;
}

void Fm2ShadowRead(const uint8_t* base, uint32_t addr, void* out, uint32_t size)
{
    if (++t_shadowLoads > 2000000)
    {
        t_escapedBy = "spinning";
        throw ShadowEscape{};
    }
    uint8_t* o = static_cast<uint8_t*>(out);
    for (uint32_t i = 0; i < size; i++)
    {
        const uint32_t a = addr + i;
        auto it = t_overlay->find(a);
        if (it != t_overlay->end())
            o[i] = it->second;
        else
            t_reads->push_back({ a, o[i] = base[a] });
    }
}

static void ShadowSizeGuard();

void Fm2NoteSetjmp(uint32_t buffer)
{
    if (g_fm2Shadow && t_jmpbufs)
        t_jmpbufs->push_back(buffer);
}

void Fm2CheckLongjmp(uint32_t buffer)
{
    if (g_fm2Shadow && (!t_jmpbufs || std::find(t_jmpbufs->begin(), t_jmpbufs->end(), buffer) == t_jmpbufs->end()))
    {
        t_escapedBy = "longjmp";
        throw ShadowEscape{};
    }
}

void Fm2ShadowWrite(uint32_t addr, const void* in, uint32_t size)
{
    ShadowSizeGuard();
    const uint8_t* b = static_cast<const uint8_t*>(in);
    for (uint32_t i = 0; i < size; i++)
        (*t_overlay)[addr + i] = b[i];
}

// a lifted run that writes this much (an overlay entry a byte) takes too long to check: given up, not kept
static void ShadowSizeGuard()
{
    if (t_overlay->size() > (1u << 20))
    {
        t_escapedBy = "big";
        throw ShadowEscape{};
    }
}

void Fm2ShadowStore(uint32_t addr, uint32_t size, uint64_t value)
{
    ShadowSizeGuard();
    for (uint32_t i = 0; i < size; i++)
        (*t_overlay)[addr + i] = uint8_t(value >> (8 * (size - 1 - i)));
}

LiftStats::LiftStats(uint32_t a, LiftReturn r, uint32_t n, uint32_t o) : address(a), ret(r), branches(n), outs(o)
{
    next = g_all.load();
    while (!g_all.compare_exchange_weak(next, this))
        ;
}

void LiftRun(LiftStats& s, PPCContext& ctx, uint8_t* base, PPCFunc* original, PPCFunc* lifted)
{
    // inside a check: in its lifted run everything below runs as the original (against the overlay); in its
    // original run a lifted function is checked too, its stores going into the outer check's log as well (else
    // functions only ever called inside other checks, which can last a whole route, were never checked: 2,192)
    static const bool nested = !getenv("FM2_LIFT_NESTED") || atoi(getenv("FM2_LIFT_NESTED"));
    if (t_depth > 0 && (g_fm2Shadow || !g_fm2StoreLog || !nested))
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
    // (and a function whose lifted runs keep reaching imports isn't run lifted again: each costs a whole shadow run)
    static const uint32_t maxUnchecked = getenv("FM2_LIFT_UNCHECKED") ? uint32_t(atoi(getenv("FM2_LIFT_UNCHECKED"))) : 16;
    if ((s.calls >= 64 && (s.covered & all) == all) || s.calls >= 1024 || s.big || s.escaped || s.unchecked >= maxUnchecked)
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
    std::vector<Logged>* const outerLog = t_log; // the check this one is inside, if any
    const bool outerLogging = g_fm2StoreLog;

    // the lifted version, against the overlay
    t_depth++;
    g_fm2StoreLog = false;
    std::unordered_map<uint32_t, uint8_t> overlay;
    std::vector<ReadByte> reads;
    std::vector<uint32_t> jmpbufs;
    t_overlay = &overlay;
    t_reads = &reads;
    t_jmpbufs = &jmpbufs;
    g_fm2Shadow = true;
    PPCContext c = ctx;
    t_shadowLoads = 0;
    t_escapedBy = "a fault"; // unless an escape says otherwise
    uint64_t branches = 0;
    t_liftBranches = &branches;
    const bool escaped = !RunShadow(lifted, c, base);
    t_liftBranches = nullptr;
    g_fm2Shadow = false;
    g_fm2StoreLog = outerLogging;
    t_overlay = nullptr;
    t_reads = nullptr;
    t_jmpbufs = nullptr;
    if (escaped)
    {
        // why, and where it faulted, before the original runs: lifted functions it calls start checks of their own,
        // which reset these (escapes at imports were taken for faults, and their functions left out)
        const char* const why = t_escapedBy;
        const uint32_t faultCode = t_faultCode;
        const uint64_t faultRip = t_faultRip, faultAddress = t_faultAddress;
        const std::string threw = t_cppException ? t_cppException : "";
        t_depth--;
        original(ctx, base);
        s.escapedBy = why;
        s.calls++;
        // a fault or a run that spun: wrong, or not checkable, so not kept. An import or a hook only means this
        // call couldn't be followed: live, the lifted code calls it for real, and the paths that lead there have
        // to pass in calls that don't reach it (the branches this one ran don't count)
        if (!strcmp(why, "big"))
            s.big = true;
        else if (!strcmp(why, "a fault") || !strcmp(why, "spinning"))
        {
            if (!s.escaped && !strcmp(why, "a fault"))
            {
                // where: the host address as an offset in the exe (llvm-symbolizer), the guest address it touched
                const uint64_t image = uint64_t(GetModuleHandleA(nullptr));
                if (!threw.empty())
                    Log("lift: %08X threw: %s", s.address, threw.c_str());
                else
                    Log("lift: %08X faulted: code %08X at exe+%llX, address %llX (guest %08llX)", s.address, faultCode,
                        (unsigned long long)(faultRip - image), (unsigned long long)faultAddress,
                        (unsigned long long)(faultAddress - uint64_t(base)));
            }
            s.escaped = true;
        }
        else
            s.unchecked++;
        return;
    }

    // the original, for real
    std::vector<Logged> log;
    t_log = &log;
    g_fm2StoreLog = true;
    original(ctx, base);
    g_fm2StoreLog = outerLogging;
    t_log = outerLog;
    if (outerLog)
        outerLog->insert(outerLog->end(), log.begin(), log.end());
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
    // registers a caller reads after the call, which the lifted code has to leave as the original does (with
    // link-time code generation a caller can know a callee leaves r4 or f2 set; the menus froze on one)
    int outDiff = -1;
    for (int i = 0; i < 21 && outDiff < 0; i++)
        if (s.outs >> i & 1)
        {
            static PPCRegister PPCContext::* const regs[21] = { &PPCContext::r3, &PPCContext::r4, &PPCContext::r5,
                &PPCContext::r6, &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10, &PPCContext::f1,
                &PPCContext::f2, &PPCContext::f3, &PPCContext::f4, &PPCContext::f5, &PPCContext::f6, &PPCContext::f7,
                &PPCContext::f8, &PPCContext::f9, &PPCContext::f10, &PPCContext::f11, &PPCContext::f12, &PPCContext::f13 };
            const bool same = (ctx.*regs[i]).u64 == (c.*regs[i]).u64;
            if (!same)
                outDiff = i;
        }
    if (outDiff >= 0)
        retSame = false;
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
        // another thread changed memory the lifted run read, between its read and the end of the original (both
        // ran on what they found): the inputs differed, so the results can too. The value the original found
        // where it stored is the one from before its first store
        std::unordered_map<uint32_t, uint8_t> first;
        for (const Logged& e : log)
            for (uint32_t i = 0; i < e.size; i++)
                first.emplace(e.addr + i, e.old[i]);
        for (const ReadByte& r : reads)
        {
            auto it = first.find(r.addr);
            if ((it != first.end() ? it->second : base[r.addr]) != r.value)
            {
                s.raced++;
                s.calls++;
                return;
            }
        }
    }
    if (!(memSame && retSame))
    {
        uint8_t none = 0;
        s.what.compare_exchange_strong(none, !memSame ? 1 : outDiff >= 0 ? 3 : 2);
        if (++s.wrong <= 3 && memSame && outDiff >= 0)
            Log("lift: %08X differs (%s%d, which a caller reads)", s.address, outDiff < 8 ? "r" : "f", outDiff < 8 ? outDiff + 3 : outDiff - 7);
        else if (s.wrong <= 3)
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
