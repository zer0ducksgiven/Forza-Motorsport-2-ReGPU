// XAPI fibers, replaced by host fibers. The title's SwitchToFiber swaps the
// stack pointer and register file by hand, which recompiled code cannot
// follow; instead each guest fiber is a host fiber running recompiled code
// with its own PPCContext and guest stack. The guest fiber object keeps
// its layout where titles read it: the fiber data at +0 (GetFiberData),
// the current fiber in KTHREAD+0x164 (GetCurrentFiber).
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <windows.h>

#include "kernel/imports.h"
#include "kernel/thread.h"

namespace
{
constexpr uint32_t kFiberObjectSize = 0xA50;

struct Fiber
{
    void* host = nullptr;
    uint32_t guest = 0;
    uint32_t start = 0;
    uint32_t stackLow = 0;
    uint32_t stackHigh = 0;
    uint64_t r13 = 0;       // PCR to run with, set by whoever switches in
    PPCContext* ctx = nullptr;
    bool converted = false; // a thread turned into a fiber
    std::atomic<bool> running{ false };
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Fiber*> g_fibers;

Fiber* Find(uint32_t guest)
{
    std::lock_guard lock(g_mutex);
    auto it = g_fibers.find(guest);
    return it == g_fibers.end() ? nullptr : it->second;
}

uint32_t CurrentFiber(PPCContext& ctx)
{
    return LoadBE32(LoadBE32(ctx.r13.u32 + kpcr::CurrentThread) + kthread::FiberPtr);
}

void SetCurrentFiber(PPCContext& ctx, uint32_t fiber)
{
    StoreBE32(LoadBE32(ctx.r13.u32 + kpcr::CurrentThread) + kthread::FiberPtr, fiber);
}

void WINAPI FiberMain(void* param)
{
    auto* fiber = static_cast<Fiber*>(param);
    PPCContext ctx{};
    ctx.r1.u64 = fiber->stackHigh - 0x100;
    ctx.r13.u64 = fiber->r13;
    ctx.msr = 0x9030;
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    fiber->ctx = &ctx;
    ctx.r3.u64 = LoadBE32(fiber->guest); // fiber data
    GuestCall(ctx, fiber->start);
    // A fiber routine must not return; XAPI ends the thread if it does.
    Log("fiber %08X returned; ending its thread", fiber->guest);
    ExitThread(0);
}

Fiber* NewGuestFiber(uint32_t data)
{
    auto* fiber = new Fiber;
    fiber->guest = GuestAlloc(Heap::Virtual, kFiberObjectSize);
    StoreBE32(fiber->guest, data);
    std::lock_guard lock(g_mutex);
    g_fibers[fiber->guest] = fiber;
    return fiber;
}

void Release(Fiber* fiber)
{
    {
        std::lock_guard lock(g_mutex);
        g_fibers.erase(fiber->guest);
    }
    if (fiber->stackLow)
        GuestFree(fiber->stackLow);
    GuestFree(fiber->guest);
    delete fiber;
}
}

// ConvertThreadToFiber(data)
PPC_FUNC(sub_82754CD0)
{
    if (CurrentFiber(ctx))
    {
        Return(ctx, 0);
        return;
    }
    Fiber* fiber = NewGuestFiber(ctx.r3.u32);
    fiber->converted = true;
    fiber->host = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    fiber->ctx = &ctx;
    fiber->running = true;
    GuestThread* t = CurrentGuestThread();
    StoreBE32(fiber->guest + 8, t ? t->stackHigh : 0);
    StoreBE32(fiber->guest + 12, t ? t->stackLow : 0);
    SetCurrentFiber(ctx, fiber->guest);
    Return(ctx, fiber->guest);
}

// ConvertFiberToThread()
PPC_FUNC(sub_82754D60)
{
    Fiber* fiber = Find(CurrentFiber(ctx));
    if (!fiber)
    {
        Return(ctx, 0);
        return;
    }
    SetCurrentFiber(ctx, 0);
    ConvertFiberToThread();
    Release(fiber);
    Return(ctx, 1);
}

// CreateFiber(stackSize, startAddress, data)
PPC_FUNC(sub_82754DB0)
{
    const uint32_t stackSize = std::max<uint32_t>((ctx.r3.u32 + 0xFFF) & ~0xFFFu, 0x10000);
    Fiber* fiber = NewGuestFiber(ctx.r5.u32);
    fiber->start = ctx.r4.u32;
    fiber->stackLow = GuestAlloc(Heap::Virtual, stackSize, 0x1000);
    fiber->stackHigh = fiber->stackLow + stackSize;
    StoreBE32(fiber->guest + 8, fiber->stackHigh);
    StoreBE32(fiber->guest + 12, fiber->stackLow);
    // Recompiled frames are large; give the host side room.
    fiber->host = CreateFiberEx(0x10000, 8 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, FiberMain, fiber);
    if (!fiber->host)
        Fatal("CreateFiberEx failed (%lu)", GetLastError());
    Return(ctx, fiber->guest);
}

// DeleteFiber(fiber)
PPC_FUNC(sub_82754E90)
{
    const uint32_t guest = ctx.r3.u32;
    if (guest == CurrentFiber(ctx))
    {
        Log("DeleteFiber on the running fiber: ending the thread");
        ExitCurrentGuestThread(0);
    }
    Fiber* fiber = Find(guest);
    if (!fiber)
        return;
    if (!fiber->converted)
        DeleteFiber(fiber->host);
    Release(fiber);
}

// SwitchToFiber(fiber)
PPC_FUNC(sub_82754EF0)
{
    Fiber* target = Find(ctx.r3.u32);
    if (!target)
        Fatal("SwitchToFiber to unknown fiber %08X (lr %08llX)", ctx.r3.u32, ctx.lr);
    // The target runs on this hardware thread now: give it our PCR.
    target->r13 = ctx.r13.u64;
    if (target->ctx)
        target->ctx->r13 = ctx.r13;
    Fiber* self = Find(CurrentFiber(ctx));
    if (target->running.exchange(true))
        Log("SwitchToFiber: fiber %08X is still running on another thread (lr %08llX)", target->guest, ctx.lr);
    SetCurrentFiber(ctx, target->guest);
    if (self)
        self->running = false;
    SwitchToFiber(target->host);
}

// The hand-written register swap SwitchToFiber jumps to; nothing should
// reach it now.
PPC_FUNC(sub_82756580)
{
    Fatal("XAPI fiber switch routine reached (lr %08llX)", ctx.lr);
}

// The swap itself (after the KeBugCheck above). ReXenon makes it a function of its own, because
// SwitchToFiber branches into it; XenonRecomp keeps it inside sub_82756580, and this is unused.
PPC_FUNC(sub_82756590)
{
    Fatal("XAPI fiber switch routine reached (lr %08llX)", ctx.lr);
}
