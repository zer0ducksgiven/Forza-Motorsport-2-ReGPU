// FM2_COVERAGE builds (cmake -DFM2_COVERAGE=ON): every recompiled function records the first time it runs (see
// src/ppc_hooks.h), and the list is written to out/coverage.tsv every 2 s: address, milliseconds since start,
// host thread. tools/decomp/coverage.py sizes and sorts it. Functions our hooks replace without calling the
// original never show up; they're ours already.
#ifdef FM2_COVERAGE
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"

namespace
{
struct Hit
{
    uint32_t address, ms, thread;
};

std::mutex g_lock;
std::vector<Hit> g_hits;
const auto g_start = std::chrono::steady_clock::now();

void Writer()
{
    const char* path = getenv("FM2_COVERAGE_FILE");
    if (!path)
        path = "out/coverage.tsv";
    size_t written = 0;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::vector<Hit> copy;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (g_hits.size() == written)
                continue;
            copy = g_hits;
        }
        const std::string tmp = std::string(path) + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "w"))
        {
            for (const Hit& h : copy)
                fprintf(f, "%08X\t%u\t%u\n", h.address, h.ms, h.thread);
            fclose(f);
            MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
            written = copy.size();
        }
    }
}
} // namespace

void Fm2Cover(const char* func)
{
    const char* p = strstr(func, "sub_");
    if (!p)
        return;
    const uint32_t address = uint32_t(strtoul(p + 4, nullptr, 16));
    const uint32_t ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
    static std::once_flag once;
    std::call_once(once, [] {
        Log("coverage: recording the recompiled functions that run");
        std::thread(Writer).detach();
    });
    std::lock_guard<std::mutex> hold(g_lock);
    g_hits.push_back({address, ms, GetCurrentThreadId()});
}
#endif
