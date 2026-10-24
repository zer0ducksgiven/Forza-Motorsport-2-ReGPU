// Memory management and module (XEX) queries.
#include "kernel/imports.h"
#include "kernel/wheel_ff.h"
#include "live_switch.h"
#include "loader.h"

namespace
{
constexpr uint32_t MEM_COMMIT_ = 0x1000;
constexpr uint32_t MEM_RESERVE_ = 0x2000;
constexpr uint32_t MEM_DECOMMIT_ = 0x4000;
constexpr uint32_t MEM_RELEASE_ = 0x8000;
constexpr uint32_t MEM_LARGE_PAGES_ = 0x20000000;
constexpr uint32_t PAGE_READWRITE_ = 0x04;
}

GUEST_EXPORT(NtAllocateVirtualMemory)
{
    // (baseAddress*, regionSize*, allocationType, protect, debugMemory)
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    const uint32_t requested = LoadBE32(basePtr);
    const uint32_t pageSize = (type & MEM_LARGE_PAGES_) ? 0x10000 : 0x1000;
    const uint32_t size = (LoadBE32(sizePtr) + pageSize - 1) & ~(pageSize - 1);

    if (requested && !(type & MEM_RESERVE_))
    {
        // Committing part of an earlier reservation: already backed.
        if (!GuestAllocationSize(requested))
        {
            Return(ctx, X_STATUS_INVALID_PARAMETER);
            return;
        }
        StoreBE32(sizePtr, size);
        Return(ctx, X_STATUS_SUCCESS);
        return;
    }
    if (requested)
        LOG_ONCE("NtAllocateVirtualMemory at a fixed address (%08X) is not supported; allocating elsewhere", requested);

    const uint32_t address = GuestAlloc(Heap::Virtual, size, 0x10000);
    if (!address)
    {
        Return(ctx, X_STATUS_NO_MEMORY);
        return;
    }
    StoreBE32(basePtr, address);
    StoreBE32(sizePtr, size);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtFreeVirtualMemory)
{
    // (baseAddress*, regionSize*, freeType, debugMemory)
    const uint32_t address = LoadBE32(ctx.r3.u32);
    if (ctx.r5.u32 & MEM_RELEASE_)
    {
        Return(ctx, GuestFree(address) ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER);
        return;
    }
    // MEM_DECOMMIT: memory stays backed; nothing to do.
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtQueryVirtualMemory)
{
    // (baseAddress, MEMORY_BASIC_INFORMATION*, debugMemory)
    const uint32_t address = ctx.r3.u32, info = ctx.r4.u32;
    uint32_t start = 0;
    const uint32_t size = GuestAllocationSize(address, &start);
    StoreBE32(info + 0x00, size ? start : address & ~0xFFFu);
    StoreBE32(info + 0x04, size ? start : 0);
    StoreBE32(info + 0x08, size ? PAGE_READWRITE_ : 0);
    StoreBE32(info + 0x0C, size ? size : 0x1000);
    StoreBE32(info + 0x10, size ? MEM_COMMIT_ : 0x10000); // MEM_FREE
    StoreBE32(info + 0x14, size ? PAGE_READWRITE_ : 1);   // PAGE_NOACCESS
    StoreBE32(info + 0x18, size ? 0x20000 : 0);           // MEM_PRIVATE
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(MmAllocatePhysicalMemoryEx)
{
    // (flags, size, protect, minAddress, maxAddress, alignment)
    const uint32_t size = ctx.r4.u32, protect = ctx.r5.u32;
    const uint32_t alignment = std::max<uint32_t>(ctx.r8.u32, 0x1000);
    Return(ctx, GuestAllocPhysical(size, alignment, PhysicalViewBase(protect)));
}

GUEST_EXPORT(MmFreePhysicalMemory)
{
    // (type, address)
    GuestFree(ctx.r4.u32);
}

GUEST_EXPORT(MmGetPhysicalAddress)
{
    Return(ctx, ctx.r3.u32 & 0x1FFFFFFF);
}

GUEST_EXPORT(MmQueryAddressProtect)
{
    Return(ctx, PAGE_READWRITE_);
}

GUEST_EXPORT(MmSetAddressProtect) {}

GUEST_EXPORT(MmQueryAllocationSize)
{
    static LiveSwitch trace("FM2_TRACE_PHYS");
    if (trace)
    {
        uint32_t start = 0;
        const uint32_t size = GuestAllocationSize(ctx.r3.u32, &start);
        Log("phys: query size of %08X -> %X (block %08X, lr %08llX)", ctx.r3.u32, size, start, ctx.lr);
    }
    Return(ctx, GuestAllocationSize(ctx.r3.u32));
}

GUEST_EXPORT(MmCreateKernelStack)
{
    // (size, unknown) -> stack base (high end)
    const uint32_t size = (ctx.r3.u32 + 0xFFF) & ~0xFFFu;
    const uint32_t low = GuestAlloc(Heap::Virtual, size, 0x1000);
    Return(ctx, low ? low + size : 0);
}

GUEST_EXPORT(MmDeleteKernelStack)
{
    // (stackBase, stackLimit)
    GuestFree(ctx.r4.u32);
}

// Modules ------------------------------------------------------------------

GUEST_EXPORT(XexCheckExecutablePrivilege)
{
    const uint32_t privilege = ctx.r3.u32;
    Return(ctx, privilege < 32 ? (g_title.systemFlags >> privilege) & 1 : 0);
}

GUEST_EXPORT(XexGetModuleHandle)
{
    // (name, handle*)
    const std::string name = GuestString(ctx.r3.u32);
    if (name.empty() || name == "default.xex")
    {
        StoreBE32(ctx.r4.u32, g_title.ldrEntry);
        Return(ctx, X_STATUS_SUCCESS);
        return;
    }
    // xboxkrnl.exe and xam.xex: for the wheel's force feedback calls only (kernel/wheel_ff.cpp).
    if ((name == "xboxkrnl.exe" || name == "xam.xex") && WheelFfServed())
    {
        StoreBE32(ctx.r4.u32, name == "xam.xex" ? kXamModuleHandle : kKernelModuleHandle);
        Return(ctx, X_STATUS_SUCCESS);
        return;
    }
    Log("XexGetModuleHandle(\"%s\"): module not available", name.c_str());
    StoreBE32(ctx.r4.u32, 0);
    Return(ctx, X_STATUS_NO_SUCH_FILE);
}

GUEST_EXPORT(XexGetModuleSection)
{
    // (module, name, data*, size*) - XEX resources (header 0x000002FF):
    // { u32 size, then { char name[8], u32 address, u32 size } ... }
    const std::string name = GuestString(ctx.r4.u32);
    const uint32_t resources = XexHeaderField(g_title.xexHeader, 0x000002FF);
    if (resources)
    {
        const uint32_t count = (LoadBE32(resources) - 4) / 16;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t entry = resources + 4 + i * 16;
            if (strncmp(GuestPtr<char>(entry), name.c_str(), 8) == 0)
            {
                StoreIfNonNull(ctx.r5.u32, LoadBE32(entry + 8));
                StoreIfNonNull(ctx.r6.u32, LoadBE32(entry + 12));
                Return(ctx, X_STATUS_SUCCESS);
                return;
            }
        }
    }
    Log("XexGetModuleSection(\"%s\"): not found", name.c_str());
    Return(ctx, X_STATUS_NOT_SUPPORTED);
}

GUEST_EXPORT(XexGetProcedureAddress)
{
    if (const uint32_t address = WheelFfProcedure(ctx.r3.u32, ctx.r4.u32, ctx.lr))
    {
        StoreIfNonNull(ctx.r5.u32, address);
        Return(ctx, X_STATUS_SUCCESS);
        return;
    }
    Log("XexGetProcedureAddress(%08X, %u): not supported", ctx.r3.u32, ctx.r4.u32);
    StoreIfNonNull(ctx.r5.u32, 0);
    Return(ctx, X_STATUS_NOT_SUPPORTED);
}
