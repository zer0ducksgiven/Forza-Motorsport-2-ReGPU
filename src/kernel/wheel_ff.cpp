// Force feedback of the Xbox 360 wheel: the kernel's XInputdFF* calls (xboxkrnl ordinals
// 0x282-0x289). FM2 looks them up at start (sub_8281DFA0 and the seven after it, through
// sub_82453238: XexGetModuleHandle("xboxkrnl.exe") and XexGetProcedureAddress) and drives the
// wheel's effects with them once XamInputGetCapabilities reports a wheel with force feedback.
// What the calls carry is from the Forza Horizon recomp (github.com/Dyotson/ForzaHorizon1-ReImagined,
// rexglue-sdk patch 0101, BSD 3-Clause), the same engine: an effect is 29 big-endian words
// (block, type, a word, gain 0-255, seven fixed words, then the type's parameters from word 11);
// types 1 constant, 8 spring, 9 damper, 10 inertia, 11 friction.
//
// For now every call is logged and succeeds without a wheel behind it, to see what FM2 sends. It
// sends what FH1 does: DeviceControl(3), the five effects (inertia and friction at 0, damper,
// spring, constant) started together, then each frame the constant force's magnitude (word 11,
// the steering force, -127 to 127) and the spring's gain (0-255, with speed and rear slip).
// FM2_FAKE_WHEEL=1 reports a force feedback wheel on player 1 (imports_xam.cpp).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <mutex>
#include <cstdlib>
#include <string>

#include "kernel/imports.h"
#include "kernel/wheel_ff.h"

namespace
{
constexpr uint32_t kFirstOrdinal = 0x282, kOrdinals = 8;
constexpr uint32_t kEffectWords = 29;
constexpr uint32_t kMaxBlocks = 16; // what GetDeviceInfo reports (FH1 needs at least 5)

// The guest addresses handed out for the eight calls: slots of the indirect-call table (one per
// 4 bytes of code) inside the body of the game's lookup helper (sub_82453238 on the disc,
// sub_8246DFE8 in the title update), which nothing calls indirectly: the 10 instructions before
// the return address of its XexGetProcedureAddress call, 0x40 bytes into it.

const char* const kNames[kOrdinals] = { "GetDeviceInfo", "SetEffect", "UpdateEffect", "EffectOperation", "DeviceControl",
    "SetDeviceGain", "CancelIo", "SetRumble" };

// The first 40 calls of each, then every 600th (UpdateEffect comes every frame). 8 and 9 are
// xam's two.
bool ShouldLog(uint32_t which)
{
    static std::atomic<uint32_t> calls[kOrdinals + 2];
    const uint32_t n = calls[which]++;
    return n < 40 || n % 600 == 0;
}

std::string EffectWords(uint32_t address)
{
    std::string s;
    char w[12];
    for (uint32_t i = 0; i < 17; i++)
    {
        snprintf(w, sizeof(w), i ? " %X" : "%X", LoadBE32(address + 4 * i));
        s += w;
    }
    return s;
}

const char* TypeName(uint32_t type)
{
    switch (type)
    {
    case 1: return "constant";
    case 8: return "spring";
    case 9: return "damper";
    case 10: return "inertia";
    case 11: return "friction";
    default: return "?";
    }
}

void LogEffect(const char* call, uint32_t address, uint32_t extra)
{
    Log("wheel ff: %s block %u %s gain %u (%X) words %s", call, LoadBE32(address), TypeName(LoadBE32(address + 4)),
        LoadBE32(address + 12), extra, EffectWords(address).c_str());
}

// With an XOVERLAPPED the call completes at once and returns STATUS_PENDING (FH1 patch 0101).
void Done(PPCContext& ctx, uint32_t overlapped)
{
    if (overlapped)
    {
        CompleteOverlapped(overlapped, 0);
        Return(ctx, X_STATUS_PENDING);
        return;
    }
    Return(ctx, X_STATUS_SUCCESS);
}

// (handle, info*): the second word is how many effects play at once.
void GetDeviceInfo(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(0))
        Log("wheel ff: GetDeviceInfo(handle %08X, info %08X)", ctx.r3.u32, ctx.r4.u32);
    if (!ctx.r4.u32)
    {
        Return(ctx, X_STATUS_INVALID_PARAMETER);
        return;
    }
    StoreBE32(ctx.r4.u32, 0);
    StoreBE32(ctx.r4.u32 + 4, kMaxBlocks);
    Return(ctx, X_STATUS_SUCCESS);
}

// (handle, effects, count, overlapped): count effects of 0x74 bytes.
void SetEffect(PPCContext& ctx, uint8_t*)
{
    const uint32_t effects = ctx.r4.u32, count = ctx.r5.u32;
    if (ShouldLog(1))
    {
        Log("wheel ff: SetEffect(handle %08X, %u effects, overlapped %08X)", ctx.r3.u32, count, ctx.r6.u32);
        for (uint32_t i = 0; i < count && i < kMaxBlocks; i++)
            LogEffect("  set", effects + i * 4 * kEffectWords, 0);
    }
    Done(ctx, ctx.r6.u32);
}

// (handle, effect, flags, overlapped): flags say what changed (0x400 the parameters, 0x100 the gain).
void UpdateEffect(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(2))
        LogEffect("UpdateEffect", ctx.r4.u32, ctx.r5.u32);
    // Every 2 s: how many updates and the range of the steering force (the constant force's
    // magnitude, word 11, -127 to 127) and the spring's gain.
    struct Range
    {
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        void Add(int32_t v) { lo = std::min(lo, v), hi = std::max(hi, v); }
    };
    static std::mutex mutex;
    static Range force, spring;
    static uint32_t updates = 0;
    static auto since = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    const uint32_t effect = ctx.r4.u32, type = LoadBE32(effect + 4);
    updates++;
    // FM2_WHEEL_FF_WORDS=1: every change of an effect's 29 words (the first 100).
    static const bool words = getenv("FM2_WHEEL_FF_WORDS") != nullptr;
    static uint32_t last[16][kEffectWords], changes = 0;
    const uint32_t block = LoadBE32(effect) & 15;
    bool changed = false;
    for (uint32_t i = 0; i < kEffectWords; i++)
        if (const uint32_t w = LoadBE32(effect + 4 * i); w != last[block][i])
            last[block][i] = w, changed = true;
    if (words && changed && changes++ < 100)
    {
        std::string all;
        char w[12];
        for (uint32_t i = 0; i < kEffectWords; i++)
            snprintf(w, sizeof(w), " %X", last[block][i]), all += w;
        Log("wheel ff: block %u (%X):%s", block, ctx.r5.u32, all.c_str());
    }
    if (type == 1)
        force.Add(int32_t(LoadBE32(effect + 44)));
    else if (type == 8)
        spring.Add(int32_t(LoadBE32(effect + 12)));
    const auto now = std::chrono::steady_clock::now();
    if (now - since >= std::chrono::seconds(2))
    {
        auto text = [](const Range& r) { return r.lo > r.hi ? std::string("none") : std::to_string(r.lo) + ".." + std::to_string(r.hi); };
        Log("wheel ff: last 2 s: %u updates, steering force %s, spring gain %s", updates, text(force).c_str(), text(spring).c_str());
        force = spring = Range{};
        updates = 0;
        since = now;
    }
    Done(ctx, ctx.r6.u32);
}

// (handle, operations, count, loop count, overlapped): count pairs of (block, operation).
void EffectOperation(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(3))
    {
        std::string ops;
        char op[24];
        for (uint32_t i = 0; i < ctx.r5.u32 && i < kMaxBlocks; i++)
        {
            snprintf(op, sizeof(op), " %u:%u", LoadBE32(ctx.r4.u32 + 8 * i), LoadBE32(ctx.r4.u32 + 8 * i + 4));
            ops += op;
        }
        Log("wheel ff: EffectOperation(handle %08X, block:op%s, loops %u)", ctx.r3.u32, ops.c_str(), ctx.r6.u32);
    }
    Done(ctx, ctx.r7.u32);
}

// (handle, command, overlapped): HID PID device control (3 stops everything).
void DeviceControl(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(4))
        Log("wheel ff: DeviceControl(handle %08X, command %u)", ctx.r3.u32, ctx.r4.u32);
    Done(ctx, ctx.r5.u32);
}

// (handle, gain, overlapped)
void SetDeviceGain(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(5))
        Log("wheel ff: SetDeviceGain(handle %08X, gain %u)", ctx.r3.u32, ctx.r4.u32);
    Done(ctx, ctx.r5.u32);
}

// (handle, overlapped)
void CancelIo(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(6))
        Log("wheel ff: CancelIo(handle %08X)", ctx.r3.u32);
    Return(ctx, X_STATUS_SUCCESS);
}

// (handle, vibration*, overlapped): a 360 controller's two motor speeds.
void SetRumble(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(7))
        Log("wheel ff: SetRumble(handle %08X, %08X %08X)", ctx.r3.u32, ctx.r4.u32 ? LoadBE32(ctx.r4.u32) : 0, ctx.r5.u32);
    Done(ctx, ctx.r5.u32);
}

// xam.xex ordinal 442, (user, level*): the profile's vibration setting. The library sends no
// rumble for 0 (FH1 patch 0101).
void GetUserVibrationLevel(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(8))
        Log("wheel ff: XamInputGetUserVibrationLevel(user %u)", ctx.r3.u32);
    StoreIfNonNull(ctx.r4.u32, 1);
    Return(ctx, 0);
}

// xam.xex ordinal 520, (user, unknown, context*): the handle the XInputdFF* calls take. The
// force feedback library (sub_8246E040 in the title update) gives up without it.
void UserGetDeviceContext(PPCContext& ctx, uint8_t*)
{
    if (ShouldLog(9))
        Log("wheel ff: XamUserGetDeviceContext(user %u, %08X)", ctx.r3.u32, ctx.r4.u32);
    StoreIfNonNull(ctx.r5.u32, 0);
    Return(ctx, 0);
}

// The kernel's eight, then xam's two.
PPCFunc* const kFunctions[kOrdinals + 2] = { GetDeviceInfo, SetEffect, UpdateEffect, EffectOperation, DeviceControl, SetDeviceGain,
    CancelIo, SetRumble, GetUserVibrationLevel, UserGetDeviceContext };
}

bool WheelFakeEnabled()
{
    static const bool on = [] {
        const char* v = getenv("FM2_FAKE_WHEEL");
        return v && atoi(v) != 0;
    }();
    return on;
}

uint32_t WheelFfProcedure(uint32_t module, uint32_t ordinal, uint32_t returnAddress)
{
    if (!WheelFakeEnabled())
        return 0;
    uint32_t i;
    if (module == kKernelModuleHandle && ordinal >= kFirstOrdinal && ordinal < kFirstOrdinal + kOrdinals)
        i = ordinal - kFirstOrdinal;
    else if (module == kXamModuleHandle && (ordinal == 442 || ordinal == 520))
        i = ordinal == 442 ? kOrdinals : kOrdinals + 1;
    else
        return 0;
    const uint32_t address = (returnAddress & ~3u) - 4 * (kOrdinals + 2 - i);
    PPC_LOOKUP_FUNC(g_base, address) = kFunctions[i];
    Log("wheel ff: %s ordinal %u at %08X", module == kKernelModuleHandle ? "xboxkrnl" : "xam", ordinal, address);
    return address;
}
