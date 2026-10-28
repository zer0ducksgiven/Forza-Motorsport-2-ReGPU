#include "memory.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <mutex>

#include <windows.h>

#include "live_switch.h"
#include "log.h"

namespace
{
// First-fit range allocator. Fine for boot; replace with something O(1)
// once allocation shows up in profiles.
class RangeAllocator
{
public:
    void Init(uint32_t start, uint32_t end) { free_[start] = end - start; }

    uint32_t Alloc(uint32_t size, uint32_t alignment)
    {
        size = (size + 15) & ~15u;
        if (!size)
            size = 16;
        std::lock_guard lock(mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it)
        {
            const uint32_t start = it->first;
            const uint64_t end = uint64_t(start) + it->second;
            const uint32_t aligned = (start + alignment - 1) & ~(alignment - 1);
            if (aligned < start || uint64_t(aligned) + size > end)
                continue;

            free_.erase(it);
            if (aligned > start)
                free_[start] = aligned - start;
            if (uint64_t(aligned) + size < end)
                free_[aligned + size] = uint32_t(end - (uint64_t(aligned) + size));
            used_[aligned] = size;
            return aligned;
        }
        return 0xFFFFFFFF;
    }

    bool Free(uint32_t address)
    {
        std::lock_guard lock(mutex_);
        auto it = used_.find(address);
        if (it == used_.end())
            return false;
        uint32_t start = it->first;
        uint32_t size = it->second;
        used_.erase(it);

        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == start + size)
        {
            size += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start)
            {
                prev->second += size;
                return true;
            }
        }
        free_[start] = size;
        return true;
    }

    uint32_t SizeOf(uint32_t address, uint32_t* start)
    {
        std::lock_guard lock(mutex_);
        auto it = used_.upper_bound(address);
        if (it == used_.begin())
            return 0;
        --it;
        if (address >= it->first + it->second)
            return 0;
        if (start)
            *start = it->first;
        return it->second;
    }

private:
    std::mutex mutex_;
    std::map<uint32_t, uint32_t> free_;
    std::map<uint32_t, uint32_t> used_;
};

RangeAllocator g_virtual;  // guest addresses
RangeAllocator g_physical; // physical offsets 0..0x1FFFFFFF

constexpr uint32_t kPhysicalSize = 0x20000000;
HANDLE g_physicalSection = nullptr;
constexpr uint32_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };
// 0x7F000000-0x7FBFFFFF shows physical 0-12 MB (the console's window is 16 MB, but the GPU
// and APU registers are ordinary memory at 0x7FC80000 and 0x7FEA0000 here).
constexpr uint32_t kWritebackWindow = 0x7F000000, kWritebackSize = 0x00C00000;

bool IsPhysicalView(uint32_t address)
{
    return address >= 0xA0000000;
}

void Check(bool ok, const char* what)
{
    if (!ok)
        Fatal("guest memory setup: %s failed (error %lu)", what, GetLastError());
}
}

void MemoryInit()
{
    // One 4 GB placeholder, split so that the three physical views map the
    // same 512 MB section and the rest is ordinary committed memory.
    constexpr uint64_t kTotal = 0x100000000ull;
    auto* base = static_cast<uint8_t*>(VirtualAlloc2(nullptr, nullptr, kTotal, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    Check(base != nullptr, "reserving 4 GB");

    // Split into [0, 7F000000), the writeback window, [7FC00000, A0000000) and three 512 MB
    // view slots.
    Check(VirtualFree(base, 0xA0000000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), "splitting low memory");
    Check(VirtualFree(base, kWritebackWindow, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), "splitting low memory");
    Check(VirtualFree(base + kWritebackWindow, kWritebackSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), "splitting the writeback window");
    for (uint32_t view : { 0xA0000000u, 0xC0000000u })
        Check(VirtualFree(base + view, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), "splitting a physical view");

    Check(VirtualAlloc2(nullptr, base, kWritebackWindow, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == base,
        "committing low memory");
    constexpr uint32_t kRegisterSpace = kWritebackWindow + kWritebackSize;
    Check(VirtualAlloc2(nullptr, base + kRegisterSpace, 0xA0000000 - kRegisterSpace, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
              PAGE_READWRITE, nullptr, 0) == base + kRegisterSpace,
        "committing register space");

    HANDLE section = g_physicalSection = CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_READ | FILE_MAP_WRITE, PAGE_READWRITE, SEC_COMMIT,
        kPhysicalSize, nullptr, nullptr, 0);
    Check(section != nullptr, "creating physical memory");
    for (uint32_t view : kPhysicalViews)
    {
        Check(MapViewOfFile3(section, nullptr, base + view, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == base + view,
            "mapping a physical view");
    }
    // The CPU sees the start of physical memory at 0x7F000000: the GPU writes there (D3D's XPS
    // read pointers: GPU 0xC0000180 is CPU 0x7F000180) and XPS geometry rings sit above it
    // (0x7F100000). The registers at 0x7FC00000 and up stay ordinary memory.
    Check(MapViewOfFile3(section, nullptr, base + kWritebackWindow, 0, kWritebackSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) ==
              base + kWritebackWindow,
        "mapping the writeback window");

    g_base = base;
    // 0x7F000000 and up is the writeback window and GPU register space (MMIO the title writes to).
    g_virtual.Init(0x40000000, kWritebackWindow);
    // Physical: nothing in the first 16 MB, which the writeback window shows (and so no allocation
    // has address 0).
    g_physical.Init(0x01000000, kPhysicalSize);
}

uint32_t PhysicalViewBase(uint32_t protect)
{
    if (protect & 0x20000000) // MEM_LARGE_PAGES: 64 KB pages
        return 0xA0000000;
    if (protect & 0x80000000) // MEM_16MB_PAGES
        return 0xC0000000;
    // 4 KB pages live at 0xE0000000 on the console, offset by 4 KB from the
    // other views; Windows maps at 64 KB granularity, so serve them from the
    // 64 KB view. MmGetPhysicalAddress stays consistent either way.
    return 0xA0000000;
}

uint32_t GuestAlloc(Heap heap, uint32_t size, uint32_t alignment)
{
    if (alignment < 16)
        alignment = 16;
    if (heap == Heap::Physical)
        return GuestAllocPhysical(size, alignment, 0xA0000000);
    const uint32_t address = g_virtual.Alloc(size, alignment);
    if (address == 0xFFFFFFFF)
    {
        Log("guest virtual heap exhausted allocating %#x bytes", size);
        return 0;
    }
    memset(g_base + address, 0, (size + 15) & ~15u);
    return address;
}

uint32_t GuestAllocPhysical(uint32_t size, uint32_t alignment, uint32_t viewBase)
{
    const uint32_t offset = g_physical.Alloc(size, std::max<uint32_t>(alignment, 0x1000));
    if (offset == 0xFFFFFFFF)
    {
        Log("guest physical memory exhausted allocating %#x bytes", size);
        return 0;
    }
    static LiveSwitch trace("FM2_TRACE_PHYS");
    if (trace)
        Log("phys: alloc %08X size %X", viewBase + offset, size);
    memset(g_base + 0xA0000000 + offset, 0, (size + 15) & ~15u);
    return viewBase + offset;
}

bool GuestFree(uint32_t address)
{
    static LiveSwitch trace("FM2_TRACE_PHYS");
    if (trace && IsPhysicalView(address))
        Log("phys: free %08X", address);
    if (IsPhysicalView(address))
        return g_physical.Free(address & 0x1FFFFFFF);
    return g_virtual.Free(address);
}

uint32_t GuestAllocationSize(uint32_t address, uint32_t* start)
{
    if (IsPhysicalView(address))
    {
        uint32_t offset = 0;
        const uint32_t size = g_physical.SizeOf(address & 0x1FFFFFFF, &offset);
        if (start)
            *start = (address & 0xE0000000) + offset;
        return size;
    }
    return g_virtual.SizeOf(address, start);
}

void* PhysicalSection()
{
    return g_physicalSection;
}

namespace
{
// One bit per 4 KB page of physical memory: armed (read-only in every view, not written since).
constexpr uint32_t kPage = 4096, kPages = kPhysicalSize / kPage, kFirstTracked = 0x01000000;
std::atomic<uint64_t> g_armed[kPages / 64];
std::atomic<uint64_t> g_writeSeq{ 1 };
uint64_t g_lastWrite[kPages]; // the g_writeSeq of each page's last write seen (under g_watchLock)
SRWLOCK g_watchLock = SRWLOCK_INIT; // arming and disarming (bit and page protection together)
std::atomic<uint32_t> g_watchFaults{ 0 }, g_watchProtects{ 0 };
std::atomic<uint64_t> g_watchProtectTicks{ 0 }, g_watchWaitTicks{ 0 };

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

void LockExclusive()
{
    if (TryAcquireSRWLockExclusive(&g_watchLock))
        return;
    const uint64_t start = Ticks();
    AcquireSRWLockExclusive(&g_watchLock);
    g_watchWaitTicks += Ticks() - start;
}

bool Armed(uint32_t page)
{
    return (g_armed[page / 64].load(std::memory_order_acquire) >> (page % 64)) & 1;
}

// Whether every view changed (read-only failed with error 87 on an RTX 4070 once the GPU had a heap
// over the section, OpenExistingHeapFromFileMapping).
bool Protect(uint32_t firstPage, uint32_t pages, DWORD protect)
{
    const uint64_t start = Ticks();
    DWORD old;
    bool ok = true;
    for (uint32_t view : kPhysicalViews)
        ok &= VirtualProtect(g_base + view + size_t(firstPage) * kPage, size_t(pages) * kPage, protect, &old) != FALSE;
    g_watchProtectTicks += Ticks() - start;
    g_watchProtects++;
    return ok;
}

// Read-only failed: the watch can't see writes here, so nothing is armed from then on (report #27:
// the arena kept tyre marks from before the race start, a write never faulting).
std::atomic<bool> g_watchUnavailable{ false };

// Disarms the armed pages of [first, first + count): writable again, counted as written.
void Disarm(uint32_t first, uint32_t count)
{
    LockExclusive();
    for (uint32_t p = first; p < first + count;)
    {
        if (!Armed(p))
        {
            p++;
            continue;
        }
        uint32_t end = p;
        const uint64_t seq = ++g_writeSeq;
        while (end < first + count && Armed(end))
        {
            g_armed[end / 64].fetch_and(~(1ull << (end % 64)), std::memory_order_acq_rel);
            g_lastWrite[end] = seq;
            end++;
        }
        Protect(p, end - p, PAGE_READWRITE);
        p = end;
    }
    ReleaseSRWLockExclusive(&g_watchLock);
}

LONG CALLBACK OnWatchedWrite(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* e = info->ExceptionRecord;
    if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2 || e->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t* at = reinterpret_cast<const uint8_t*>(e->ExceptionInformation[1]);
    if (at < g_base + kPhysicalViews[0] || at >= g_base + 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t page = uint32_t((at - g_base) & 0x1FFFFFFF) / kPage;
    // Every view is committed read-write: a write that faults here is to a page armed now, or
    // one another thread is disarming (its protection changes under the lock).
    AcquireSRWLockShared(&g_watchLock);
    const bool armed = Armed(page);
    ReleaseSRWLockShared(&g_watchLock);
    // The whole 64 KB block around it: a write rarely stops at one page (a damaged car body is
    // rewritten in one go), and a fault per 4 KB page with its protection changes cost a 40 ms
    // frame when a car crashed.
    g_watchFaults++;
    if (armed)
        Disarm(page & ~15u, 16);
    else
    {
        // Not ours (or disarmed meanwhile): retry only if the page is writable now.
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(at, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
}

bool WriteWatchArm(uint32_t offset, uint32_t bytes)
{
    static const bool handler = [] { return AddVectoredExceptionHandler(1, OnWatchedWrite) != nullptr; }();
    if (!handler || g_watchUnavailable || !bytes || offset < kFirstTracked || uint64_t(offset) + bytes > kPhysicalSize)
        return false;
    const uint32_t first = offset / kPage, end = (offset + bytes + kPage - 1) / kPage;
    LockExclusive();
    for (uint32_t p = first; p < end;)
    {
        if (Armed(p))
        {
            p++;
            continue;
        }
        uint32_t run = p;
        while (run < end && !Armed(run))
        {
            g_armed[run / 64].fetch_or(1ull << (run % 64), std::memory_order_acq_rel);
            run++;
        }
        if (!Protect(p, run - p, PAGE_READONLY))
        {
            const DWORD error = GetLastError();
            for (uint32_t q = p; q < run; q++)
                g_armed[q / 64].fetch_and(~(1ull << (q % 64)), std::memory_order_acq_rel);
            Protect(p, run - p, PAGE_READWRITE);
            if (!g_watchUnavailable.exchange(true))
                Log("write watch: guest memory can't be made read-only (error %lu): changed vertex buffers are found by hashing", error);
            ReleaseSRWLockExclusive(&g_watchLock);
            return false;
        }
        p = run;
    }
    ReleaseSRWLockExclusive(&g_watchLock);
    return true;
}

bool WriteWatchClean(uint32_t offset, uint32_t bytes, uint64_t since)
{
    if (!bytes || offset < kFirstTracked || uint64_t(offset) + bytes > kPhysicalSize)
        return false;
    const uint32_t first = offset / kPage, end = (offset + bytes + kPage - 1) / kPage;
    AcquireSRWLockShared(&g_watchLock);
    bool clean = true;
    for (uint32_t p = first; p < end && clean; p++)
        clean = Armed(p) && g_lastWrite[p] < since;
    ReleaseSRWLockShared(&g_watchLock);
    return clean;
}

uint64_t WriteWatchSeq()
{
    return g_writeSeq.load(std::memory_order_acquire);
}

void GuestWillWrite(uint32_t address, uint32_t bytes)
{
    if (!bytes || address < kPhysicalViews[0])
        return;
    const uint32_t offset = address & 0x1FFFFFFF;
    const uint32_t first = offset / kPage, end = uint32_t(std::min<uint64_t>(uint64_t(offset) + bytes + kPage - 1, kPhysicalSize) / kPage);
    Disarm(first, end - first);
}

WriteWatchStats WriteWatchTakeStats()
{
    static const double msPerTick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    return { g_watchFaults.exchange(0), g_watchProtects.exchange(0), double(g_watchProtectTicks.exchange(0)) * msPerTick,
        double(g_watchWaitTicks.exchange(0)) * msPerTick };
}
