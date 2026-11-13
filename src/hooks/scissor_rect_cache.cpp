// A cache for the HUD's scissor rectangles. TU 82622308 (disc 82605668) takes an item's bounding box (r5: min xyz,
// max xyz), its world matrix (r6) and a view (r4), projects the box's 8 corners through the world matrix and,
// for each corner, TU 826214E8, which rebuilds view * projection from the view's camera (cam = *(r4 + 0x60):
// matrices at cam + 0x50 and cam + 0x90, viewport at cam + 0x110), and clips the bounds to the view's viewport
// (r4 + 0x18). It writes x, y, w, h and two constant floats (24 bytes) to r3 and returns r3. That is about 7.5% of
// the game's main thread in a race, mostly for HUD items whose inputs do not change from frame to frame.
//
// Default: the result is kept per thread in a small table keyed on every byte the function reads, so a hit is
// exact. FM2_NO_SCISSOR_CACHE=1 (live switch): always computed. FM2_SCISSOR_STATS=1: hits and misses once a second.
#include <chrono>
#include <cstdint>
#include <cstring>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

namespace
{
struct Key
{
    uint32_t view, cam;
    uint8_t box[24], world[64], camMatrices[128], camViewport[16], viewport[16];
};
static_assert(sizeof(Key) == 256);

struct Entry
{
    Key key;
    uint8_t rect[24];
    bool valid;
};

constexpr uint32_t kEntries = 1024;
thread_local Entry* t_cache = nullptr;

uint32_t Hash(const Key& k)
{
    uint64_t words[sizeof(Key) / 8];
    memcpy(words, &k, sizeof(k));
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint64_t w : words)
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    return uint32_t(h ^ (h >> 29)) % kEntries;
}
} // namespace

PPC_FUNC_IMPL(TU_IMP(82622308));
PPC_FUNC(TU_SUB(82622308))
{
    static LiveSwitch off("FM2_NO_SCISSOR_CACHE");
    static LiveSwitch stats("FM2_SCISSOR_STATS");
    if (off)
        return TU_IMP(82622308)(ctx, base);

    const uint32_t out = ctx.r3.u32, view = ctx.r4.u32;
    Key key;
    key.view = view;
    key.cam = PPC_LOAD_U32(view + 0x60);
    memcpy(key.box, base + ctx.r5.u32, sizeof(key.box));
    memcpy(key.world, base + ctx.r6.u32, sizeof(key.world));
    memcpy(key.camMatrices, base + key.cam + 0x50, sizeof(key.camMatrices));
    memcpy(key.camViewport, base + key.cam + 0x110, sizeof(key.camViewport));
    memcpy(key.viewport, base + view + 0x18, sizeof(key.viewport));

    if (!t_cache)
        t_cache = new Entry[kEntries]();
    Entry& e = t_cache[Hash(key)];
    const bool hit = e.valid && memcmp(&e.key, &key, sizeof(key)) == 0;
    if (hit)
        memcpy(base + out, e.rect, sizeof(e.rect)); // r3 (the return value) is still out
    else
    {
        TU_IMP(82622308)(ctx, base);
        e.key = key;
        memcpy(e.rect, base + out, sizeof(e.rect));
        e.valid = true;
    }

    if (stats)
    {
        static thread_local uint64_t hits = 0, misses = 0;
        static thread_local auto last = std::chrono::steady_clock::now();
        ++(hit ? hits : misses);
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1))
        {
            Log("scissor cache: %llu hits, %llu misses a second", (unsigned long long)hits, (unsigned long long)misses);
            hits = misses = 0;
            last = now;
        }
    }
}
#endif
