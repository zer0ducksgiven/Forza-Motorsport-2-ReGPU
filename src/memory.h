#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// The guest's 4 GB address space, reserved and committed in one block. Guest
// address A lives at g_base + A.
inline uint8_t* g_base = nullptr;

template <typename T = void>
inline T* GuestPtr(uint32_t address)
{
    return reinterpret_cast<T*>(g_base + address);
}

inline uint32_t GuestAddr(const void* host)
{
    return uint32_t(static_cast<const uint8_t*>(host) - g_base);
}

template <typename T>
struct be
{
    T raw;

    static T Swap(T v)
    {
        if constexpr (sizeof(T) == 1)
            return v;
        else if constexpr (sizeof(T) == 2)
        {
            uint16_t x;
            memcpy(&x, &v, 2);
            x = __builtin_bswap16(x);
            memcpy(&v, &x, 2);
            return v;
        }
        else if constexpr (sizeof(T) == 4)
        {
            uint32_t x;
            memcpy(&x, &v, 4);
            x = __builtin_bswap32(x);
            memcpy(&v, &x, 4);
            return v;
        }
        else
        {
            uint64_t x;
            memcpy(&x, &v, 8);
            x = __builtin_bswap64(x);
            memcpy(&v, &x, 8);
            return v;
        }
    }

    be() = default;
    be(T v) : raw(Swap(v)) {}
    operator T() const { return Swap(raw); }
    be& operator=(T v)
    {
        raw = Swap(v);
        return *this;
    }
    T get() const { return Swap(raw); }
};

inline uint32_t LoadBE32(uint32_t address) { return __builtin_bswap32(*GuestPtr<uint32_t>(address)); }
inline void StoreBE32(uint32_t address, uint32_t v) { *GuestPtr<uint32_t>(address) = __builtin_bswap32(v); }
inline uint64_t LoadBE64(uint32_t address) { return __builtin_bswap64(*GuestPtr<uint64_t>(address)); }
inline void StoreBE64(uint32_t address, uint64_t v) { *GuestPtr<uint64_t>(address) = __builtin_bswap64(v); }

void MemoryInit();

// Guest heaps. Memory is already committed; these only hand out ranges, and
// zero them. All are thread-safe.
enum class Heap
{
    Virtual,  // NtAllocateVirtualMemory, runtime objects: 0x40000000-0x7EFFFFFF
    Physical, // MmAllocatePhysicalMemoryEx (GPU-visible), 0xA0000000 view
};

uint32_t GuestAlloc(Heap heap, uint32_t size, uint32_t alignment = 16);
// Physical memory is visible at 0xA0000000 (64 KB pages), 0xC0000000 (16 MB
// pages) and 0xE0000000 (4 KB pages); all three alias the same 512 MB.
uint32_t GuestAllocPhysical(uint32_t size, uint32_t alignment, uint32_t viewBase);
uint32_t PhysicalViewBase(uint32_t protect);
// Returns false if address is not the start of a live allocation.
bool GuestFree(uint32_t address);
// Size of the allocation that contains address, 0 if none.
uint32_t GuestAllocationSize(uint32_t address, uint32_t* start = nullptr);

// File-mapping handle of the 512 MB physical memory section (for GPU access).
void* PhysicalSection();

// Write tracking for physical memory (the GPU's vertex arena): WriteWatchArm makes a range's
// pages read-only in every view; the first write to one (guest code or host code) faults, the
// page is writable again and counts as written. WriteWatchClean: no page of the range written
// since it was armed. Ranges under 16 MB (also seen through the writeback window) never count
// as clean. Physical offsets (address & 0x1FFFFFFF).
void WriteWatchArm(uint32_t offset, uint32_t bytes);
bool WriteWatchClean(uint32_t offset, uint32_t bytes);
// Before a write the kernel makes for us (ReadFile into guest memory: it fails on a read-only
// page instead of faulting): the range's armed pages become writable and count as written.
void GuestWillWrite(uint32_t address, uint32_t bytes);
// Write-watch work since the last call (FM2_PERF slow-frame lines): faults handled, VirtualProtect
// calls and time in them, time waiting for the watch lock.
struct WriteWatchStats
{
    uint32_t faults, protects;
    double protectMs, lockWaitMs;
};
WriteWatchStats WriteWatchTakeStats();
