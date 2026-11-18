// FPS meter (fps_meter.h).
#include "gpu/fps_meter.h"

#include "live_switch.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

namespace
{
using Clock = std::chrono::steady_clock;

// Frames kept for the 1% low: the last 10 minutes at 60 fps.
constexpr size_t kLowWindow = 36000;

struct Meter
{
    std::mutex mutex;
    std::deque<float> recent;          // frame times for the 1% low (ms)
    std::deque<Clock::time_point> now; // presents in the last half second, for the current rate
    double sumMs = 0;                  // since the reset
    uint64_t frames = 0;
    Clock::time_point since = Clock::now();
    Clock::time_point lastLog = Clock::now();
    float lastMs = 0;
    bool resetSwitchSeen = false, resetSwitchWas = false, f10Was = false;
} g_meter;

bool On()
{
    static LiveSwitch regpu("REGPU_FPS_OVERLAY");
    static LiveSwitch fm2("FM2_FPS_OVERLAY");
    return regpu || fm2;
}

void Reset(Meter& m)
{
    m.recent.clear();
    m.sumMs = 0;
    m.frames = 0;
    m.since = Clock::now();
}

// The average frame rate over the slowest 1% of the kept frames.
double OnePercentLow(const Meter& m)
{
    if (m.recent.size() < 100)
        return 0;
    std::vector<float> sorted(m.recent.begin(), m.recent.end());
    const size_t n = sorted.size() / 100;
    std::nth_element(sorted.begin(), sorted.begin() + n, sorted.end(), std::greater<float>());
    double sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += sorted[i];
    return sum > 0 ? 1000.0 * n / sum : 0;
}

bool OwnWindowFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
}

void FpsMeterFrame(float frameMs)
{
    if (!On())
        return;
    Meter& m = g_meter;
    std::lock_guard lock(m.mutex);
    const auto t = Clock::now();

    static LiveSwitch resetSwitch("REGPU_FPS_RESET");
    static LiveSwitch resetSwitchFm2("FM2_FPS_RESET"); // fm2ctl.py takes FM2_ names
    const bool resetNow = resetSwitch != resetSwitchFm2;
    if (m.resetSwitchSeen && resetNow != m.resetSwitchWas)
        Reset(m);
    m.resetSwitchSeen = true;
    m.resetSwitchWas = resetNow;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !m.f10Was && OwnWindowFocused())
    {
        Reset(m);
        Log("fps meter: reset (F10)");
    }
    m.f10Was = f10;

    // A frame over 1 s is a load or a pause, not rendering: it doesn't count.
    if (frameMs > 0 && frameMs < 1000)
    {
        m.recent.push_back(frameMs);
        if (m.recent.size() > kLowWindow)
            m.recent.pop_front();
        m.sumMs += frameMs;
        m.frames++;
        m.lastMs = frameMs;
    }
    m.now.push_back(t);
    while (!m.now.empty() && t - m.now.front() > std::chrono::milliseconds(500))
        m.now.pop_front();

    if (t - m.lastLog >= std::chrono::seconds(10))
    {
        m.lastLog = t;
        Log("fps meter: %.1f fps now, %.1f average, %.1f 1%% low over %llu frames (%.0f s)", m.now.size() * 2.0,
            m.frames && m.sumMs > 0 ? 1000.0 * m.frames / m.sumMs : 0.0, OnePercentLow(m), (unsigned long long)m.frames,
            std::chrono::duration<double>(t - m.since).count());
    }
}

bool FpsMeterLines(std::vector<std::string>& lines)
{
    if (!On())
        return false;
    Meter& m = g_meter;
    std::lock_guard lock(m.mutex);
    const double nowFps = m.now.size() * 2.0; // presents in the last half second
    const double avg = m.frames && m.sumMs > 0 ? 1000.0 * m.frames / m.sumMs : 0.0;
    const double low = OnePercentLow(m);
    const int secs = int(std::chrono::duration<double>(Clock::now() - m.since).count());
    char l[160];
    snprintf(l, sizeof(l), "FPS %5.1f   avg %5.1f   1%% low %5.1f", nowFps, avg, low);
    lines.push_back(l);
    snprintf(l, sizeof(l), "    %5.2f ms   %d:%02d measured (F10 resets)", m.lastMs, secs / 60, secs % 60);
    lines.push_back(l);
    return true;
}
