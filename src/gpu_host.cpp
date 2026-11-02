// The GPU is the ReGPU library (thirdparty/ReGPU, include/regpu/regpu.h). This file hands it the
// player's display settings and the runtime pieces it needs: guest memory, guest threads, guest
// calls and the interrupt lock. ReGPU provides the Vd* imports itself.
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include "regpu/regpu.h"

#include "boot_screen.h"
#include "pipeline_seed.h"
#include "static_shaders.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"
#include "settings.h"

std::string CurrentHostStack(int maxFrames); // crash.cpp
void LogAllHostThreadStacks(const char* why); // hang.cpp
void SpikeDump(double ms);                    // kernel/thread.cpp

namespace
{
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
    c.enhance = s.enhance;
    c.msaa = s.msaa;
    c.upscaler = regpu::Config::Upscaler(uint32_t(s.upscaler));
    c.quality = s.quality;
    c.sharpness = s.sharpness;
}
} // namespace

void GpuStart()
{
    regpu::Config& config = regpu::GetConfig();
    ApplySettings(config);
    config.userDir = UserDir(); // shader and pipeline caches, in the same place as before
    config.windowTitle = L"Forza Motorsport 4";

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
    host.slowFrame = [](double ms) { SpikeDump(ms); };
    host.bootScreen = BootScreenCompose;
    host.staticShaders = StaticShaders;
    host.staticShadersSaved = StaticShadersSaved;
    // Then the shipped pipeline list (src/pipeline_seed.h, from data/manifests by tools/pipeline_seed.py).
    host.pipelineSeed = [] { return kPipelineSeed; };
    // The road surface: the game renders its two-channel detail image (k_8_8, each mip level) and the
    // 32x512 strips it builds it from, then reads that memory back as textures in other formats (the
    // strips as DXT1), which ReGPU cannot map onto the rendered images. Without these in memory, every
    // track after the first showed stale strips: streaks along the road while driving. Written back
    // deferred, a frame late, as the image is rebuilt continuously; only the rows each resolve covers (the
    // image is rebuilt half at a time). FM4_NO_ROAD_WRITEBACK=1: not (A/B). FM4_ROAD_WRITEBACK=strips or
    // =image: only that part (test, which one the road needs).
    // Photo mode keeps its 64-bit picture accumulation in EDRAM across frames, with the 32-bit 4x scene
    // drawn over the same base in between: 64-bit colour layouts get their own surface, or the scene (and
    // its purple clear) bled into the photo below the first ~200 rows. FM4_COLOR_SURFACE_SHARED=1: off.
    if (!getenv("FM4_COLOR_SURFACE_SHARED") && !getenv("REGPU_COLOR_SURFACE_PER_BPP"))
        _putenv_s("REGPU_COLOR_SURFACE_PER_BPP", "1");
    // Textures unchanged for a while are checked every 4th/16th frame, not every frame (2.6 -> 0.35 ms a
    // frame of texture checks in a race, no visible change). FM4_TEXTURE_CHECK_EVERY_FRAME=1: every frame.
    if (!getenv("FM4_TEXTURE_CHECK_EVERY_FRAME") && !getenv("REGPU_TEXTURE_CHECK_STAGGER"))
        _putenv_s("REGPU_TEXTURE_CHECK_STAGGER", "1");
    // These write-backs come every frame; a fence held behind each (REGPU's default for a small write-back)
    // tied the game to the GPU every frame (about 60 -> 43 fps). The image is rebuilt continuously, so a
    // copy landing a frame late is harmless: fences are not held. FM4_WRITEBACK_FENCES=1 holds them (A/B).
    if (!getenv("FM4_WRITEBACK_FENCES") && !getenv("REGPU_NO_WRITEBACK_FENCES"))
        _putenv_s("REGPU_NO_WRITEBACK_FENCES", "1");
    host.resolveWriteback = [](const regpu::ResolveCopy& c) {
        regpu::Writeback w;
        static LiveSwitch off("FM4_NO_ROAD_WRITEBACK"); // live
        static const std::string part = getenv("FM4_ROAD_WRITEBACK") ? getenv("FM4_ROAD_WRITEBACK") : "";
        const bool image = c.format == 10 && c.width <= 512 && c.height <= 512 && part != "strips";
        const bool strip = c.width == 32 && c.height == 512 && part != "image";
        if (!off && !c.depth && (image || strip))
        {
            w.mode = regpu::Writeback::Mode::Deferred;
            // Each image written back, once (address, size, format, rows): which ones this rule catches.
            static std::mutex m;
            static std::set<uint64_t> seen;
            std::lock_guard lock(m);
            if (seen.insert(uint64_t(c.base) << 32 | c.width << 16 | c.height).second && seen.size() <= 400)
                Log("road writeback: %08X %ux%u format %u rows %d-%d", c.base, c.width, c.height, c.format, c.top, c.bottom);
            if (c.bottom > c.top)
            {
                w.y0 = uint32_t(std::max(c.top, 0));
                w.y1 = uint32_t(c.bottom);
            }
        }
        return w;
    };

    // The soft car shadow (fetch slot 11) is read through three tiles. The near tile (red) holds the
    // player's own shadow and the mid tile (green) the other cars' silhouettes, built from a depth copy
    // the shaders read as packed depth bytes (REGPU_DEPTH_AS_8888). The far tile (blue) reads the same
    // copy at about 2.3 times its scale (dark blocks on the road), so it reads lit.
    // FM4_NO_CASCADE_FIX=1 starts without this; each part is a live switch.
    if (!getenv("FM4_NO_CASCADE_FIX"))
    {
        if (!getenv("REGPU_CASCADE_SLOTS"))
            _putenv_s("REGPU_CASCADE_SLOTS", "11");
        for (const char* name : { "REGPU_DEPTH_AS_8888", "REGPU_CASCADE_B_LIT" })
            if (!getenv(name))
                _putenv_s(name, "1");
    }

    Log("gpu: ReGPU at %ux%u, msaa %u", config.width, config.height, config.msaa);
    regpu::Start();
}
