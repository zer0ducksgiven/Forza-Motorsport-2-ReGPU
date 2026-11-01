// Shared pieces of the decompiled functions (decomp.h): the check switch and its log.
#include "decomp/decomp.h"

// FM2_DECOMP_CHECK=1 checks everything; =lift only the lifted functions (lift.cpp), the others being slow to check
bool DecompChecking()
{
    static const char* v = getenv("FM2_DECOMP_CHECK");
    static const bool on = v && strcmp(v, "lift") != 0;
    return on;
}

bool LiftChecking()
{
    static const bool on = getenv("FM2_DECOMP_CHECK") != nullptr;
    return on;
}

bool DecompOff()
{
    static const bool off = getenv("FM2_DECOMP_OFF") != nullptr;
    return off;
}

void DecompReport(DecompStats& s, bool same, uint32_t r3, uint32_t original, uint32_t decompiled, const char* what)
{
    const uint32_t n = ++s.calls;
    if (!same)
    {
        const uint32_t w = ++s.wrong;
        if (w <= 10)
            Log("decomp: %s differs (%s): r3 %08X, original %08X, decompiled %08X", s.name, what, r3, original, decompiled);
    }
    if ((n & (n - 1)) == 0 && n >= 256)
        Log("decomp: %s checked %u calls, %u differ", s.name, n, s.wrong.load());
}
