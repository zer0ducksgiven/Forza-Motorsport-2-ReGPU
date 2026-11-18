// Auto-drive (autodrive.h): lane keeping from the picture. MVP: keeps the car moving and roughly on the
// road with the chase camera and the game's assists on; it does not race.
//
// Each analysed frame: in a band of rows just above the car (the road ahead), every pixel that looks like
// asphalt (grey: low saturation, mid brightness) counts as road. Per row, the widest run of road (gaps of a
// few pixels bridged: cars and lines) gives a centre; the band's average centre against the screen centre
// is the error the stick steers by (proportional, smoothed). The throttle eases off when the error is large.
#include "hooks/autodrive.h"

#include "live_switch.h"
#include "log.h"
#include "png.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace
{
struct Command
{
    float steer = 0;    // -1 left .. 1 right
    float throttle = 0; // 0..1
    float brake = 0;
};
std::mutex g_mutex;
Command g_command;
float g_smoothed = 0;
bool g_haveRoad = false;
std::chrono::steady_clock::time_point g_lastSeen{}, g_lastDebug{}, g_lastLog{};

float EnvFloat(const char* name, float fallback)
{
    const char* v = getenv(name);
    return v ? float(atof(v)) : fallback;
}

// Asphalt: grey (channels within ~18), darker than the concrete apron and walls (Nissan Speedway: asphalt
// 50-120, the apron 135-145 and yellower).
bool Road(const uint8_t* p)
{
    const int r = p[0], g = p[1], b = p[2];
    const int hi = std::max({ r, g, b }), lo = std::min({ r, g, b });
    return hi - lo < 18 && hi > 40 && hi < 128;
}

// Stuck: the picture hardly changes while the throttle is on (nose in a wall). A coarse 16x9 grid of
// brightness is compared between analysed frames; 2.5 s of stillness backs the car out for 2.5 s.
float g_grid[16 * 9];
bool g_haveGrid = false;
std::chrono::steady_clock::time_point g_movingAt{}, g_reverseUntil{};
float g_reverseSteer = 0;
float g_flow = 0; // how fast the lower half of the picture changes: a rough speed (smoothed)
}

std::atomic<bool> g_roadSeen{ false };
bool AutodriveRoadSeen() { return g_roadSeen.load(); }

bool AutodriveOn()
{
    static LiveSwitch on("FM2_AUTODRIVE");
    return on;
}

void AutodriveAnalyze(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch)
{
    if (!rgba || width < 64 || height < 64)
        return;
    // The band (fractions of the height): the road beyond the car's roof in the chase camera.
    // Read every time, so fm2ctl.py "env FM2_AUTODRIVE_GAIN 0.8" tunes it live.
    const float bandTop = EnvFloat("FM2_AUTODRIVE_TOP", 0.44f), bandBottom = EnvFloat("FM2_AUTODRIVE_BOTTOM", 0.56f);
    const float gain = EnvFloat("FM2_AUTODRIVE_GAIN", 0.9f), damping = EnvFloat("FM2_AUTODRIVE_DAMPING", 1.2f);
    const float speed = EnvFloat("FM2_AUTODRIVE_THROTTLE", 0.7f), smoothing = EnvFloat("FM2_AUTODRIVE_SMOOTH", 0.6f);
    static LiveSwitch debug("FM2_AUTODRIVE_DEBUG");
    const uint32_t y0 = uint32_t(height * bandTop), y1 = uint32_t(height * bandBottom);
    const uint32_t step = std::max<uint32_t>(1, width / 320); // columns sampled
    const uint32_t bridge = std::max<uint32_t>(2, width / 60); // non-road pixels a run may cross

    const bool dump = debug && std::chrono::steady_clock::now() - g_lastDebug > std::chrono::seconds(2);
    std::vector<uint8_t> mask;
    if (dump)
        mask.assign(size_t(width) * height * 4, 0);

    double centreSum = 0, widthSum = 0;
    int rows = 0;
    for (uint32_t y = y0; y < y1; y += 2)
    {
        const uint8_t* line = rgba + size_t(y) * pitch;
        // Widest run of road, gaps up to `bridge` pixels bridged.
        int bestStart = -1, bestEnd = -1, runStart = -1, lastRoad = -1;
        for (uint32_t x = 0; x < width; x += step)
        {
            const bool road = Road(line + size_t(x) * 4);
            if (dump && road)
            {
                uint8_t* m = &mask[(size_t(y) * width + x) * 4];
                m[1] = 255, m[3] = 255;
            }
            if (!road)
                continue;
            if (runStart < 0 || int(x) - lastRoad > int(bridge))
                runStart = int(x);
            lastRoad = int(x);
            if (lastRoad - runStart > bestEnd - bestStart)
                bestStart = runStart, bestEnd = lastRoad;
        }
        if (bestStart < 0 || bestEnd - bestStart < int(width / 12))
            continue;
        centreSum += (bestStart + bestEnd) * 0.5;
        widthSum += bestEnd - bestStart;
        rows++;
    }

    Command c;
    const auto now = std::chrono::steady_clock::now();
    {
        float grid[16 * 9] = {};
        for (uint32_t gy = 0; gy < 9; gy++)
            for (uint32_t gx = 0; gx < 16; gx++)
            {
                const uint8_t* p = rgba + size_t(gy * height / 9 + height / 18) * pitch + size_t(gx * width / 16 + width / 32) * 4;
                grid[gy * 16 + gx] = float(p[0] + p[1] + p[2]);
            }
        float change = 0, lower = 0;
        for (int i = 0; i < 16 * 9; i++)
        {
            const float d = std::abs(grid[i] - g_grid[i]);
            change += d;
            if (i >= 16 * 5)
                lower += d;
        }
        change /= 16 * 9 * 3;
        lower /= 16 * 4 * 3;
        if (g_haveGrid)
            g_flow = g_flow * 0.8f + lower * 0.2f;
        if (!g_haveGrid || change > EnvFloat("FM2_AUTODRIVE_STILL", 2.5f))
            g_movingAt = now;
        memcpy(g_grid, grid, sizeof(grid));
        g_haveGrid = true;
        if (now > g_reverseUntil && now - g_movingAt > std::chrono::milliseconds(2500))
        {
            g_reverseUntil = now + std::chrono::milliseconds(1500);
            g_reverseSteer = g_smoothed > 0 ? -1.0f : 1.0f; // the other way from where it was heading
            g_movingAt = g_reverseUntil;
            Log("autodrive: stuck, backing out");
        }
    }
    if (now < g_reverseUntil)
    {
        c.steer = g_reverseSteer;
        c.brake = 1.0f; // held at a standstill, the automatic gearbox reverses
        std::lock_guard lock(g_mutex);
        g_command = c;
        return;
    }
    if (rows >= 3)
    {
        const double centre = centreSum / rows;
        const double error = (centre - width * 0.5) / (width * 0.5); // -1 road far left .. 1 far right
        const float previous = g_smoothed;
        g_smoothed = float(g_smoothed * smoothing + error * (1.0f - smoothing));
        // Proportional plus damping: the picture lags the stick by a few frames, so a fast-moving error is
        // answered less than a steady one (no weaving).
        c.steer = std::clamp(g_smoothed * gain - (g_smoothed - previous) * damping, -1.0f, 1.0f);
        c.throttle = std::abs(c.steer) > 0.6f ? speed * 0.45f : std::abs(c.steer) > 0.3f ? speed * 0.7f : speed;
        // Speed limit from the picture's flow: over it, coast; well over it, brake a little. Off by default: the
        // flow follows the scenery more than the speed (51 mph read 8.5, 2 mph 4.1, 73 mph 6.8 at Nissan Speedway).
        const float fast = EnvFloat("FM2_AUTODRIVE_FAST", 999.0f);
        if (g_flow > fast * 1.3f)
            c.throttle = 0, c.brake = 0.3f;
        else if (g_flow > fast)
            c.throttle = 0;
        g_haveRoad = true;
        if (!g_roadSeen.load()) g_roadSeen = true; // test tooling: the race view is up (AutodriveRoadSeen)
        g_lastSeen = now;
    }
    else
    {
        // No road in view (a wall, a menu): straighten a little and roll slowly.
        g_smoothed *= 0.8f;
        c.steer = std::clamp(g_smoothed * gain, -1.0f, 1.0f);
        c.throttle = now - g_lastSeen < std::chrono::seconds(2) ? speed * 0.4f : 0.25f;
        g_haveRoad = false;
    }
    {
        std::lock_guard lock(g_mutex);
        g_command = c;
    }
    if (debug && now - g_lastLog > std::chrono::milliseconds(500))
    {
        g_lastLog = now;
        Log("autodrive: %d rows, road %s, centre %.0f of %u, width %.0f, flow %.1f, steer %+.2f, throttle %.2f, brake %.2f", rows,
            g_haveRoad ? "seen" : "lost", rows ? centreSum / rows : 0.0, width, rows ? widthSum / rows : 0.0, g_flow, c.steer, c.throttle, c.brake);
    }
    if (dump)
    {
        g_lastDebug = now;
        for (uint32_t x = 0; x < width; x++) // the band's edges in red
            for (uint32_t y : { y0, y1 })
            {
                uint8_t* m = &mask[(size_t(std::min(y, height - 1)) * width + x) * 4];
                m[0] = 255, m[3] = 255;
            }
        std::error_code ec;
        std::filesystem::create_directories("out", ec);
        WritePng("out/autodrive.png", width, height, mask);
    }
}

bool AutodrivePad(int16_t& lx, uint8_t& throttle, uint8_t& brake)
{
    if (!AutodriveOn())
        return false;
    Command c;
    {
        std::lock_guard lock(g_mutex);
        c = g_command;
    }
    // Test tooling: FM2_AUTODRIVE_STRAIGHT=1 holds full throttle with the wheel straight, so every run drives the
    // same line (with a fixed frame cap, the same frames) for side-by-side comparisons.
    static const bool straight = getenv("FM2_AUTODRIVE_STRAIGHT") != nullptr;
    if (straight)
        c = Command{ 0.0f, 1.0f, 0.0f };
    lx = int16_t(std::clamp(c.steer, -1.0f, 1.0f) * 32000.0f);
    throttle = uint8_t(std::clamp(c.throttle, 0.0f, 1.0f) * 255.0f);
    brake = uint8_t(std::clamp(c.brake, 0.0f, 1.0f) * 255.0f);
    return true;
}
