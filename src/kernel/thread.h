#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ppc_context.h>

#include "kernel/objects.h"

// Guest-side layout (offsets into KPCR / KTHREAD the title reads directly).
namespace kpcr
{
constexpr uint32_t TlsPtr = 0x000;
constexpr uint32_t PcrPtr = 0x030;
constexpr uint32_t StackBase = 0x070;
constexpr uint32_t StackEnd = 0x074;
constexpr uint32_t CurrentThread = 0x100;
constexpr uint32_t CurrentCpu = 0x10C;
constexpr uint32_t DpcActive = 0x150;
constexpr uint32_t Size = 0x2D8;
}

namespace kthread
{
constexpr uint32_t StackBase = 0x05C;
constexpr uint32_t StackLimit = 0x060;
constexpr uint32_t TlsAddress = 0x064;
constexpr uint32_t ThreadId = 0x14C;
constexpr uint32_t StartAddress = 0x150;
constexpr uint32_t LastError = 0x160;
constexpr uint32_t FiberPtr = 0x164;
constexpr uint32_t CreationFlags = 0x16C;
constexpr uint32_t Size = 0xAB0;
}

struct GuestThread;

struct ThreadObject : KernelObject
{
    ThreadObject() : KernelObject(Kind::Thread) {}
    bool IsSignaled(uint32_t) const override { return exited; }

    std::shared_ptr<GuestThread> thread;
    bool exited = false;
    uint32_t exitCode = 0;
};

struct GuestThread
{
    uint32_t id = 0;
    uint32_t pcr = 0;
    uint32_t kthread = 0;
    uint32_t stackLow = 0;
    uint32_t stackHigh = 0;
    uint32_t tls = 0;
    uint32_t entry = 0;
    uint32_t param = 0;
    uint32_t xapiStartup = 0;
    std::function<void(PPCContext&)> hostEntry; // host-driven thread (DPC-like)
    std::vector<uint32_t> tlsSlots;
    std::string name;
    std::string waitingOn; // debugging

    std::mutex suspendMutex;
    std::condition_variable suspendCv;
    int suspendCount = 0;

    PPCContext* ctx = nullptr; // valid while the thread runs
    void* hostHandle = nullptr;
    std::weak_ptr<ThreadObject> object;
};

void ThreadSystemInit(uint32_t tlsTemplate, uint32_t tlsDataSize, uint32_t tlsSlotCount);

// Starts a guest thread at entry(param) (or xapiStartup(entry, param)).
std::shared_ptr<ThreadObject> CreateGuestThread(uint32_t entry, uint32_t param, uint32_t xapiStartup,
    uint32_t stackSize, bool suspended, const char* name);

// A guest-capable thread whose body is host code (for callbacks the
// console's kernel would run: GPU interrupts, audio frames).
std::shared_ptr<ThreadObject> CreateHostGuestThread(const char* name, std::function<void(PPCContext&)> body);

void ResumeGuestThread(GuestThread& thread);
int SuspendGuestThread(GuestThread& thread);
[[noreturn]] void ExitCurrentGuestThread(uint32_t code);

GuestThread* CurrentGuestThread();
void LogGuestThreads(); // debugging: last call site of every thread
std::string GuestThreadCpuUsage(double wallSeconds); // " t3:85% t7:40%" since the last call
std::shared_ptr<ThreadObject> FindThreadByKthread(uint32_t kthread);

// Calls guest code at address with the current thread's registers.
void GuestCall(PPCContext& ctx, uint32_t address);

// Initialises ctx for running guest code on thread (r1, r13, fpscr).
void InitGuestContext(PPCContext& ctx, GuestThread& thread);

// Per-CPU locks held while a guest thread has MSR[EE] clear and while an
// interrupt callback for that CPU runs: what "interrupts disabled"
// guarantees on the console.
void InterruptLockAcquire(int cpu);
void InterruptLockRelease();

uint32_t TlsAllocSlot();
void TlsFreeSlot(uint32_t slot);
