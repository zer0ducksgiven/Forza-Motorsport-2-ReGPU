#pragma once
// Helpers for implementing guest imports: PPC_FUNC(__imp__Name) receives the
// caller's registers. Arguments are r3-r10, then 8-byte stack slots.
#include <string>

#include <ppc_context.h>

#include "kernel/objects.h"
#include "log.h"
#include "memory.h"

// Every import goes through TraceImport and TraceImportReturn (FM2_TRACE_IMPORTS=<seconds>: log calls made
// after that time, a few per name, with what they return; FM2_TRACE_IMPORTS_MATCH=Xam,XNotify: only names
// containing one of those).
void TraceImport(const char* name, PPCContext& ctx);
void TraceImportReturn(const char* name, PPCContext& ctx);
#define GUEST_EXPORT(name)                                         static void Impl_##name(PPCContext& __restrict ctx, uint8_t* base);     PPC_FUNC(__imp__##name)                                        {                                                                  TraceImport(#name, ctx);                                       Impl_##name(ctx, base);                                        TraceImportReturn(#name, ctx);                             }                                                              static void Impl_##name(PPCContext& __restrict ctx, uint8_t* base)

inline uint32_t StackArg(PPCContext& ctx, int index) // index 8 = 9th argument
{
    return LoadBE32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
}

inline void Return(PPCContext& ctx, uint64_t value)
{
    ctx.r3.u64 = value;
}

inline void StoreIfNonNull(uint32_t address, uint32_t value)
{
    if (address)
        StoreBE32(address, value);
}

inline std::string GuestString(uint32_t address)
{
    return address ? std::string(GuestPtr<const char>(address)) : std::string();
}

// ANSI_STRING / STRING: { u16 Length, u16 MaximumLength, u32 Buffer }.
inline std::string GuestAnsiString(uint32_t stringPtr)
{
    if (!stringPtr)
        return {};
    const uint16_t length = __builtin_bswap16(*GuestPtr<uint16_t>(stringPtr));
    const uint32_t buffer = LoadBE32(stringPtr + 4);
    return buffer ? std::string(GuestPtr<const char>(buffer), length) : std::string();
}

// OBJECT_ATTRIBUTES: { u32 RootDirectory, u32 ObjectName, u32 Attributes }.
inline std::string ObjectAttributesName(uint32_t attributes, uint32_t* root = nullptr)
{
    if (!attributes)
        return {};
    if (root)
        *root = LoadBE32(attributes);
    return GuestAnsiString(LoadBE32(attributes + 4));
}

// XOVERLAPPED completion: result in InternalLow, then signal hEvent.
void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t extendedError = 0, uint32_t length = 0);

uint32_t TscFrequency();
// The Xbox 360's timebase (mftb, KeQueryPerformanceFrequency): 49.875 MHz.
constexpr uint32_t kGuestTimebaseHz = 49875000;
extern "C++" uint64_t Fm2Timebase();

// Guest printf: arguments from registers/stack starting at firstArg (0 = r3),
// or from a guest va_list (8-byte slots).
std::string FormatGuestStack(PPCContext& ctx, uint32_t fmt, int firstArg);
std::string FormatGuestVaList(uint32_t fmt, uint32_t vaList);

constexpr uint32_t X_ERROR_SUCCESS = 0;
constexpr uint32_t X_ERROR_CANCELLED = 0x4C7;
constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t X_ERROR_NO_MORE_FILES = 18;
constexpr uint32_t X_ERROR_FUNCTION_FAILED = 1627;
constexpr uint32_t X_ERROR_IO_PENDING = 997;
constexpr uint32_t X_ERROR_NOT_FOUND = 1168;
constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t X_ERROR_ACCESS_DENIED = 5;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 1317;
