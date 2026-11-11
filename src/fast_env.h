#pragma once
// getenv, cached. The debugging switches are read with getenv all over the renderers, some of them on every draw, and
// the C runtime's getenv is a linear, case-insensitive search of the whole environment: a sampling profile of the
// Vulkan render thread during a race had 42% of its time in it. FastEnv answers from a small per-thread table keyed
// by the address of the name (string literals, in practice) and checked against its text, and returns exactly what
// getenv would. Code that changes the environment at run time (the debug server, LiveSwitch) calls FastEnvChanged().
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

inline std::atomic<uint32_t> g_fastEnvGeneration{ 1 };
inline void FastEnvChanged() { g_fastEnvGeneration.fetch_add(1, std::memory_order_relaxed); }

inline const char* FastEnv(const char* name)
{
    struct Entry
    {
        const char* key = nullptr;
        const char* value = nullptr; // the C runtime's own pointer, as getenv returns it
        uint32_t generation = 0;
        char name[52] = {};
    };
    thread_local Entry table[256];
    const uintptr_t p = reinterpret_cast<uintptr_t>(name);
    Entry& entry = table[((p >> 3) ^ (p >> 11)) & 255];
    const uint32_t generation = g_fastEnvGeneration.load(std::memory_order_relaxed);
    if (entry.key == name && entry.generation == generation && !strcmp(entry.name, name))
        return entry.value;
    const char* value = std::getenv(name);
    const size_t length = strlen(name);
    if (length < sizeof entry.name)
    {
        memcpy(entry.name, name, length + 1);
        entry.key = name;
        entry.value = value;
        entry.generation = generation;
    }
    return value;
}
