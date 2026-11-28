// Benchmark: a frozen start grid for pixel comparisons, then a race with the game's AI driving the player's car
// (the FM2 version of the FM3/PGR3 ports' grid test and benchmark race). Booted with --racenow=race=N,ai=1 (main.cpp:
// straight into an Exhibition race, the driver AI racing player 1's car; tools/regpu/bench.sh does it).
//
// FM2_AI_BENCH=<seconds> turns it on: the race's measured length (0: no race phase). Then:
// 1. RaceNow starts the race with no countdown (its countdown=1 path stops at the Starting Grid menu, and after Start
//    Race the AI doesn't drive). The moment the loading screen has gone and the cars still wait on the grid (found by
//    the frames' draw counts, BenchSwap), the world is frozen (FM2_FREEZE_TIME, kernel/runtime.cpp: the main loop's dt stops), so the cars
//    sit on their grid spots and the camera stays put, while every frame is still drawn. (The game's "driving" state,
//    hooks/wheel_driving.cpp, comes on during the race's load, under the loading screen: too early.)
// 2. FM2_BENCH_GRID_SETTLE seconds (default 4) later "bench: grid shot" is logged (the script takes its screenshot
//    for the pixel comparison then) and the frozen grid is measured for FM2_BENCH_GRID seconds (default 15; 0: no grid).
// 3. The world goes on (no time jump), the AI drives; FM2_BENCH_SETTLE seconds (default 5) later the race is measured
//    for FM2_AI_BENCH seconds.
// 4. Each phase's frame times are logged ("bench: grid frames ...", "bench: race frames ..."), then the game's window
//    is closed (as the user would: the caches are saved).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "regpu_host/bench.h"
#include "regpu/driver.h"
#include "live_switch.h"
#include "log.h"

void PgoWrite(); // pgo.cpp (a -DFM2_PGO=gen build: the profile of this run; else nothing)
extern std::atomic<bool> g_benchRecording; // (ReGPU-V's; kernel/thread.cpp's FM2_SAMPLE_BENCH=1 samples only while set)

namespace
{
using Clock = std::chrono::steady_clock;

double EnvSeconds(const char* name, double fallback)
{
    const char* v = getenv(name);
    return v && *v ? std::max(0.0, atof(v)) : fallback;
}

double BenchmarkSeconds()
{
    static const double s = EnvSeconds("FM2_AI_BENCH", -1.0);
    return s;
}

std::mutex g_mutex;
bool g_recording = false;
std::vector<double> g_ms; // frame times of the phase being measured
Clock::time_point g_last{};
std::atomic<bool> g_started{ false };
// Frame-locked time: from boot to the end of the frozen grid, each frame shown adds 1/60 s (FM2_BENCH_FIXED=0: real time).
std::atomic<int64_t> g_tickNs{ -1 }, g_tickEndNs{ 0 };
bool FixedTime()
{
    static const bool on = BenchmarkSeconds() >= 0 && !(getenv("FM2_BENCH_FIXED") && atoi(getenv("FM2_BENCH_FIXED")) == 0);
    return on;
}

void StartPhase()
{
    g_benchRecording = true;
    std::lock_guard lock(g_mutex);
    g_ms.clear();
    g_ms.reserve(100000);
    g_last = Clock::now();
    g_recording = true;
}

// Ends the phase and logs its frame times (the per-frame stats the PGR3/FM3 tools read).
void EndPhase(const char* name, double seconds)
{
    std::vector<double> ms;
    g_benchRecording = false;
    {
        std::lock_guard lock(g_mutex);
        g_recording = false;
        ms.swap(g_ms);
    }
    if (ms.empty())
    {
        Log("bench: %s: no frames", name);
        return;
    }
    double total = 0;
    for (double v : ms)
        total += v;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    auto at = [&](double q) { return sorted[std::min(sorted.size() - 1, size_t(q * double(sorted.size())))]; };
    // 1% / 0.1% lows: the average fps of the slowest 1% / 0.1% of frames.
    auto low = [&](double share) {
        const size_t n = std::max<size_t>(1, size_t(share * double(sorted.size())));
        double t = 0;
        for (size_t i = sorted.size() - n; i < sorted.size(); i++)
            t += sorted[i];
        return 1000.0 * double(n) / t;
    };
    const double median = at(0.5);
    size_t over2x = 0, over16 = 0, over33 = 0;
    for (double v : ms)
        over2x += v > 2 * median, over16 += v > 16.7, over33 += v > 33.4;
    Log("bench: %s frames %zu in %.1f s, avg %.1f fps (%.2f ms), 1%% low %.1f fps, 0.1%% low %.1f fps, p99 %.2f ms, p99.9 %.2f ms, "
        "worst %.2f ms, median %.2f ms, stutters: over 2x median %zu, over 16.7 ms %zu, over 33.4 ms %zu",
        name, ms.size(), seconds, 1000.0 * double(ms.size()) / total, total / double(ms.size()), low(0.01), low(0.001), at(0.99), at(0.999),
        sorted.back(), median, over2x, over16, over33);
}

BOOL CALLBACK CloseOurWindow(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void Run()
{
    const double gridSettle = EnvSeconds("FM2_BENCH_GRID_SETTLE", 4.0), grid = EnvSeconds("FM2_BENCH_GRID", 15.0);
    const double settle = EnvSeconds("FM2_BENCH_SETTLE", 5.0), race = BenchmarkSeconds();
    auto wait = [](double s) { std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(s * 1000))); };
    if (grid > 0)
    {
        LiveSwitch::Set("FM2_FREEZE_TIME", true);
        Log("bench: frozen on the grid at the race's start");
        wait(gridSettle);
        Log("bench: shot grid"); // (tools/regpu/bench.sh takes a screenshot at every "bench: shot <name>")
        wait(1.0);
        StartPhase();
        Log("bench: grid measuring %.0f s", grid);
        wait(grid);
        EndPhase("grid", grid);
        // FM2_BENCH_AB=<switch> (REGPU_* or FM2_*, a live one): A/B on the frozen grid, the same picture both ways (cars,
        // camera and time stand still), FM2_BENCH_AB_ROUNDS rounds (default 3) of off then on, FM2_BENCH_AB_SECONDS each
        // (default 8), a screenshot of each state (shot off<n> / on<n>: tools/regpu/pixdiff.py) and its frame times.
        if (const char* ab = getenv("FM2_BENCH_AB"); ab && *ab)
        {
            const std::string name = ab;
            const int rounds = int(EnvSeconds("FM2_BENCH_AB_ROUNDS", 3));
            const double each = EnvSeconds("FM2_BENCH_AB_SECONDS", 8);
            auto set = [&](bool on) {
                if (name.rfind("REGPU_", 0) == 0)
                    regpu::driver::SetSwitch(name.c_str(), on);
                else
                    LiveSwitch::Set(name, on);
                _putenv_s(name.c_str(), on ? "1" : "0");
            };
            // Pictures first, back to back (the frozen world still creeps by ~3 ms of game time a second: dt has a
            // 1e-5 s floor further in): off, 0.5 s, off again (the noise between two shots of one state), on, 0.5 s,
            // on again. Then the frame times, off and on in turn.
            auto shot = [&](const char* tag) {
                Log("bench: shot %s", tag);
                wait(0.7); // (the script's screenshot takes a frame or two)
            };
            set(false);
            wait(0.5);
            shot("offA");
            shot("offB");
            set(true);
            wait(0.3);
            shot("onA");
            shot("onB");
            for (int r = 1; r <= rounds; r++)
                for (int on = 0; on <= 1; on++)
                {
                    set(on != 0);
                    wait(1.0); // (a frame or two for it to take, and the frame times to settle)
                    StartPhase();
                    wait(each);
                    EndPhase(("ab " + name + " " + (on ? "on" : "off") + std::to_string(r)).c_str(), each);
                }
            set(false);
        }
        g_tickEndNs = g_tickNs.load();
        g_tickNs = -1; // (real time from here: the race)
        LiveSwitch::Set("FM2_FREEZE_TIME", false);
        Log("bench: grid done, the race goes on");
    }
    if (race > 0)
    {
        wait(settle);
        StartPhase();
        Log("bench: race measuring %.0f s (the AI drives)", race);
        wait(race);
        EndPhase("race", race);
        // Screenshots after the measurement (each stalls a frame by ~70 ms: inside it, they were the race's worst
        // frames), 3 s apart: what was measured, the AI driving.
        for (int q = 1; q <= 3; q++)
        {
            Log("bench: shot race%d", q);
            wait(3.0);
        }
    }
    PgoWrite();
    Log("bench: done");
    if (!getenv("FM2_BENCH_KEEP"))
        EnumWindows(CloseOurWindow, 0);
}
} // namespace

void BenchDriving(bool) {}

uint32_t RendererLastFrameDraws(); // ReGPU-V: the last frame's draws

int64_t BenchTickNs()
{
    return FixedTime() ? g_tickNs.load(std::memory_order_relaxed) : -1;
}
int64_t BenchTickEndNs()
{
    return g_tickEndNs.load(std::memory_order_relaxed);
}

void BenchSwap()
{
    if (BenchmarkSeconds() < 0)
        return;
    // Frame-locked time: 1/60 s a frame until the grid freezes (then it stays: frozen).
    if (FixedTime() && !g_started.load(std::memory_order_relaxed))
    {
        static int64_t frames = 0;
        // (From 5 s: ahead of the boot's real time at the first frame, ~1.7 s, so time never goes back; the same start
        // in every run.)
        g_tickNs.store(5000000000LL + int64_t(++frames * 1000000000LL / 60), std::memory_order_relaxed);
    }
    // The start grid, found by draw counts (FM2_BENCH_TRACE_DRAWS=1 shows them; Nissan Speedway, 720p): the race is
    // drawn under the loading screen first (~2,900 draws a frame, steady), then the loading screen goes (~440 fewer,
    // still steady: the cars wait on their grid spots for about 40 frames with the race clock at 0), then the race
    // starts and the counts move. Frozen on the second frame after the drop (on the renderer's thread, at this
    // frame's end); the rest runs on a thread of its own. If no drop comes within 600 frames, frozen there anyway.
    if (!g_started.load(std::memory_order_relaxed))
    {
        static const uint32_t sceneDraws = uint32_t(EnvSeconds("FM2_BENCH_SCENE_DRAWS", 800));
        static int run = 0, below = 0, frames = 0;
        static uint32_t plateau = 0;
        const uint32_t d = RendererLastFrameDraws();
        if (run < 3)
        {
            run = d >= sceneDraws ? run + 1 : 0;
            plateau = std::max(plateau, d);
        }
        else
        {
            frames++;
            if (frames < 8)
                plateau = std::max(plateau, d); // (the first frames are a little busier)
            // After the drop (the loading screen's outro), the counts alternate between two steady values while it
            // fades; the first frames off that pair (by more than 20) are the race view coming up with its HUD.
            static uint32_t pair[2] = {}, steady = 0;
            static int phase = 0, off = 0;
            if (phase == 0)
            {
                below = d + 300 < plateau ? below + 1 : 0;
                if (below >= 2)
                    phase = 1;
            }
            else
            {
                const bool inPair = (pair[0] && (d + 20 >= pair[0] && d <= pair[0] + 20)) || (pair[1] && (d + 20 >= pair[1] && d <= pair[1] + 20));
                if (steady < 2)
                    pair[steady++] = d;
                else
                    off = inPair ? 0 : off + 1;
            }
            if ((off >= 2 || frames >= 600) && !g_started.exchange(true))
            {
                if (EnvSeconds("FM2_BENCH_GRID", 15.0) > 0)
                    LiveSwitch::Set("FM2_FREEZE_TIME", true);
                Log("bench: race view up %d frames after the scene (%u -> %u, %u/%u -> %u draws a frame)%s", frames, plateau, pair[0], pair[0],
                    pair[1], d, off >= 2 ? "" : " (not found: frozen anyway)");
                std::thread(Run).detach();
            }
        }
    }
    // FM2_BENCH_TRACE_DRAWS=1: every frame's draw count for 400 frames after the scene is up (finding the trigger).
    static const bool traceDraws = getenv("FM2_BENCH_TRACE_DRAWS") != nullptr;
    static int traced = 0;
    if (traceDraws && g_started && traced < 400)
    {
        static uint32_t last = 0;
        const uint32_t d = RendererLastFrameDraws();
        if (d != last || (traced % 20) == 0)
            Log("bench: frame %d after the scene: %u draws", traced, d);
        last = d;
        traced++;
    }
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    if (g_recording)
        g_ms.push_back(std::chrono::duration<double, std::milli>(now - g_last).count());
    g_last = now;
}
