// Ultrawide: the game's own camera aspect. sub_8257E0D0 is the camera's aspect setter
// (stfs f1,268(r3)), called every frame with 16/9. On an output wider than 16:9 it gets
// the wider aspect instead, so the game's projection and its frustum culling both cover
// the wider view (nothing at the sides is culled); the renderer then stops narrowing
// perspective geometry itself (see vwide in renderer.cpp).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>

#include "log.h"

#include "kernel/imports.h"
#include "live_switch.h"

float GameAspect();                          // renderer.cpp: the aspect to give the game (16/9 when not wider)
// When the game last set its camera's aspect (steady clock, ms): it does so every frame of a race and
// never in the menus, so a recent one means a race is on screen.
std::atomic<int64_t> g_cameraAspectAt{ 0 };

static int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CameraAspectWidened()
{
    return NowMs() - g_cameraAspectAt.load(std::memory_order_relaxed) < 250;
}

PPC_FUNC_IMPL(__imp__sub_8257E0D0);
PPC_FUNC(sub_8257E0D0)
{
    const double sixteenNine = 16.0 / 9.0;
    // FM2_TRACE_CAMERA: each distinct caller (and camera) of the aspect setter, the first time.
    if (static const bool trace = getenv("FM2_TRACE_CAMERA") != nullptr; trace)
    {
        static std::mutex m;
        static std::set<uint64_t> seen;
        std::lock_guard lock(m);
        if (seen.insert(uint64_t(uint32_t(ctx.lr)) << 32 | ctx.r3.u32).second)
            Log("camera: aspect %.4f from lr %08X camera %08X", ctx.f1.f64, uint32_t(ctx.lr), ctx.r3.u32);
    }
    static LiveSwitch off("FM2_NO_CAMERA_ASPECT"); // A/B: the renderer alone widens the view
    if (!off && std::fabs(ctx.f1.f64 - sixteenNine) < 1e-3)
    {
        const float wide = GameAspect();
        if (wide > float(sixteenNine) + 1e-3f)
        {
            ctx.f1.f64 = wide;
            g_cameraAspectAt.store(NowMs(), std::memory_order_relaxed);
        }
    }
    __imp__sub_8257E0D0(ctx, base);
}
