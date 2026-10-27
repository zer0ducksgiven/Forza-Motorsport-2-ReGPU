// Debug probe (FM2_PROBE_WAIT=1): logs what the title's GPU wait loop
// (sub_82369340) polls, once per distinct address.
#include <cstdlib>
#include <atomic>
#include <set>
#include <map>
#include <mutex>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <immintrin.h>

#include "kernel/imports.h"
#include "kernel/objects.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_82369340);
extern std::atomic<uint32_t> g_titleWaitAddress;

PPC_FUNC(sub_82369340)
{
    static LiveSwitch probe("FM2_PROBE_WAIT");
    if (probe)
    {
        static std::set<uint32_t> seen;
        const uint32_t device = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32));
        const uint32_t pointer = __builtin_bswap32(*GuestPtr<uint32_t>(device + 10768));
        const uint32_t target = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32 + 8));
        if (seen.size() < 8 && seen.insert(pointer).second)
            Log("probe: wait loop polls %08X (now %08X, waiting for change from %08X), lr %08llX", pointer,
                __builtin_bswap32(*GuestPtr<uint32_t>(pointer)), target, ctx.lr);
    }
    // The loop polls a fence the command processor writes (the frame it waits for to free ring
    // space). Spinning there was about 40% of the render thread, a core taken from the game and the
    // command processor on 4-core CPUs: it sleeps on the address instead, woken by gpu.cpp's
    // WriteBackNow (1 ms at most per wait, then it checks again). FM2_SPIN_RING_WAIT=1: spin (A/B).
    static LiveSwitch spin("FM2_SPIN_RING_WAIT");
    if (!spin)
    {
        const uint32_t device = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32));
        const uint32_t pointer = __builtin_bswap32(*GuestPtr<uint32_t>(device + 10768));
        if (pointer >= 0x80000000u) // a physical view (the CP writes there)
        {
            const uint32_t target = *GuestPtr<uint32_t>(ctx.r3.u32 + 8); // (both big-endian)
            volatile uint32_t* p = GuestPtr<uint32_t>(pointer);
            // A short spin first: most waits end within microseconds, sooner than a wake.
            for (int i = 0; i < 1000 && *p == target; i++)
                _mm_pause();
            if (*p == target)
            {
                g_titleWaitAddress = pointer;
                uint32_t seen = target;
                for (int i = 0; i < 50 && *p == target; i++)
                    WaitOnAddress(const_cast<uint32_t*>(p), &seen, sizeof(seen), 1);
                g_titleWaitAddress = 0;
            }
        }
    }
    __imp__sub_82369340(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82373E38);

// The wait loop's timeout path: count how often it fires.
PPC_FUNC(sub_82373E38)
{
    static LiveSwitch probe("FM2_PROBE_WAIT");
    static std::atomic<uint64_t> calls{ 0 };
    if (probe && (++calls % 200) == 1)
        Log("probe: wait-loop timeout path taken %llu times", (unsigned long long)calls.load());
    __imp__sub_82373E38(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82953E38);
uint32_t GuestAllocationSize(uint32_t address, uint32_t* start);

// Timer callback list walk: validate nodes (callback at +16, next at +0)
// before the title calls through them (FM2_PROBE_LIST=1).
PPC_FUNC(sub_82953E38)
{
    static LiveSwitch probe("FM2_PROBE_LIST");
    if (probe)
    {
        static std::set<uint32_t> known;
        uint32_t node = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32 + 24));
        for (int n = 0; node && n < 256; n++)
        {
            const uint32_t callback = __builtin_bswap32(*GuestPtr<uint32_t>(node + 16));
            if (callback < 0x82000000 || callback >= 0x83000000)
            {
                uint32_t start = 0;
                const uint32_t size = GuestAllocationSize(node, &start);
                Log("probe: bad timer node %08X (callback %08X) in allocation %08X+%X; words: %08X %08X %08X %08X %08X %08X", node, callback, start,
                    size, __builtin_bswap32(GuestPtr<uint32_t>(node)[0]), __builtin_bswap32(GuestPtr<uint32_t>(node)[1]),
                    __builtin_bswap32(GuestPtr<uint32_t>(node)[2]), __builtin_bswap32(GuestPtr<uint32_t>(node)[3]),
                    __builtin_bswap32(GuestPtr<uint32_t>(node)[4]), __builtin_bswap32(GuestPtr<uint32_t>(node)[5]));
                if (const char* ring = getenv("FM2_DUMP_RING"))
                {
                    // "start,end" guest range written to out/ring.bin.
                    const uint32_t a = uint32_t(strtoul(ring, nullptr, 16)), b = uint32_t(strtoul(strchr(ring, ',') + 1, nullptr, 16));
                    if (FILE* f = fopen("out/ring.bin", "wb"))
                    {
                        fwrite(GuestPtr(a), 1, b - a, f);
                        fclose(f);
                        Log("probe: wrote ring %08X..%08X", a, b);
                    }
                }
                break;
            }
            if (known.insert(node).second)
                Log("probe: timer node %08X callback %08X", node, callback);
            node = __builtin_bswap32(*GuestPtr<uint32_t>(node));
        }
    }
    __imp__sub_82953E38(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82957260);

// Video plane writer: log its arguments (FM2_PROBE_VIDEO=1).
PPC_FUNC(sub_82957260)
{
    static LiveSwitch probe("FM2_PROBE_VIDEO");
    if (probe)
    {
        static std::atomic<int> n{ 0 };
        if ((ctx.r5.u32 == 0x2F0 || ctx.r5.u32 == 0x178) && n++ < 40)
            Log("probe: video write r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X lr %08llX", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.lr);
    }
    const uint32_t dest = ctx.r3.u32, width = ctx.r5.u32, height = ctx.r6.u32, pitch = ctx.r7.u32, video = ctx.r10.u32;
    {
        extern uint32_t g_binkPlane[3];
        g_binkPlane[0] = dest, g_binkPlane[1] = pitch, g_binkPlane[2] = width;
        static LiveSwitch logStarts("FM2_LOG_PLANES");
        static uint32_t lumaStart = 0;
        if (logStarts && video == ctx.r10.u32)
        {
            if (width == 0x2F0)
                lumaStart = ctx.r8.u32;
            Log("bink: video %08X plane w %u bits at %08X (+%X) r9 %08X", video, width, ctx.r8.u32, ctx.r8.u32 - lumaStart, ctx.r9.u32);
        }
    }
    // FM2_CHECK_FRAMEBUF=1: hash each video's bitstream buffer (r8 at the luma
    // call) and report when it changes before that frame's chroma planes decode.
    static LiveSwitch checkBuf("FM2_CHECK_FRAMEBUF");
    if (checkBuf)
    {
        struct Seen
        {
            uint32_t start = 0;
            uint64_t hash = 0;
            int frame = 0;
        };
        static std::mutex lock;
        static std::map<uint32_t, Seen> videos;
        auto hashOf = [](uint32_t a) {
            uint64_t h = 1469598103934665603ull;
            const uint8_t* p = GuestPtr<uint8_t>(a);
            for (int i = 0; i < 0x8000; i++)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        };
        std::lock_guard<std::mutex> guard(lock);
        Seen& s = videos[video];
        if (width == 0x2F0)
        {
            s.start = ctx.r8.u32;
            s.hash = hashOf(s.start);
            s.frame++;
        }
        else if (s.start && hashOf(s.start) != s.hash)
            Log("bink: video %08X frame %d: bitstream buffer %08X changed before its chroma decode (thread %u)", video, s.frame, s.start,
                CurrentGuestThreadId());
    }
    // FM2_CHECK_FRAMES=<file.bik>: before each 752-wide luma decode, compare the
    // frame buffer after the read ring with that file's frame.
    static const char* checkFile = getenv("FM2_CHECK_FRAMES");
    if (checkFile && width == 0x2F0)
    {
        extern uint32_t g_binkRingBase;
        static std::vector<uint8_t> file;
        static std::vector<uint32_t> offsets;
        if (file.empty())
            if (FILE* f = fopen(checkFile, "rb"))
            {
                fseek(f, 0, SEEK_END);
                file.resize(ftell(f));
                fseek(f, 0, SEEK_SET);
                fread(file.data(), 1, file.size(), f);
                fclose(f);
                uint32_t frames, tracks;
                memcpy(&frames, &file[8], 4);
                memcpy(&tracks, &file[40], 4);
                for (uint32_t i = 0; i <= frames; i++)
                {
                    uint32_t o;
                    memcpy(&o, &file[44 + tracks * 12 + i * 4], 4);
                    offsets.push_back(o & ~1u);
                }
            }
        const uint8_t* buf = GuestPtr<uint8_t>(g_binkRingBase + 0x200040);
        if (g_binkRingBase && offsets.size() > 1)
        {
            int frame = -1;
            uint32_t at = 0;
            for (uint32_t p = 0; p + 64 <= file.size() && frame < 0; p++)
                if (memcmp(&file[p], buf, 64) == 0)
                {
                    at = p;
                    for (size_t i = 0; i + 1 < offsets.size(); i++)
                        if (offsets[i] <= p && p < offsets[i + 1])
                            frame = int(i);
                }
            if (frame < 0)
                Log("bink: frame buffer %08X matches nothing in the file", g_binkRingBase + 0x200040);
            else
            {
                const uint32_t len = offsets[frame + 1] - at;
                uint32_t k = 0;
                while (k < len && buf[k] == file[at + k])
                    k++;
                static int lastFrame = -1;
                if (k < len)
                    Log("bink: frame %d (buffer = file %X, %u bytes to frame end) differs at +%X", frame, at, len, k);
                else if (frame != lastFrame && frame % 20 == 0)
                    Log("bink: frame %d ok (buffer = file %X)", frame, at);
                lastFrame = frame;
            }
        }
    }
    // FM2_RING_AT=<n>: dump FM2_DUMP_RING before the n-th luma decode (0-based) of
    // the first 752-wide video.
    static const int ringAt = getenv("FM2_RING_AT") ? atoi(getenv("FM2_RING_AT")) : -1;
    static uint32_t ringVideo = 0;
    static int lumaCalls = 0;
    if (ringAt >= 0 && width == 0x2F0)
    {
        if (!ringVideo)
            ringVideo = video;
        if (video == ringVideo && lumaCalls++ == ringAt)
            if (const char* ring = getenv("FM2_DUMP_RING"))
            {
                const uint32_t a = uint32_t(strtoul(ring, nullptr, 16)), b = uint32_t(strtoul(strchr(ring, ',') + 1, nullptr, 16));
                if (FILE* f = fopen("out/ring.bin", "wb"))
                {
                    fwrite(GuestPtr(a), 1, b - a, f);
                    fclose(f);
                    Log("probe: ring dumped before luma decode %d", ringAt);
                }
            }
    }
    __imp__sub_82957260(ctx, base);
    // FM2_DUMP_LAST_CHROMA=1: keep the most recent finished 376-wide plane in out/chroma_last.bin.
    static LiveSwitch dumpLast("FM2_DUMP_LAST_CHROMA");
    if (dumpLast && width == 376)
        if (FILE* f = fopen("out/chroma_last.bin", "wb"))
        {
            fwrite(GuestPtr(dest), 1, pitch * height, f);
            fclose(f);
        }
    // FM2_DUMP_VIDEO_Y=<width>: append each decoded luma plane of the first
    // video with that width to out/video_y.raw (width x height, 8 bpp).
    static const uint32_t dumpWidth = getenv("FM2_DUMP_VIDEO_Y") ? uint32_t(strtoul(getenv("FM2_DUMP_VIDEO_Y"), nullptr, 0)) : 0;
    static uint32_t dumpVideo = 0;
    if (dumpWidth && width == dumpWidth)
    {
        if (!dumpVideo)
            dumpVideo = video;
        if (video == dumpVideo)
        {
            static FILE* f = fopen("out/video_y.raw", "wb");
            if (f)
                fwrite(GuestPtr(dest), 1, size_t(pitch) * ((height + 31) & ~31u), f); // raw (possibly tiled) plane
            if (f)
                fflush(f);
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_827C3BF0);
PPC_FUNC_IMPL(__imp__sub_827C3CB8);

// The title's malloc/free (FM2_PROBE_HEAP=<min bytes> logs allocations at least that big).
PPC_FUNC(sub_827C3BF0)
{
    static const uint32_t minBytes = getenv("FM2_PROBE_HEAP") ? uint32_t(strtoul(getenv("FM2_PROBE_HEAP"), nullptr, 0)) : 0;
    const uint32_t size = ctx.r3.u32;
    const uint64_t lr = ctx.lr;
    __imp__sub_827C3BF0(ctx, base);
    if (minBytes && size >= minBytes)
        Log("probe: malloc(%X) = %08X..%08X (lr %08llX)", size, ctx.r3.u32, ctx.r3.u32 + size, lr);
}

PPC_FUNC(sub_827C3CB8)
{
    static const uint32_t minBytes = getenv("FM2_PROBE_HEAP") ? 1u : 0u;
    if (minBytes)
        Log("probe: free(%08X) (lr %08llX)", ctx.r3.u32, ctx.lr);
    __imp__sub_827C3CB8(ctx, base);
}


// FM2_TRACE_PATCH=1: logs every call to the shader microcode patcher (sub_8272F650) with its
// inputs and the return address, so each patched shader load can be matched to its inputs.
PPC_FUNC_IMPL(__imp__sub_8272F650);
std::atomic<uint64_t> g_patchConfig{ 0 }; // hash of the latest patcher call's inputs (FM2_RECORD_CONFIGS)

// Hash of the patcher inputs that vary between calls (the 19 configurations).
static uint64_t PatchConfigHash(PPCContext& ctx)
{
    const uint32_t words[] = { uint32_t(ctx.lr), ctx.r7.u32, ctx.r8.u32, ctx.r25.u32, ctx.r26.u32, ctx.r10.u32, ctx.r22.u32, ctx.r24.u32, ctx.r27.u32, ctx.r28.u32 };
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t w : words)
        for (int i = 0; i < 4; i++)
            h = (h ^ ((w >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
    return h;
}

PPC_FUNC(sub_8272F650)
{
    static LiveSwitch record("FM2_RECORD_CONFIGS");
    if (record)
    {
        // The configuration is the inputs on entry; the registers change during the call.
        const uint64_t entry = PatchConfigHash(ctx);
        // FM2_CAPTURE_PATCH=<dir>: the first call per configuration writes the full input state
        // (registers, the 16 KB context at r31, the descriptors at r5/r6, and the output cursor
        // region before and after) so the patcher can be replayed offline.
        static const char* captureDir = getenv("FM2_CAPTURE_PATCH");
        static std::set<uint64_t> captured;
        const bool capture = captureDir && captured.insert(entry).second;
        auto blob = [&](uint32_t addr, uint32_t bytes) {
            std::vector<uint8_t> v(bytes);
            for (uint32_t i = 0; i < bytes; i++)
                v[i] = *GuestPtr<uint8_t>(addr + i);
            return v;
        };
        std::vector<uint8_t> ctxBefore, cursorBefore, r5Before, r6Before;
        uint32_t cursor = 0;
        if (capture)
        {
            cursor = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r31.u32 + 48));
            ctxBefore = blob(ctx.r31.u32, 0x4000);
            cursorBefore = blob(cursor, 0x400);
            r5Before = blob(ctx.r5.u32, 0x200);
            r6Before = blob(ctx.r6.u32, 0x200);
        }
        const uint32_t regsBefore[] = { uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
            ctx.r10.u32, ctx.r11.u32, ctx.r22.u32, ctx.r24.u32, ctx.r25.u32, ctx.r26.u32, ctx.r27.u32, ctx.r28.u32, ctx.r29.u32, ctx.r30.u32, ctx.r31.u32 };
        __imp__sub_8272F650(ctx, base);
        g_patchConfig.store(entry);
        if (capture)
        {
            char name[512];
            snprintf(name, sizeof(name), "%s/%016llx.bin", captureDir, (unsigned long long)entry);
            if (FILE* f = fopen(name, "wb"))
            {
                fwrite(regsBefore, 4, sizeof(regsBefore) / 4, f);
                for (const auto* v : { &ctxBefore, &cursorBefore, &r5Before, &r6Before })
                    fwrite(v->data(), 1, v->size(), f);
                const auto cursorAfter = blob(cursor, 0x400);
                fwrite(cursorAfter.data(), 1, cursorAfter.size(), f);
                fclose(f);
            }
        }
        return;
    }
    static LiveSwitch trace("FM2_TRACE_PATCH");
    static std::atomic<uint64_t> calls{ 0 };
    if (trace && calls++ < 400000)
        Log("patch %llu: lr %08llX r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r22 %08X r24 %08X r25 %08X r26 %08X r27 %08X r28 %08X r29 %08X r30 %08X r31 %08X",
            (unsigned long long)calls.load(), (unsigned long long)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
            ctx.r9.u32, ctx.r10.u32, ctx.r22.u32, ctx.r24.u32, ctx.r25.u32, ctx.r26.u32, ctx.r27.u32, ctx.r28.u32, ctx.r29.u32, ctx.r30.u32, ctx.r31.u32);
    static LiveSwitch mem("FM2_TRACE_PATCH_MEM");
    static std::atomic<int> dumps{ 0 };
    if (mem && dumps++ < 40)
    {
        auto words = [&](uint32_t a) {
            std::string s;
            for (int i = 0; i < 16; i++)
            {
                char t[12];
                snprintf(t, sizeof(t), " %08X", __builtin_bswap32(*GuestPtr<uint32_t>(a + i * 4)));
                s += t;
            }
            return s;
        };
        const uint32_t r5 = ctx.r5.u32, r6 = ctx.r6.u32;
        Log("patchmem before: r5 %08X:%s | r6 %08X:%s", r5, words(r5).c_str(), r6, words(r6).c_str());
        __imp__sub_8272F650(ctx, base);
        Log("patchmem after:  r5 %08X:%s | r6 %08X:%s", r5, words(r5).c_str(), r6, words(r6).c_str());
        return;
    }
    __imp__sub_8272F650(ctx, base);
}

// FM2_TRACE_PATCH_MEM=1: dumps 16 words at r5 and r6 before and after each patcher call.
