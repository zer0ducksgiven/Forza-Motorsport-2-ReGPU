// The ReGPU-V build (-DFM2_REGPU=ON): the GPU is ReGPU-V, the Vulkan ReGPU (thirdparty/ReGPU,
// github.com/braaap-stututu/ReGPU), linked the way the FM3 and PGR3 ports link it. This file hands it FM2's display
// settings and the runtime pieces it needs: guest memory, guest threads, guest calls, the interrupt lock, the write watch.
// ReGPU provides the Vd* imports, the window, presentation and the debug server itself.
//
// The port's own renderer (src/gpu, src/render) is not built in this configuration. ReGPU-V carries its work (the
// November 2026 merge took FM2's renderer up to 0.14.0); what stays FM2's own lives here: which resolves the game reads
// back on the CPU, and its pacing.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <windows.h>

#include "regpu/driver.h"
#include "regpu/hyper.h"
#include "regpu/regpu.h"

#include "gpu/renderer.h"
#include "regpu_host/bench.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"
#include "settings.h"

std::string CurrentHostStack(int maxFrames); // crash.cpp
void LogAllHostThreadStacks(const char* why); // hang.cpp

namespace
{
std::atomic<uint64_t> g_frame{ 0 }, g_lastFocusFrame{ ~0ull >> 1 }; // (Photo Mode's write-backs, below)

// Sets a ReGPU environment default unless the player (or a test script) already set it.
void DefaultEnv(const char* name, const char* value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

void ApplySettings(regpu::Config& c)
{
    const Settings& s = GetSettings();
    c.width = s.width;
    c.height = s.height;
    c.aspectW = s.aspectW;
    c.aspectH = s.aspectH;
    c.fullscreen = s.fullscreen;
    c.vsync = s.vsync;
    c.maxFps = s.maxFps;
    c.msaa = s.msaa;
    c.upscaler = regpu::Config::Upscaler(uint32_t(s.upscaler)); // (Off, Fsr1, Fsr3, Dlss, Fsr4: the same order)
    c.quality = s.quality;
    c.sharpness = s.sharpness;
    c.lowPerformance = s.lowPerformance;
    // FSR 1's render scale (FM2's renderScale, percent per axis) as ReGPU's quality steps: 1 = 67%, 2 = 59%, 3 = 50%,
    // 4 = 33%. 100% with sharpening is FSR 1 at native (quality 0).
    if (s.upscaler == Settings::Upscaler::Fsr1)
        c.quality = s.renderScale >= 100 ? 0 : s.renderScale >= 67 ? 1 : s.renderScale >= 59 ? 2 : s.renderScale >= 50 ? 3 : 4;
    // FM2's SMAA (0 off, 1-4 presets): ReGPU's post-AA SMAA.
    if (s.smaa)
        c.postAA = regpu::Config::PostAA::Smaa;
    if (s.anisotropy < 16)
        regpu::SetAnisotropy(int(s.anisotropy));
}

// Test aid: FM2_WRITEBACK="WxH,WxH,..." writes colour resolves of those sizes back to memory at once, to find out which
// images the game reads on the CPU (read at start).
bool ExperimentWriteback(const regpu::ResolveCopy& c)
{
    static const std::set<std::pair<uint32_t, uint32_t>> sizes = [] {
        std::set<std::pair<uint32_t, uint32_t>> v;
        for (const char* p = getenv("FM2_WRITEBACK"); p && *p;)
        {
            uint32_t w = 0, h = 0;
            if (sscanf(p, "%ux%u", &w, &h) == 2)
                v.insert({ w, h });
            p = strchr(p, ',');
            if (p)
                p++;
        }
        return v;
    }();
    return !sizes.empty() && sizes.count({ c.width, c.height });
}
} // namespace

void RendererChangeSettings(std::function<void(Settings&)> change)
{
    Settings& s = GetSettings();
    const Settings before = s;
    change(s);
    SaveSettings();
    // What ReGPU changes while running; the rest takes effect at the next start.
    if (s.width != before.width || s.height != before.height)
        regpu::SetOutputSize(s.width, s.height);
    if (s.fullscreen != before.fullscreen)
        regpu::SetFullscreen(s.fullscreen);
    if (s.msaa != before.msaa)
        regpu::SetMsaa(s.msaa);
    if (s.maxFps != before.maxFps || s.vsync != before.vsync)
        regpu::SetFrameLimit(s.maxFps, s.vsync);
    if (s.anisotropy != before.anisotropy)
        regpu::SetAnisotropy(int(s.anisotropy));
}

// (GpuLogState, the hang report's GPU lines, is ReGPU-V's own.)
void Fm2GpuInit()
{
    regpu::Config& config = regpu::GetConfig();
    ApplySettings(config);
    config.userDir = UserDir(); // shader and pipeline caches
    config.windowTitle = L"Forza Motorsport 2";
    // GPU memory in 32 MB blocks (PGR3 Deck session 9, ReGPU-V REGPU_VMA_BLOCK_MB; VMA's default is 256 MB): a new
    // block's allocation clears it, and a 256 MB one stalled the frame that streamed a texture into it by ~30 ms.
    DefaultEnv("REGPU_VMA_BLOCK_MB", "32");
    // Vertex arena (from this port): large vertex buffers kept in video memory, copied again only when the game writes
    // their pages (src/memory.cpp write watch). settings.ini vertex_arena=0 (Settings::vertexArena): off.
    if (GetSettings().vertexArena)
        DefaultEnv("REGPU_VERTEX_ARENA", "1");
    if (GetSettings().vramUploadRing)
        DefaultEnv("REGPU_GPU_UPLOAD_RING", "1");

    // Early fences (as the PGR3/FH1R ports ship them): fences are written when the command processor reaches them, not
    // after the host GPU has finished. FM2's render thread waited for each, so the game, the command processor and the
    // GPU took turns. Deck benchmark (720p 2x, uncapped): frozen grid 41.4 -> 84.0 fps, AI race 44 -> 90 fps, the race
    // looked right throughout (grid, overtakes, a lap). FM2_LATE_FENCES=1: after the GPU, as before (start-time).
    if (!getenv("FM2_LATE_FENCES"))
        DefaultEnv("REGPU_EARLY_FENCES", "1");

    // Large dynamic index buffers read in place unless seen changing (REGPU_DIRECT_DYNAMIC_INDICES, as PGR3 ships it).
    // Frozen grid live A/B, 2 rounds: 84.2 -> 90.7 fps (+7.7%), the picture within 5/255 on 1% of pixels. Measured
    // there and not taken: REGPU_SPLIT_ALPHA_TEST (+2.8%, but the crowd in the stands disappears), REGPU_GPU_UPLOAD_RING
    // (-3.4%), and no change from FETCH_VARIANTS, FETCH_SPECIALIZE, UPLOAD_SAMPLED, CONSTANTS_BY_COUNT, NO_FENCE_ORDER,
    // ASYNC_PRESENT, VBLANK_ON_WAIT, COPIES_1010102, EXP_LAZY_LOOPS, NO_FOREIGN_MSAA. FM2_NO_DIRECT_INDICES=1: off.
    if (!getenv("FM2_NO_DIRECT_INDICES") && !getenv("REGPU_DIRECT_DYNAMIC_INDICES"))
        regpu::driver::SetSwitch("REGPU_DIRECT_DYNAMIC_INDICES", true);

    // Command processor waits under a frame cap (from the PGR3/FM3 ports, ReGPU REGPU_CP_IDLE_SLEEP_US): with the frame
    // rate held by a cap the CP is idle most of each frame but spun a core; on the Deck those watts are the GPU's too.
    // Only at caps of 144 and below or with vsync (uncapped it costs a little where the CPU is the limit).
    // FM2_CP_SPIN=1: off (spins as before).
    {
        const Settings& s = GetSettings();
        if (!getenv("FM2_CP_SPIN") && (s.vsync || (s.maxFps > 0 && s.maxFps <= 144)))
        {
            DefaultEnv("REGPU_CP_SPIN_MS", "1");
            DefaultEnv("REGPU_CP_IDLE_SLEEP_US", "200");
        }
    }

    regpu::Host& host = regpu::GetHost();
    host.base = g_base;
    host.physicalSection = PhysicalSection();
    host.alloc = [](bool physical, uint32_t size, uint32_t align) {
        return GuestAlloc(physical ? Heap::Physical : Heap::Virtual, size, align);
    };
    host.createThread = [](const char* name, void (*body)(PPCContext&)) { CreateHostGuestThread(name, body); };
    host.currentPcr = [] { return CurrentGuestThread()->pcr; };
    host.guestCall = [](PPCContext& ctx, uint32_t address) { GuestCall(ctx, address); };
    host.interruptLockAcquire = [](int cpu) { InterruptLockAcquire(cpu); };
    host.interruptLockRelease = [] { InterruptLockRelease(); };
    host.cpuUsage = [](double wallSeconds) { return GuestThreadCpuUsage(wallSeconds); };
    host.logThreadStacks = [](const char* why) { LogAllHostThreadStacks(why); };
    host.currentStack = [](int maxFrames) { return CurrentHostStack(maxFrames); };
    host.watchArm = [](uint32_t offset, uint32_t bytes) { return WriteWatchArm(offset, bytes); };
    host.watchClean = [](uint32_t offset, uint32_t bytes, uint64_t since) { return WriteWatchClean(offset, bytes, since); };
    host.watchSeq = [] { return WriteWatchSeq(); };

    host.onSwap = [](uint64_t frame) {
        g_frame = frame;
        BenchSwap();
    }; // (the benchmark's frame times; a flag check otherwise)

    // Resolves the game reads back on the CPU (FM2's renderer, FM2_RESOLVE_WRITEBACK's default "256x128,1024x1024"):
    // the garage thumbnail it renders, then compresses and saves (256x128), and the 1024x1024 livery a race compresses
    // for the car body. Written back at once, as FM2's renderer did. FM2_RESOLVE_WRITEBACK overrides the list.
    host.resolveWriteback = [](const regpu::ResolveCopy& c) {
        regpu::Writeback w;
        static const std::vector<std::pair<uint32_t, uint32_t>> sizes = [] {
            std::vector<std::pair<uint32_t, uint32_t>> v;
            const char* e = getenv("FM2_RESOLVE_WRITEBACK");
            for (const char* p = e ? e : "256x128,1024x1024"; p && *p;)
            {
                uint32_t width = 0, height = 0;
                if (sscanf(p, "%ux%u", &width, &height) == 2)
                    v.emplace_back(width, height);
                p = strchr(p, ',');
                if (p)
                    p++;
            }
            return v;
        }();
        // Photo Mode (FM2's renderer, WriteDepthToGuest / WritePhotoToGuest): the title reads the depth under the
        // reticle on the CPU for its focus point (the 4x scene's depth copied out as 8888, every Photo Mode frame;
        // races copy smaller depth images and must not pay for it), and saving a photo compresses the 4x 8888 scene
        // image on the CPU (sub_82293E28) while that focus readback says Photo Mode is up (within 2 frames). Both
        // written back Deferred (a frame late, no stall: a wait every frame ran Photo Mode at 20 fps).
        // FM2_NO_DEPTH_WRITEBACK=1: neither (as before the port's fix).
        static LiveSwitch noPhoto("FM2_NO_DEPTH_WRITEBACK");
        if (!noPhoto && c.samples == 4 && c.width >= 1280 && c.format == 6)
        {
            if (c.depth)
            {
                g_lastFocusFrame = g_frame.load();
                w.mode = regpu::Writeback::Mode::Deferred;
                static bool logged = false;
                if (!logged && (logged = true))
                    Log("writeback: Photo Mode focus depth at %08X", c.base);
                return w;
            }
            // Every tile of it (FM2 draws the 4x 720p scene in three: rows 0-256, 256-512, 512-720), at once: deferred, the
            // saved JPEG held a later frame (the save dialog) and only the tile written last (Deck session, 27 Nov).
            if (c.height == 720 && g_frame.load() - g_lastFocusFrame.load() <= 2)
            {
                static LiveSwitch deferred("FM2_PHOTO_DEFERRED");
                w.mode = deferred ? regpu::Writeback::Mode::Deferred : regpu::Writeback::Mode::Now;
                static std::atomic<int> logged{ 0 };
                if (logged.fetch_add(1) < 8)
                    Log("writeback: Photo Mode picture %08X %ux%u", c.base, c.width, c.height);
                return w;
            }
        }
        if (c.depth)
            return w;
        if (ExperimentWriteback(c) ||
            (c.format == 6 && c.top == 0 && std::find(sizes.begin(), sizes.end(), std::make_pair(c.width, c.height)) != sizes.end()))
        {
            w.mode = regpu::Writeback::Mode::Now;
            static std::atomic<int> logged{ 0 };
            if (logged.fetch_add(1) < 32)
                Log("writeback: %08X %ux%u format %u", c.base, c.width, c.height, c.format);
        }
        return w;
    };

    // FM2's pacing (its old gpu.cpp VblankLoop): the title presents on vblanks, every one in races and every second one
    // in menus, and keeps game time by the clock, not by frames. So vblanks run at twice the frame cap (120-720 Hz) and
    // ReGPU's limiter in VdSwap holds the cap. FM2_VBLANK_HZ (or REGPU_VBLANK_HZ) sets the rate.
    {
        const Settings& s = GetSettings();
        uint32_t hz = std::clamp<uint32_t>(s.maxFps * 2, 120, 720);
        if (const char* v = getenv("FM2_VBLANK_HZ"))
            hz = uint32_t(std::clamp(atoi(v), 30, 4000));
        DefaultEnv("REGPU_VBLANK_HZ", std::to_string(hz).c_str());
    }

    Log("gpu: ReGPU-V at %ux%u, msaa %u, cap %u, vsync %d, vblank %s Hz", config.width, config.height, config.msaa, config.maxFps,
        int(config.vsync), getenv("REGPU_VBLANK_HZ"));
    regpu::Start();
}

// hooks/perf_probe.cpp's diagnostic: the address the title's wait loop is polling (the old gpu.cpp read it; kept so the
// probe still builds).
std::atomic<uint32_t> g_titleWaitAddress{ 0 };
