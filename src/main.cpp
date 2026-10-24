#include <filesystem>
#include <thread>

#include <windows.h>

#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "loader.h"
#include "debug.h"
#include "log.h"
#include "memory.h"

void CrashHandlerInit();
void WheelStart();
void GpuLogState();
void D3DDebugLog();

int main(int argc, char** argv)
{
    CrashHandlerInit();
    std::filesystem::path gameDir = argc > 1 ? argv[1] : "game";
    Log("fm2: game folder %s", std::filesystem::absolute(gameDir).string().c_str());

    MemoryInit();
    LoadTitle(gameDir / "default.xex");
    if (const char* dump = getenv("FM2_DUMP_IMAGE")) // writes the loaded executable image (0x82000000..0x83000000)
    {
        if (FILE* f = fopen(dump, "wb"))
        {
            fwrite(GuestPtr(0x82000000), 1, 0x2000000, f);
            fclose(f);
            Log("wrote the loaded image to %s", dump);
        }
    }
    RuntimeInit(gameDir);
    WheelStart(); // early (the game asks what player 1 holds a second in), after the settings' folder is known
    DebugServerStart(); // FM2_DEBUG=1: tools/fm2ctl.py drives the running game
    if (getenv("FM2_BOOTSCREEN_PREVIEW"))
    {
        void BootScreenPreview();
        BootScreenPreview();
        return 0;
    }
    ThreadSystemInit(g_title.tlsTemplate, g_title.tlsDataSize, g_title.tlsSlotCount);

    void StartThreadProfiler();
    StartThreadProfiler();
    void StartSpikeRecorder();
    StartSpikeRecorder();
    void StartWatch();
    StartWatch();
    auto main = CreateGuestThread(g_title.entryPoint, 0, 0, g_title.stackSize, false, "main");
    char watchdogMs[16] = {};
    const DWORD watchdog = GetEnvironmentVariableA("FM2_WATCHDOG", watchdogMs, 16) ? (atoi(watchdogMs) > 1 ? std::max(50, atoi(watchdogMs)) : 3000) : INFINITE;
    if (getenv("FM2_WATCHW"))
    {
        std::thread([] {
            void ArmWatchpointAll();
            for (;;)
            {
                Sleep(10);
                ArmWatchpointAll();
            }
        }).detach();
    }
    while (WaitForSingleObject(main->thread->hostHandle, watchdog) == WAIT_TIMEOUT)
    {
        Log("watchdog:");
        LogGuestThreads();
        GpuLogState();
        D3DDebugLog();
        void LogFileStats();
        LogFileStats();
    }
    Log("fm2: main thread exited with %08X", main->exitCode);
    return 0;
}
