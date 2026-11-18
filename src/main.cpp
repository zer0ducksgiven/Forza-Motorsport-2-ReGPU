#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
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

#if defined(FM2_VULKAN)
bool VulkanProbe();
#endif
// --racenow[=payload] (or --racenow "payload"): boot straight into an Exhibition race. No menu is shown: the
// boot flow's start screen is replaced by Flows/RaceNow.xml (kernel/zip_overlay.cpp), which loads player 1's
// profile, sets the race up the way the Arcade menus would and launches it (loading screen, then the race, with
// no 3-2-1 countdown). Auto-drive (hooks/autodrive.h) and the FPS meter (gpu/fps_meter.h) are on.
// Payload, comma separated, every key optional (blank = defaults):
//   race=N    Nth Exhibition race (0 = the first, Nissan Speedway; the races in Arcade order)
//   class=N   car class id (CarClasses.Id; default 1, the Arcade menu's default)
//   car=N     a Data_Car id; default the first car of the class in the profile's arcade garage
//   drive=0   no auto-drive        fps=0   no FPS meter        countdown=1   keep the countdown
//   hyper=P   a Hyper preset: mild, low, extreme or potato (gpu/hyper.h)
static void RaceNow(const std::string& payload)
{
    std::map<std::string, std::string> kv;
    for (size_t s = 0; s < payload.size();)
    {
        size_t e = payload.find(',', s);
        if (e == std::string::npos)
            e = payload.size();
        const std::string item = payload.substr(s, e - s);
        if (const size_t eq = item.find('='); eq != std::string::npos)
            kv[item.substr(0, eq)] = item.substr(eq + 1);
        s = e + 1;
    }
    auto set = [&](const char* key, const char* env) {
        if (kv.count(key))
            _putenv_s(env, std::to_string(atoi(kv[key].c_str())).c_str());
    };
    _putenv_s("FM2_RACENOW", "1");
    set("race", "FM2_RACENOW_RACE");
    set("class", "FM2_RACENOW_CLASS");
    set("car", "FM2_RACENOW_CAR");
    set("ai", "FM2_RACENOW_AI"); // ai=1: the game's driver AI races the player's car (zip_overlay.cpp)
    if (kv.count("countdown") && kv["countdown"] == "1")
        _putenv_s("FM2_RACENOW_COUNTDOWN", "1");
    _putenv_s("FM2_NO_LAUNCHER", "1");
    _putenv_s("FM2_SKIP_INTRO", "1");
    if (!kv.count("drive") || kv["drive"] != "0")
        _putenv_s("FM2_AUTODRIVE", "1");
    if (!kv.count("fps") || kv["fps"] != "0")
        _putenv_s("FM2_FPS_OVERLAY", "1");
    if (kv.count("hyper")) // a Hyper preset (gpu/hyper.h)
        _putenv_s("REGPU_HYPER", kv["hyper"].c_str());
    Log("racenow: payload \"%s\"", payload.c_str());
}

int main(int argc, char** argv)
{
    CrashHandlerInit();
#if defined(FM2_VULKAN)
    if (getenv("FM2_VULKAN_PROBE"))
        return VulkanProbe() ? 0 : 1;
#endif
    std::filesystem::path gameDir = "game";
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--racenow")
        {
            // A payload follows when the next argument is one (has "=" and isn't a path that exists).
            std::string payload;
            if (i + 1 < argc && strchr(argv[i + 1], '=') && !std::filesystem::exists(argv[i + 1]))
                payload = argv[++i];
            RaceNow(payload);
        }
        else if (arg.rfind("--racenow=", 0) == 0)
            RaceNow(arg.substr(10));
        else if (arg.rfind("--", 0) != 0)
            gameDir = arg;
    }
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
