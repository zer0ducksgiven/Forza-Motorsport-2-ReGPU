// Rtl*: critical sections, strings, printf, time conversion, exceptions.
#include <atomic>
#include <cwchar>

#include <windows.h>

#include "kernel/imports.h"
#include "kernel/thread.h"
#include "live_switch.h"
#include "loader.h"

namespace
{
// RTL_CRITICAL_SECTION: DISPATCHER_HEADER (0x10), LockCount 0x10,
// RecursionCount 0x14, OwningThread 0x18. Owner is the KTHREAD address,
// stored big-endian; we only ever compare it.
constexpr uint32_t kCsLockCount = 0x10;
constexpr uint32_t kCsRecursion = 0x14;
constexpr uint32_t kCsOwner = 0x18;
// Threads queued in RtlEnterCriticalSection, kept (host order) in the header's WaitListHead,
// which the section's own event would use on the console.
constexpr uint32_t kCsWaiters = 0x08;
// Owner value while a leaving owner hands the section to a queued thread.
constexpr uint32_t kHandoff = 0xFFFFFFFF;

std::atomic<uint32_t>& CsOwner(uint32_t cs)
{
    return *reinterpret_cast<std::atomic<uint32_t>*>(GuestPtr(cs + kCsOwner));
}

std::atomic<uint32_t>& CsWaiters(uint32_t cs)
{
    return *reinterpret_cast<std::atomic<uint32_t>*>(GuestPtr(cs + kCsWaiters));
}

uint32_t Me()
{
    GuestThread* t = CurrentGuestThread();
    return __builtin_bswap32(t ? t->kthread : 1);
}

void InitCs(uint32_t cs)
{
    memset(GuestPtr(cs), 0, 0x1C);
    *GuestPtr<uint8_t>(cs) = 1; // SynchronizationEvent-like header
    StoreBE32(cs + kCsLockCount, 0xFFFFFFFF);
}

bool TryEnterCs(uint32_t cs, uint32_t me)
{
    auto& owner = CsOwner(cs);
    uint32_t expected = 0;
    if (owner.compare_exchange_strong(expected, me, std::memory_order_acquire))
    {
        StoreBE32(cs + kCsRecursion, 1);
        StoreBE32(cs + kCsLockCount, 0);
        return true;
    }
    if (expected == me)
    {
        StoreBE32(cs + kCsRecursion, LoadBE32(cs + kCsRecursion) + 1);
        StoreBE32(cs + kCsLockCount, LoadBE32(cs + kCsLockCount) + 1);
        return true;
    }
    return false;
}
}

GUEST_EXPORT(RtlInitializeCriticalSection)
{
    InitCs(ctx.r3.u32);
}

GUEST_EXPORT(RtlInitializeCriticalSectionAndSpinCount)
{
    InitCs(ctx.r3.u32);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(RtlEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = Me();
    for (int spin = 0; spin < 64; spin++)
    {
        if (TryEnterCs(cs, me))
            return;
        _mm_pause();
    }
    // Queue as a waiter: a leaving owner hands the section to one of us (kHandoff), which a
    // thread arriving later cannot take.
    static LiveSwitch noHandoff("FM2_NO_CS_HANDOFF"); // A/B: no handoff (barging)
    auto& owner = CsOwner(cs);
    CsWaiters(cs).fetch_add(1, std::memory_order_acq_rel);
    for (;;)
    {
        uint32_t expected = kHandoff;
        if (owner.compare_exchange_strong(expected, me, std::memory_order_acquire))
        {
            StoreBE32(cs + kCsRecursion, 1);
            StoreBE32(cs + kCsLockCount, 0);
            break;
        }
        if (TryEnterCs(cs, me))
            break;
        uint32_t seen = owner.load(std::memory_order_relaxed);
        if (seen && seen != kHandoff)
            WaitOnAddress(&owner, &seen, 4, noHandoff ? 1 : 10);
    }
    CsWaiters(cs).fetch_sub(1, std::memory_order_acq_rel);
}

GUEST_EXPORT(RtlTryEnterCriticalSection)
{
    Return(ctx, TryEnterCs(ctx.r3.u32, Me()) ? 1 : 0);
}

GUEST_EXPORT(RtlLeaveCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t recursion = LoadBE32(cs + kCsRecursion) - 1;
    StoreBE32(cs + kCsRecursion, recursion);
    StoreBE32(cs + kCsLockCount, LoadBE32(cs + kCsLockCount) - 1);
    if (recursion == 0)
    {
        // With threads waiting, hand the section to one of them, as the kernel does: releasing it
        // to anyone let the owner take it straight back in a loop and starve them.
        static LiveSwitch noHandoff("FM2_NO_CS_HANDOFF");
        const bool handoff = !noHandoff && CsWaiters(cs).load(std::memory_order_acquire) > 0;
        CsOwner(cs).store(handoff ? kHandoff : 0, std::memory_order_release);
        WakeByAddressSingle(&CsOwner(cs));
    }
}

// Strings ------------------------------------------------------------------

GUEST_EXPORT(RtlInitAnsiString)
{
    const uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    const uint16_t length = src ? uint16_t(strlen(GuestPtr<char>(src))) : 0;
    *GuestPtr<uint16_t>(dest) = __builtin_bswap16(length);
    *GuestPtr<uint16_t>(dest + 2) = __builtin_bswap16(src ? length + 1 : 0);
    StoreBE32(dest + 4, src);
}

GUEST_EXPORT(RtlInitUnicodeString)
{
    const uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t length = 0;
    if (src)
    {
        while (*GuestPtr<uint16_t>(src + length * 2))
            length++;
    }
    *GuestPtr<uint16_t>(dest) = __builtin_bswap16(length * 2);
    *GuestPtr<uint16_t>(dest + 2) = __builtin_bswap16(src ? (length + 1) * 2 : 0);
    StoreBE32(dest + 4, src);
}

GUEST_EXPORT(RtlFreeAnsiString)
{
    const uint32_t s = ctx.r3.u32;
    if (const uint32_t buffer = LoadBE32(s + 4))
        GuestFree(buffer);
    memset(GuestPtr(s), 0, 8);
}

GUEST_EXPORT(RtlUnicodeStringToAnsiString)
{
    const uint32_t dest = ctx.r3.u32, src = ctx.r4.u32, allocate = ctx.r5.u32;
    const uint16_t bytes = __builtin_bswap16(*GuestPtr<uint16_t>(src));
    const uint32_t srcBuffer = LoadBE32(src + 4);
    const uint16_t chars = bytes / 2;
    uint32_t buffer = LoadBE32(dest + 4);
    uint16_t maxLength = __builtin_bswap16(*GuestPtr<uint16_t>(dest + 2));
    if (allocate)
    {
        maxLength = chars + 1;
        buffer = GuestAlloc(Heap::Virtual, maxLength);
        StoreBE32(dest + 4, buffer);
        *GuestPtr<uint16_t>(dest + 2) = __builtin_bswap16(maxLength);
    }
    else if (chars + 1 > maxLength)
    {
        Return(ctx, X_STATUS_BUFFER_OVERFLOW);
        return;
    }
    for (uint16_t i = 0; i < chars; i++)
        GuestPtr<char>(buffer)[i] = char(__builtin_bswap16(GuestPtr<uint16_t>(srcBuffer)[i]));
    GuestPtr<char>(buffer)[chars] = 0;
    *GuestPtr<uint16_t>(dest) = __builtin_bswap16(chars);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(RtlMultiByteToUnicodeN)
{
    // (UnicodeString, MaxBytesInUnicodeString, BytesInUnicodeString*, MultiByteString, BytesInMultiByteString)
    const uint32_t out = ctx.r3.u32, maxBytes = ctx.r4.u32, written = ctx.r5.u32, in = ctx.r6.u32, inBytes = ctx.r7.u32;
    const uint32_t count = std::min(maxBytes / 2, inBytes);
    for (uint32_t i = 0; i < count; i++)
        GuestPtr<uint16_t>(out)[i] = __builtin_bswap16(uint8_t(GuestPtr<char>(in)[i]));
    StoreIfNonNull(written, count * 2);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(RtlUnicodeToMultiByteN)
{
    // (MultiByteString, MaxBytesInMultiByteString, BytesInMultiByteString*, UnicodeString, BytesInUnicodeString)
    const uint32_t out = ctx.r3.u32, maxBytes = ctx.r4.u32, written = ctx.r5.u32, in = ctx.r6.u32, inBytes = ctx.r7.u32;
    const uint32_t count = std::min(maxBytes, inBytes / 2);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(in)[i]);
        GuestPtr<char>(out)[i] = c < 0x100 ? char(c) : '?';
    }
    StoreIfNonNull(written, count);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(RtlCompareMemoryUlong)
{
    const uint32_t src = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    uint32_t matched = 0;
    for (; matched + 4 <= length; matched += 4)
    {
        if (LoadBE32(src + matched) != pattern)
            break;
    }
    Return(ctx, matched);
}

GUEST_EXPORT(RtlFillMemoryUlong)
{
    const uint32_t dest = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= length; i += 4)
        StoreBE32(dest + i, pattern);
}

GUEST_EXPORT(RtlNtStatusToDosError)
{
    const uint32_t status = ctx.r3.u32;
    uint32_t error;
    switch (status)
    {
    case X_STATUS_SUCCESS: error = 0; break;
    case X_STATUS_PENDING: error = 997; break;
    case X_STATUS_TIMEOUT: error = 1460; break;
    case X_STATUS_NO_SUCH_FILE:
    case X_STATUS_OBJECT_NAME_NOT_FOUND: error = 2; break;
    case X_STATUS_OBJECT_PATH_NOT_FOUND: error = 3; break;
    case X_STATUS_ACCESS_DENIED: error = 5; break;
    case X_STATUS_INVALID_HANDLE: error = 6; break;
    case X_STATUS_NO_MEMORY: error = 8; break;
    case X_STATUS_NO_MORE_FILES: error = 18; break;
    case X_STATUS_END_OF_FILE: error = 38; break;
    case X_STATUS_NOT_SUPPORTED: error = 50; break;
    case X_STATUS_INVALID_PARAMETER: error = 87; break;
    case X_STATUS_BUFFER_OVERFLOW: error = 234; break;
    case X_STATUS_OBJECT_NAME_COLLISION: error = 183; break;
    case X_STATUS_DEVICE_NOT_CONNECTED: error = 1167; break;
    default:
        error = (status & 0xFFFF0000) == 0x80070000 ? (status & 0xFFFF) : 317;
        break;
    }
    Return(ctx, error);
}

// Time ---------------------------------------------------------------------

// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (u16 each).
GUEST_EXPORT(RtlTimeToTimeFields)
{
    const uint64_t time = LoadBE64(ctx.r3.u32);
    FILETIME ft{ DWORD(time), DWORD(time >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    auto* f = GuestPtr<uint16_t>(ctx.r4.u32);
    const uint16_t v[8] = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, st.wDayOfWeek };
    for (int i = 0; i < 8; i++)
        f[i] = __builtin_bswap16(v[i]);
}

GUEST_EXPORT(RtlTimeFieldsToTime)
{
    auto* f = GuestPtr<uint16_t>(ctx.r3.u32);
    SYSTEMTIME st{};
    st.wYear = __builtin_bswap16(f[0]);
    st.wMonth = __builtin_bswap16(f[1]);
    st.wDay = __builtin_bswap16(f[2]);
    st.wHour = __builtin_bswap16(f[3]);
    st.wMinute = __builtin_bswap16(f[4]);
    st.wSecond = __builtin_bswap16(f[5]);
    st.wMilliseconds = __builtin_bswap16(f[6]);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft))
    {
        Return(ctx, 0);
        return;
    }
    StoreBE64(ctx.r4.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    Return(ctx, 1);
}

// XEX ----------------------------------------------------------------------

GUEST_EXPORT(RtlImageXexHeaderField)
{
    Return(ctx, XexHeaderField(ctx.r3.u32, ctx.r4.u32));
}

// Exceptions and debugging -------------------------------------------------

GUEST_EXPORT(RtlCaptureContext)
{
    memset(GuestPtr(ctx.r3.u32), 0, 0x200);
}

GUEST_EXPORT(RtlRaiseException)
{
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = LoadBE32(record);
    if (code == 0x406D1388) // MS_VC_EXCEPTION: SetThreadName
    {
        const uint32_t info = LoadBE32(record + 0x14 + 4); // ExceptionInformation[1]: name
        if (GuestThread* t = CurrentGuestThread(); t && info)
        {
            t->name = GuestString(info);
            Log("thread %u named \"%s\"", t->id, t->name.c_str());
        }
        return;
    }
    // EXCEPTION_RECORD: code, flags, nested record, address, parameter count, parameters.
    const uint32_t flags = LoadBE32(record + 4), address = LoadBE32(record + 0xC), count = LoadBE32(record + 0x10);
    char hex[64] = {};
    for (uint32_t i = 0, n = 0; i < count && i < 4; i++)
        n += snprintf(hex + n, sizeof(hex) - n, " %08X", LoadBE32(record + 0x14 + i * 4));
    // The throwing function is the one containing lr; the crash handler's guest stack names it.
    Fatal("guest raised exception %08X%s flags %08X at %08X, parameters%s (lr %08llX)", code,
        code == 0xE06D7363 ? " (MSVC C++ throw; guest unwinding is not implemented)" : "", flags, address, hex, ctx.lr);
}

GUEST_EXPORT(RtlUnwind)
{
    // (targetFrame, targetIp, exceptionRecord, returnValue)
    Fatal("RtlUnwind(frame %08X, ip %08X, record %08X, value %08X) called (lr %08llX): guest SEH unwinding is not supported",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.lr);
}

GUEST_EXPORT(__C_specific_handler)
{
    Return(ctx, 1); // ExceptionContinueSearch
}

GUEST_EXPORT(KeBugCheck)
{
    Log("KeBugCheck(%08X) (lr %08llX)", ctx.r3.u32, ctx.lr);
    __builtin_debugtrap(); // crash handler logs the call stack
}

GUEST_EXPORT(KeBugCheckEx)
{
    Fatal("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X) (lr %08llX)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.lr);
}

GUEST_EXPORT(DbgBreakPoint)
{
    Log("DbgBreakPoint (lr %08llX)", ctx.lr);
}

GUEST_EXPORT(DbgPrint)
{
    std::string s = FormatGuestStack(ctx, ctx.r3.u32, 1);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    Log("DbgPrint: %s", s.c_str());
    Return(ctx, 0);
}

GUEST_EXPORT(HalReturnToFirmware)
{
    Log("HalReturnToFirmware(%u): title asked to exit", ctx.r3.u32);
    void WheelStop();
    WheelStop();
    std::_Exit(0);
}

// printf family -------------------------------------------------------------

namespace
{
template <typename NextArg>
std::string Format(const char* fmt, NextArg next)
{
    std::string out;
    for (const char* p = fmt; *p; p++)
    {
        if (*p != '%')
        {
            out += *p;
            continue;
        }
        const char* start = p++;
        if (*p == '%')
        {
            out += '%';
            continue;
        }
        std::string spec = "%";
        while (*p && strchr("-+ #0", *p))
            spec += *p++;
        if (*p == '*')
        {
            spec += std::to_string(int32_t(next()));
            p++;
        }
        while (*p >= '0' && *p <= '9')
            spec += *p++;
        if (*p == '.')
        {
            spec += *p++;
            if (*p == '*')
            {
                spec += std::to_string(int32_t(next()));
                p++;
            }
            while (*p >= '0' && *p <= '9')
                spec += *p++;
        }
        int longs = 0;
        bool wide = false, narrow = false;
        for (;; p++)
        {
            if (*p == 'l')
                longs++;
            else if (*p == 'h')
                narrow = true;
            else if (*p == 'w')
                wide = true;
            else if (*p == 'I' && p[1] == '6' && p[2] == '4')
            {
                longs = 2;
                p += 2;
            }
            else if (*p == 'z' || *p == 't' || *p == 'j')
                ;
            else
                break;
        }
        char buffer[512];
        const char conv = *p;
        switch (conv)
        {
        case 'd':
        case 'i':
            if (longs >= 2)
                snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)next());
            else
                snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int32_t(next()));
            out += buffer;
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            if (longs >= 2)
                snprintf(buffer, sizeof(buffer), (spec + "ll" + conv).c_str(), (unsigned long long)next());
            else
                snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), uint32_t(next()));
            out += buffer;
            break;
        case 'p':
            snprintf(buffer, sizeof(buffer), "%08X", uint32_t(next()));
            out += buffer;
            break;
        case 'c':
            out += char(next());
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        {
            uint64_t bits = next();
            double d;
            memcpy(&d, &bits, 8);
            snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), d);
            out += buffer;
            break;
        }
        case 's':
        case 'S':
        {
            const uint32_t s = uint32_t(next());
            std::string str;
            if (!s)
                str = "(null)";
            else if ((conv == 'S' && !narrow) || wide || longs)
            {
                for (uint32_t i = 0; uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(s)[i]); i++)
                    str += c < 0x80 ? char(c) : '?';
            }
            else
                str = GuestPtr<const char>(s);
            snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), str.c_str());
            out += buffer;
            break;
        }
        case 'n':
            next();
            break;
        default:
            out.append(start, p + 1);
            break;
        }
        if (!*p)
            break;
    }
    return out;
}

void WriteGuest(uint32_t dest, const std::string& s, uint32_t capacity = 0xFFFFFFFF)
{
    const uint32_t n = uint32_t(std::min<size_t>(s.size(), capacity ? capacity - 1 : 0));
    memcpy(GuestPtr(dest), s.data(), n);
    if (capacity)
        *GuestPtr<char>(dest + n) = 0;
}
}

std::string FormatGuestStack(PPCContext& ctx, uint32_t fmt, int firstArg)
{
    int index = firstArg;
    return Format(GuestPtr<const char>(fmt), [&]() -> uint64_t {
        const int i = index++;
        switch (i)
        {
        case 0: return ctx.r3.u64;
        case 1: return ctx.r4.u64;
        case 2: return ctx.r5.u64;
        case 3: return ctx.r6.u64;
        case 4: return ctx.r7.u64;
        case 5: return ctx.r8.u64;
        case 6: return ctx.r9.u64;
        case 7: return ctx.r10.u64;
        }
        return LoadBE64(ctx.r1.u32 + 0x50 + (i - 8) * 8);
    });
}

std::string FormatGuestVaList(uint32_t fmt, uint32_t vaList)
{
    uint32_t slot = vaList;
    return Format(GuestPtr<const char>(fmt), [&]() -> uint64_t {
        const uint64_t v = LoadBE64(slot);
        slot += 8;
        return v;
    });
}

GUEST_EXPORT(sprintf)
{
    const std::string s = FormatGuestStack(ctx, ctx.r4.u32, 2);
    WriteGuest(ctx.r3.u32, s);
    Return(ctx, s.size());
}

GUEST_EXPORT(vsprintf)
{
    const std::string s = FormatGuestVaList(ctx.r4.u32, ctx.r5.u32);
    WriteGuest(ctx.r3.u32, s);
    Return(ctx, s.size());
}

GUEST_EXPORT(_vsnprintf)
{
    const uint32_t count = ctx.r4.u32;
    const std::string s = FormatGuestVaList(ctx.r5.u32, ctx.r6.u32);
    const uint32_t n = uint32_t(std::min<size_t>(s.size(), count));
    memcpy(GuestPtr(ctx.r3.u32), s.data(), n);
    if (n < count)
        *GuestPtr<char>(ctx.r3.u32 + n) = 0;
    Return(ctx, s.size() > count ? uint64_t(-1) : s.size());
}

GUEST_EXPORT(vswprintf)
{
    // Wide format: narrow it, format, widen the result.
    // In a wide format %s is a wide string and %S a narrow one.
    std::string fmt;
    bool inSpec = false, sized = false;
    for (uint32_t i = 0; uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(ctx.r4.u32)[i]); i++)
    {
        if (inSpec && !sized && c == 's')
            fmt += 'l';
        else if (inSpec && !sized && c == 'S')
        {
            fmt += "hs";
            inSpec = false;
            continue;
        }
        if (inSpec && (c == 'l' || c == 'h' || c == 'w'))
            sized = true;
        if (c == '%')
        {
            inSpec = !inSpec;
            sized = false;
        }
        else if (inSpec && isalpha(c) && c != 'l' && c != 'h' && c != 'w' && c != 'I')
            inSpec = false;
        fmt += char(c);
    }
    const uint32_t tmp = GuestAlloc(Heap::Virtual, uint32_t(fmt.size() + 1));
    memcpy(GuestPtr(tmp), fmt.c_str(), fmt.size() + 1);
    std::string s = FormatGuestVaList(tmp, ctx.r5.u32);
    GuestFree(tmp);
    auto* out = GuestPtr<uint16_t>(ctx.r3.u32);
    for (size_t i = 0; i < s.size(); i++)
        out[i] = __builtin_bswap16(uint8_t(s[i]));
    out[s.size()] = 0;
    Return(ctx, s.size());
}
