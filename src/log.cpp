#include "log.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace
{
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
const auto g_start = std::chrono::steady_clock::now();
std::deque<std::string> g_recent; // the last lines, for the debug server's log command

void Write(const char* prefix, const char* fmt, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();

    std::lock_guard lock(g_logMutex);
    char line[2100];
    snprintf(line, sizeof(line), "[%9.4f] %s%s", t, prefix, buffer);
    g_recent.emplace_back(line);
    if (g_recent.size() > 2000)
        g_recent.pop_front();
    if (!g_logFile)
        g_logFile = fopen("fm2.log", "w");
    for (FILE* f : { stderr, g_logFile })
    {
        if (f)
        {
            fprintf(f, "[%9.4f] %s%s\n", t, prefix, buffer);
            fflush(f);
        }
    }
}
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("", fmt, args);
    va_end(args);
}

void Fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("FATAL: ", fmt, args);
    va_end(args);
#ifdef _WIN32 // (the native Linux test tools link this file without the wheel code)
    void WheelStop();
    WheelStop();
#endif
    std::_Exit(1);
}

std::vector<std::string> LogTail(size_t n)
{
    std::lock_guard lock(g_logMutex);
    const size_t from = g_recent.size() > n ? g_recent.size() - n : 0;
    return std::vector<std::string>(g_recent.begin() + ptrdiff_t(from), g_recent.end());
}

#if FM2_REGPU
// The Vulkan ReGPU calls LogStart once Config::userDir is final (its own regpu.log); FM2 keeps its own fm2.log, so this
// does nothing (and keeps ReGPU's log.cpp out of the link, where its Log would clash with this one).
void LogStart() {}

// What ReGPU-V's log has besides (it calls both): every line is already flushed as it is written, and the seconds
// since start are the log's own clock.
void LogFlush() {}
double LogSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}
#endif // FM2_REGPU
