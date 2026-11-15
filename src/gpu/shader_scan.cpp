// Finds the title's shader objects in guest memory.
//
// A shader object the title builds from its asset data embeds a pre-built
// IM_LOAD_IMMEDIATE packet (header 0xC0nn2B00, type, size, then the microcode) that
// the D3D layer later copies into the command stream when the shader is bound. The
// objects appear in memory as a race loads, before anything draws with them, so
// scanning for those packets lets the renderer compile every shader of a race while
// the loading screen is still up instead of one at a time on first use.
#include "gpu/shader_scan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <windows.h>

#include "gpu/ucode.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

namespace
{
constexpr uint32_t kBase = 0xA0000000, kSize = 0x20000000; // the physical memory view
constexpr uint32_t kMaxDwords = 0x2000;

std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen;
std::vector<ScannedShader> g_found;
std::atomic<uint64_t> g_totalFound{ 0 };

uint64_t Fnv(const uint32_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n * 4; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

// Scans [begin, end) (byte offsets into the view) and reports valid shaders.
void ScanRange(uint32_t begin, uint32_t end, std::vector<ScannedShader>& out)
{
    const uint32_t* words = GuestPtr<uint32_t>(kBase);
    const uint32_t count = kSize / 4;
    const uint32_t first = begin / 4, last = std::min(end / 4, count - 3);
    for (uint32_t i = first; i < last; i++)
    {
        const uint32_t w = words[i];
        // Big-endian bytes C0..C3, count low, 2B, 00 read as a little-endian word.
        if ((w & 0xFFFF00FC) != 0x002B00C0)
            continue;
        const uint32_t header = __builtin_bswap32(w);
        const uint32_t packetCount = (header >> 16) & 0x3FFF;
        const uint32_t type = __builtin_bswap32(words[i + 1]);
        const uint32_t size = __builtin_bswap32(words[i + 2]);
        if (type > 1 || (size >> 16) != 0 || size < 2 || size > kMaxDwords || packetCount != (size & 0xFFFF) + 1)
            continue;
        if (i + 3 + size > count)
            continue;
        const uint32_t* code = &words[i + 3];
        const uint32_t used = ucode::UsedDwords(code, size);
        const uint64_t hash = Fnv(code, used) ^ type;
        {
            std::lock_guard lock(g_mutex);
            if (!g_seen.insert(hash).second)
                continue;
        }
        const ucode::Shader parsed = ucode::Parse(code, used, type == 1);
        if (!parsed.valid)
            continue;
        out.push_back({ type == 1, std::vector<uint32_t>(code, code + used), kBase + (i + 3) * 4 });
    }
}
}

void ShaderScanNow(unsigned threads)
{
    threads = std::clamp(threads, 1u, 16u);
    std::vector<std::vector<ScannedShader>> results(threads);
    std::vector<std::thread> workers;
    const uint32_t chunk = (kSize / threads + 0xFFFF) & ~0xFFFFu;
    for (unsigned t = 0; t < threads; t++)
        workers.emplace_back([&, t] { ScanRange(t * chunk, std::min(kSize, (t + 1) * chunk), results[t]); });
    for (auto& w : workers)
        w.join();
    std::lock_guard lock(g_mutex);
    for (auto& r : results)
    {
        g_totalFound += r.size();
        for (auto& s : r)
            g_found.push_back(std::move(s));
    }
}

void ShaderScanStart()
{
    static LiveSwitch off("FM2_NO_SHADER_SCAN");
    if (off)
        return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            Sleep(1500);
            ShaderScanNow(1);
        }
    }).detach();
}

bool ShaderScanTake(std::vector<ScannedShader>& out, size_t maxCount)
{
    std::lock_guard lock(g_mutex);
    if (g_found.empty())
        return false;
    const size_t n = std::min(maxCount, g_found.size());
    for (size_t i = 0; i < n; i++)
        out.push_back(std::move(g_found[i]));
    g_found.erase(g_found.begin(), g_found.begin() + n);
    return true;
}

uint64_t ShaderScanTotal()
{
    return g_totalFound.load();
}
