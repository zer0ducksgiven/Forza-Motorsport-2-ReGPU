// The native renderer's entry points, Vulkan side: the same seam the Direct3D 12 renderer
// implements (gpu/renderer.h), so the command processor, the lookahead and the presenter do
// not know which backend is running. The saved renderer setting selects it; FM2_RENDERER overrides it for tests.
//
// The backend owns Vulkan device initialization, guest shader conversion, draw submission,
// EDRAM surfaces and resolves. The existing D3D12 presenter displays the resulting frames.
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <immintrin.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "gpu/boot_screen.h"
#include "gpu/constants.h"
#include "gpu/indices.h"
#include "gpu/texture.h"
#include "gpu/regs.h"
#include "gpu/ucode.h"
#include "gpu/renderer.h"
#include "debug.h"
#include "kernel/runtime.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_draw.h"
#include "gpu/vk/vk_resolve.h"
#include "gpu/vk/vk_shader.h"
#include "gpu/vk/vk_shaders.h"
#include "gpu/vk/vk_state.h"
#include "gpu/vk/vk_surface.h"
#include "gpu/vk/vk_texture.h"
#include "memory.h"
#include "png.h"
#include "render/presenter.h"
#include "live_switch.h"
#include "log.h"
#include "settings.h"
#if defined(_WIN32)
#include <windows.h>
#endif

// One definition of the per-slot info word, shared by the fresh and the memoised bind paths so they cannot disagree
// about the dimension or the resolved bit.
static inline uint32_t TinfoWord(const TextureFetch& fetch, uint32_t resolvedCount)
{
    return (fetch.Dimension() == 3 ? 3u : 1u) | (resolvedCount ? (1u << 31) : 0u) | (fetch.Swizzle() << 8);
}

namespace
{
struct StateKeyHash
{
    size_t operator()(const vk::StateKey& k) const
    {
        uint64_t h = k.vs * 0x9E3779B97F4A7C15ull ^ k.ps;
        h ^= (uint64_t(k.blend) << 32 | k.depth) * 0xC2B2AE3D27D4EB4Full;
        h ^= (uint64_t(k.stencilMaskMode) << 32 | k.flags) * 0x165667B19E3779F9ull;
        return size_t(h ^ (h >> 29));
    }
};

struct State
{
    bool enabled = false, initialised = false, failed = false;
    vk::Context context;
    vk::ShaderSet shaders;
    vk::SurfaceCache surfaces;
    vk::TextureCache textures0; // the bindless uploader (2D)
    vk::TextureCache cubes;     // the same for cube maps (set 2)
    vk::DrawContext draw;
    vk::Resolve resolve;

    // Surface copies made for resolve packets, by the guest address they were resolved to: a
    // fetch at that address samples the copy instead of decoding guest memory (which the
    // renderer never wrote).
    struct ResolvedTexture
    {
        uint32_t index = 0;
        uint32_t width = 0, height = 0;
        bool valid = false;
        bool depth = false;
        bool swapped = false;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint64_t frame = ~0ull;
        uint64_t memHash = 0, memChecked = 0;
        uint32_t guestBpp = 4;
        bool stale = false;
        // The image's own size: the guest size times the render scale of the pass it was resolved from (see the resolve
        // packet). width/height above stay in guest pixels - they are what fetches are matched against.
        uint32_t hostWidth = 0, hostHeight = 0;
        std::vector<ResolvedTexture> spares;
    };
    std::unordered_map<uint32_t, ResolvedTexture> resolved;
    std::vector<uint32_t> retiredTextures;
    std::unordered_map<uint32_t, ResolvedTexture> presentable;
    VkSampler samplers[4]{};
    VkBuffer gphys = VK_NULL_HANDLE;
    std::unordered_map<vk::StateKey, VkPipeline, StateKeyHash> pipelines;

    uint64_t vsHash = 0, psHash = 0;

    // Decoded textures, by fetch identity: the index the shader samples, and a hash of the
    // guest data so a rewritten texture is re-uploaded instead of being served stale.
    struct CachedTexture
    {
        uint32_t index = 0;
        uint64_t hash = 0;
        uint64_t checkedFrame = 0; // frame the hash was last verified on (0: never)
        uint32_t unchanged = 0;    // consecutive checks that found the same bytes
    };
    std::unordered_map<uint64_t, CachedTexture> textures;
    struct MipChain
    {
        uint32_t index = UINT32_MAX;
        uint64_t checkedFrame = ~0ull;
    };
    std::unordered_map<uint32_t, MipChain> mipChains;
    struct SlotBinding
    {
        uint32_t fetch[6] = {};
        uint64_t frame = ~0ull, epoch = ~0ull;
        uint32_t tinfo[4] = {};
        float tbias[4] = {};
    };
    static constexpr uint32_t kBindingSlots = 8192;
    std::array<SlotBinding, kBindingSlots> bindings;
    uint64_t resolveEpoch = 0, bindReused = 0;
    std::vector<uint32_t> vsCode, psCode;
    uint64_t frame = 0;
    bool reportedDraws = false;
    uint32_t skippedShader = 0, skippedSurface = 0, skippedIndex = 0, skippedPipeline = 0, skippedDraw = 0;
    uint64_t drawsEntered = 0; // every draw the guest hands us, so "arrived" can be compared with "drawn"
    uint32_t texBound = 0, texFailed = 0, texSlots = 0, texDecoded = 0; // texDecoded: ran the decoder this frame
    // Placement state per draw, histogrammed: the frame's dominant viewport is what matters, not
    // whichever draw happened to be last.
    std::unordered_map<uint64_t, uint32_t> viewports;
    uint32_t viewportTarget = 0;

    // The frame: one command buffer, one render pass per surface (the D3D12 path does the same
    // — the guest's tile replays become one pass), submitted when the guest swaps.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vk::Surface* openSurface = nullptr;
    uint32_t openDepthBase = 0;
    vk::Surface* lastDrawSurface = nullptr;
    bool passOpen = false;
    uint64_t drawsThisFrame = 0;   // ordinal of the next draw in this frame
    bool reportedConfig = false; // one-shot: active switch banner
    VkQueryPool frameGpuTimes = VK_NULL_HANDLE;
    uint32_t frameGpuQuery = 0;
    bool frameGpuStarted = false;

    // Upload DXT textures as block-compressed images and let the GPU sample them, like D3D12.
    // FM2_VK_NO_BC=1 selects the CPU decoder fallback.
    bool bcUploads = false;
    // FM2_PERF: where the frame time goes (see the perf_vk report in RendererVkOnSwap).
    bool perf = false;
    double perfDrawsMs = 0, perfSubmitMs = 0, perfPresentMs = 0;
    double perfEndMs = 0, perfQueueMs = 0, perfWaitMs = 0;
    double perfBankMs = 0, perfTexMs = 0, perfPipelineMs = 0, perfRecordMs = 0, perfSetupMs = 0; // the draw, split
    double perfPreFetchMs = 0, perfFetchLoopMs = 0, perfVertexMs = 0, perfConstantCopyMs = 0, perfDescriptorMs = 0, perfDrawCallMs = 0;
    double perfResolveMs = 0, perfIndexMs = 0, perfConstantsMs = 0, perfShaderMs = 0, perfTailMs = 0, perfSurfaceMs = 0;
    double perfPostConstMs = 0, perfPreConstMs = 0; // the spans either side of BuildConstants
    double perfPreludeMs = 0, perfDynamicMs = 0;
    double perfPresentResolveMs = 0, perfPresentConvertMs = 0, perfPresentHandoffMs = 0; // inside perfPresentMs
    double perfIndicesMs = 0;  // index expansion, which runs between the record timer and DrawList
    double perfIndexedMs = 0;  // the DrawIndexed entry point
    double perfHashMs = 0;                                                     // inside perfTexMs
    double perfBindMs = 0;                                                     // inside perfTexMs
    uint64_t perfTexCalls = 0, perfHashCalls = 0, perfResolves = 0, perfResolveAllocations = 0;
    uint64_t perfBindCube = 0, perfBindResolved = 0, perfBindRefused = 0, perfBind2D = 0;
    uint64_t perfFrames = 0, perfDraws = 0;
    double perfFrameGpuMs = 0;
    uint32_t perfFrameGpuSamples = 0;
    std::chrono::steady_clock::time_point perfWindow;
} g;

struct AsyncFrame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    vk::Image resolved; // RGBA8, output size, display gamma applied: what the window shows
    VkBuffer copy = VK_NULL_HANDLE;
    void* readback = nullptr;
    uint32_t width = 0, height = 0;
    bool resolvedUsed = false;
    size_t framebufferCount = 0, viewCount = 0, setCount = 0;
    uint32_t gpuQuery = UINT32_MAX;
    bool hasImage = false, pending = false;
};
AsyncFrame g_async[2];
uint32_t g_asyncIndex = 0;
vk::DrawContext g_asyncOther;
std::mutex g_asyncMutex;

bool UseAsyncPresent()
{
    // On by default: the synchronous path ends every frame with vkQueueWaitIdle; this replaces it with a wait on the
    // previous frame's fence. FM2_VK_SYNC_PRESENT=1 selects the synchronous path. Headless keeps it off: there is no
    // present to overlap there.
    static const bool enabled = !getenv("FM2_VK_SYNC_PRESENT") && !getenv("FM2_HEADLESS");
    return enabled;
}

struct AsyncFenceApi
{
    PFN_vkCreateFence create;
    PFN_vkWaitForFences wait;
    PFN_vkResetFences reset;
    PFN_vkDestroyFence destroy;
};

const AsyncFenceApi& AsyncFences()
{
    static const AsyncFenceApi api = [] {
        const auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkldr::GetInstanceProcAddr(g.context.instance, "vkGetDeviceProcAddr"));
        return AsyncFenceApi{ reinterpret_cast<PFN_vkCreateFence>(get(g.context.device, "vkCreateFence")),
            reinterpret_cast<PFN_vkWaitForFences>(get(g.context.device, "vkWaitForFences")),
            reinterpret_cast<PFN_vkResetFences>(get(g.context.device, "vkResetFences")),
            reinterpret_cast<PFN_vkDestroyFence>(get(g.context.device, "vkDestroyFence")) };
    }();
    return api;
}

void AsyncPresentPrevious(bool swapDraw);

// Adds to a millisecond counter for as long as it lives; a null sink costs no clock reads.
struct ScopedMs
{
    std::chrono::steady_clock::time_point start;
    double* sink;
    explicit ScopedMs(double* s) : start(s ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}), sink(s) {}
    ~ScopedMs() { Stop(); }
    void Stop()
    {
        if (sink)
        {
            *sink += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            sink = nullptr;
        }
    }
};

struct GpuTimingApi
{
    PFN_vkCreateQueryPool create = nullptr;
    PFN_vkCmdWriteTimestamp write = nullptr;
    PFN_vkGetQueryPoolResults results = nullptr;
    PFN_vkCmdResetQueryPool reset = nullptr;
};

const GpuTimingApi& TimingApi()
{
    static const GpuTimingApi api = [] {
        GpuTimingApi a;
        const auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkldr::GetInstanceProcAddr(g.context.instance, "vkGetDeviceProcAddr"));
        if (get)
        {
            a.create = reinterpret_cast<PFN_vkCreateQueryPool>(get(g.context.device, "vkCreateQueryPool"));
            a.write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(get(g.context.device, "vkCmdWriteTimestamp"));
            a.results = reinterpret_cast<PFN_vkGetQueryPoolResults>(get(g.context.device, "vkGetQueryPoolResults"));
            a.reset = reinterpret_cast<PFN_vkCmdResetQueryPool>(get(g.context.device, "vkCmdResetQueryPool"));
        }
        return a;
    }();
    return api;
}

// FM2_VK_PHASE_GPU_TIMES (with FM2_VK_FRAME_GPU_TIMES): GPU time of an undisturbed frame, split at its resolves.
// The marks only add timestamps where the pass is already closed.
struct PhaseMark
{
    uint32_t draw, base;
    uint16_t width, height;
    uint8_t kind; // 0 frame start, 1 draws done (resolve begins), 2 resolve done, 3 frame end, 4 one draw (mode 2)
    uint64_t ps = 0;
    uint32_t vertices = 0;
};
constexpr uint32_t kPhaseMarks = 4096;
bool g_phasePerDraw = false; // FM2_VK_PHASE_GPU_TIMES=2: also a mark after every draw, inside the open pass
VkQueryPool g_phasePool = VK_NULL_HANDLE;
std::vector<PhaseMark> g_phaseMarks[2];
uint32_t g_phaseSlot = 0;
bool g_phaseStarted = false;

void PhaseMarkWrite(uint8_t kind, uint32_t base = 0, uint32_t width = 0, uint32_t height = 0, uint64_t ps = 0, uint32_t vertices = 0)
{
    if (kind == 4 && !g_phasePerDraw)
        return;
    if (!g_phaseStarted || g_phaseMarks[g_phaseSlot].size() >= kPhaseMarks)
        return;
    TimingApi().write(g.cmd, kind ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_phasePool,
        g_phaseSlot * kPhaseMarks + uint32_t(g_phaseMarks[g_phaseSlot].size()));
    g_phaseMarks[g_phaseSlot].push_back({ uint32_t(g.drawsThisFrame), base, uint16_t(width), uint16_t(height), kind, ps, vertices });
}

void PhaseBegin()
{
    static const bool enabled = getenv("FM2_VK_PHASE_GPU_TIMES") != nullptr;
    if (!enabled)
        return;
    g_phasePerDraw = atoi(getenv("FM2_VK_PHASE_GPU_TIMES")) == 2;
    const GpuTimingApi& api = TimingApi();
    if (!g_phasePool)
    {
        VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kPhaseMarks * 2;
        if (api.create(g.context.device, &info, nullptr, &g_phasePool) != VK_SUCCESS)
            return;
    }
    g_phaseSlot = UseAsyncPresent() ? g_asyncIndex : 0;
    g_phaseMarks[g_phaseSlot].clear();
    api.reset(g.cmd, g_phasePool, g_phaseSlot * kPhaseMarks, kPhaseMarks);
    g_phaseStarted = true;
    PhaseMarkWrite(0);
}

// Reads one finished frame's marks. Every 120th frame is logged in full; the split is accumulated for all of them.
void PhaseEnd(uint32_t slot)
{
    std::vector<PhaseMark>& marks = g_phaseMarks[slot];
    if (!g_phasePool || marks.size() < 2)
        return;
    std::vector<uint64_t> values(marks.size());
    if (TimingApi().results(g.context.device, g_phasePool, slot * kPhaseMarks, uint32_t(marks.size()), values.size() * sizeof(uint64_t),
            values.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
    {
        marks.clear();
        return;
    }
    static uint64_t frames = 0;
    static double sumDraws = 0, sumResolves = 0, sumTail = 0;
    const bool detail = ++frames % 120 == 0;
    double drawsMs = 0, resolvesMs = 0, tailMs = 0;
    std::vector<std::pair<double, size_t>> perDraw;
    std::unordered_map<uint64_t, std::pair<double, uint32_t>> perShader;
    for (size_t i = 1; i < marks.size(); i++)
    {
        const double ms = values[i] >= values[i - 1] ? double(values[i] - values[i - 1]) * g.context.props.limits.timestampPeriod / 1e6 : 0.0;
        const PhaseMark& mark = marks[i];
        (mark.kind == 1 || mark.kind == 4 ? drawsMs : mark.kind == 2 ? resolvesMs : tailMs) += ms;
        if (mark.kind == 4)
        {
            if (detail)
            {
                perDraw.emplace_back(ms, i);
                perShader[mark.ps].first += ms;
                perShader[mark.ps].second++;
            }
            continue;
        }
        if (detail && ms >= 0.25)
            Log("phase_vk:   %s %.3f ms | draws %u..%u | resolve %08x %ux%u", mark.kind == 1 ? "draws  " : mark.kind == 2 ? "resolve" : "tail   ",
                ms, marks[i - 1].draw, mark.draw, mark.base, mark.width, mark.height);
    }
    sumDraws += drawsMs;
    sumResolves += resolvesMs;
    sumTail += tailMs;
    if (detail && !perDraw.empty())
    {
        std::sort(perDraw.begin(), perDraw.end(), std::greater<>());
        for (size_t i = 0; i < std::min<size_t>(24, perDraw.size()); i++)
        {
            const PhaseMark& mark = marks[perDraw[i].second];
            Log("phase_vk:   draw %u %.3f ms | ps %016llx | %u vertices | target %ux%u", mark.draw, perDraw[i].first,
                (unsigned long long)mark.ps, mark.vertices, mark.width, mark.height);
        }
        std::vector<std::pair<double, uint64_t>> shaders;
        for (const auto& [hash, total] : perShader)
            shaders.emplace_back(total.first, hash);
        std::sort(shaders.begin(), shaders.end(), std::greater<>());
        for (size_t i = 0; i < std::min<size_t>(12, shaders.size()); i++)
            Log("phase_vk:   ps %016llx %.3f ms in %u draws", (unsigned long long)shaders[i].second, shaders[i].first,
                perShader[shaders[i].second].second);
    }
    if (detail)
    {
        Log("phase_vk: frame %llu: draws %.3f ms, resolves %.3f ms, tail %.3f ms, %zu marks | mean of 120: draws %.3f resolves %.3f tail %.3f",
            (unsigned long long)g.frame, drawsMs, resolvesMs, tailMs, marks.size(), sumDraws / 120, sumResolves / 120, sumTail / 120);
        sumDraws = sumResolves = sumTail = 0;
    }
    marks.clear();
}

void FrameGpuTimesBegin()
{
    static const bool enabled = getenv("FM2_VK_FRAME_GPU_TIMES") != nullptr;
    if (!enabled || g.frameGpuStarted)
        return;
    const GpuTimingApi& api = TimingApi();
    if (!api.create || !api.write || !api.results || !api.reset)
        return;
    if (!g.frameGpuTimes)
    {
        VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 8; // four timestamps per async frame, two async frames
        if (api.create(g.context.device, &info, nullptr, &g.frameGpuTimes) != VK_SUCCESS)
            return;
    }
    // The frame pair is at slot*4.
    g.frameGpuQuery = (UseAsyncPresent() ? g_asyncIndex : 0) * 4;
    api.reset(g.cmd, g.frameGpuTimes, g.frameGpuQuery, 2);
    api.write(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.frameGpuTimes, g.frameGpuQuery);
    g.frameGpuStarted = true;
    PhaseBegin();
}

void FrameGpuTimesEndQuery()
{
    if (g.frameGpuStarted)
        TimingApi().write(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.frameGpuTimes, g.frameGpuQuery + 1);
    PhaseMarkWrite(3);
    g_phaseStarted = false;
}

void FrameGpuTimesEnd()
{
    if (!g.frameGpuStarted)
        return;
    uint32_t query = g.frameGpuQuery;
    if (UseAsyncPresent())
    {
        AsyncFrame& previous = g_async[g_asyncIndex];
        query = previous.pending ? UINT32_MAX : previous.gpuQuery;
        previous.gpuQuery = UINT32_MAX;
    }
    if (query != UINT32_MAX)
    {
        uint64_t values[2]{};
        const GpuTimingApi& api = TimingApi();
        if (api.results(g.context.device, g.frameGpuTimes, query, 2, sizeof values, values, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            g.perfFrameGpuMs += double(values[1] - values[0]) * g.context.props.limits.timestampPeriod / 1e6;
            g.perfFrameGpuSamples++;
        }
        PhaseEnd(query / 4);
    }
    g.frameGpuStarted = false;
}

float RegFloat(uint32_t index)
{
    const uint32_t word = gpureg::Get(index);
    float f;
    memcpy(&f, &word, 4);
    return f;
}

// The guest's own pass dimensions, lifted from the D3D12 path (renderer.cpp: BigPassSize,
// PassWidth, PassHeight, PassScale, ScaleX, ScaleY). A "big pass" is a render target wider than
// the screen — the paint editor's 1024x1024 livery target, the tall shadow maps — and its pass
// size is what the shaders divide by, while the host viewport shrinks to fit. Substituting the
// host size for it (which this renderer did) squeezes those draws into part of the frame.
constexpr uint32_t kWidth = 1280, kHeight = 720;
// The guest's window scissor, lifted from the D3D12 path (renderer.cpp: WindowScissor), which
// also detects the tall pass: a scissor reaching past the screen height on a pass narrower than
// it. Its y extent becomes the pass height, which is what the shaders divide by — the reason the
// 3D content was landing in a band at the top.
uint32_t g_tallInfo = ~0u, g_tallHeight = 0;

// FM2_VK_TRACE_BINDS: one line per distinct (ps, slot, address) with its size. Called from both bind paths - the
// memo-served one is the majority of a frame.
static void TraceBind(uint32_t slot, const TextureFetch& fetch, uint64_t psHash)
{
    static const bool traceBinds = getenv("FM2_VK_TRACE_BINDS") != nullptr;
    if (!traceBinds)
        return;
    static uint32_t seen[256] = {};
    static uint32_t seenCount = 0;
    const uint32_t key = uint32_t(fetch.Address()) ^ (slot << 25);
    for (uint32_t i = 0; i < seenCount; i++)
        if (seen[i] == key)
            return;
    if (seenCount < 256)
    {
        seen[seenCount++] = key;
        Log("bind_vk: ps %016llx slot %u dim %u %ux%u addr %08x", (unsigned long long)psHash, slot,
            fetch.Dimension(), fetch.Width(), fetch.Height(), fetch.Address());
    }
}

uint32_t PassWidth();
uint32_t PassHeight();
struct ScissorRect
{
    int32_t left = 0, top = 0, right = 0, bottom = 0;
};

struct TileState
{
    int32_t originY = 0;
    ScissorRect rect{ 0, 0, 1 << 14, 1 << 14 };
    uint32_t surface = ~0u;
} g_tile;

ScissorRect WindowScissor()
{
    const uint32_t offsetReg = gpureg::Get(0x2080 /* PA_SC_WINDOW_OFFSET */);
    const int32_t wx = int32_t(offsetReg << 17) >> 17, wy = int32_t((offsetReg >> 16) << 17) >> 17;
    const bool vertexOffset = (gpureg::Get(gpureg::PA_SU_SC_MODE_CNTL) >> 16) & 1;
    const uint32_t tl = gpureg::Get(0x2081 /* PA_SC_WINDOW_SCISSOR_TL */), br = gpureg::Get(0x2082 /* BR */);
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF), x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!(tl >> 31))
    {
        x0 += wx;
        x1 += wx;
        y0 += wy;
        y1 += wy;
    }
    if (vertexOffset)
    {
        x0 -= wx;
        x1 -= wx;
        y0 -= wy;
        y1 -= wy;
    }
    const uint32_t surfaceInfo = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    if (y1 > int32_t(kHeight) && y1 <= 2048 && (surfaceInfo & 0x3FFF) < kWidth)
    {
        static uint32_t logged = 0;
        if (logged++ < 3)
            Log("tall_vk: scissor y %d..%d x %d..%d, window offset %d,%d, vertexOffset %d, surface %08X", y0, y1, x0, x1, wx, wy,
                int(vertexOffset), surfaceInfo);
        g_tallInfo = surfaceInfo;
        g_tallHeight = uint32_t(y1);
    }
    const int32_t maxX = int32_t(PassWidth()), maxY = int32_t(PassHeight());
    ScissorRect rect;
    rect.left = std::clamp(x0, 0, maxX);
    rect.top = std::clamp(y0, 0, maxY);
    rect.right = std::clamp(x1, 0, maxX);
    rect.bottom = std::clamp(y1, 0, maxY);
    return rect;
}

uint32_t BigPassSize()
{
    const uint32_t pitch = gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF;
    return pitch > kWidth ? std::min<uint32_t>(2048, pitch * 64 / 65) : 0;
}
uint32_t PassWidth() { const uint32_t big = BigPassSize(); return big ? big : kWidth; }
uint32_t PassHeight()
{
    const uint32_t big = BigPassSize();
    if (big)
        return big;
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    return g_tallHeight && surface == g_tallInfo ? g_tallHeight : kHeight;
}
float PassScale() { return std::min(float(kWidth) / float(PassWidth()), float(kHeight) / float(PassHeight())); }
float ScaleY()
{
    uint32_t w = 0, h = 0;
    RenderSize(w, h);
    return float(h) / float(kHeight) * PassScale();
}
float ScaleX()
{
    uint32_t w = 0, h = 0;
    RenderSize(w, h);
    return (gpureg::Get(0x2000) & 0x3FFF) == kWidth ? float(w) / float(kWidth) * PassScale() : ScaleY();
}

vk::StateKey MakeKey(uint64_t vs, uint64_t ps, bool cullable, uint32_t primitive)
{
    vk::StateKey key;
    key.vs = vs;
    key.ps = ps;
    key.blend = gpureg::Get(0x2201);                              // RB_BLENDCONTROL0
    key.depth = gpureg::Get(0x2200);                              // RB_DEPTHCONTROL
    const uint32_t mode = gpureg::Get(gpureg::PA_SU_SC_MODE_CNTL) & 7;
    const uint32_t mask = gpureg::Get(0x2104) & 0xF;              // RB_COLOR_MASK, an enable mask
    const uint32_t stencil = gpureg::Get(0x210D) & 0xFFFF00;      // RB_STENCILREFMASK (masks only)
    key.stencilMaskMode = stencil | (mask << 24) | (mode << 28);
    key.flags = ((gpureg::Get(0x2202) >> 4) & 1) | (uint32_t(cullable) << 1) |
        (((gpureg::Get(0x2208) & 7) == 5 ? 1u : 0u) << 2) | // RB_COLORCONTROL bit 4
        ((primitive & 7u) << 3);                            // bits 3-5: primitive type, see vk_state.cpp
    return key;
}

VkPipeline PipelineFor(const vk::StateKey& key, const vk::VkShader* vs, const vk::VkShader* ps, vk::Surface& surface)
{
    vk::StateKey cacheKey = key;
    cacheKey.vs = uint64_t(reinterpret_cast<uintptr_t>(vs->module));
    cacheKey.ps = uint64_t(reinterpret_cast<uintptr_t>(ps->module));
    if (auto it = g.pipelines.find(cacheKey); it != g.pipelines.end())
        return it->second;
    vk::TargetFormats targets;
    targets.samples = surface.samples;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vk::CreatePipeline(g.context, key, vs->module, ps->module, g.draw.layout, surface.pass, targets, pipeline))
        return VK_NULL_HANDLE;
    g.pipelines[cacheKey] = pipeline;
    return pipeline;
}

// The surface's pixels as RGBA8, through the resolve path the surface test verified. The resolve
// target and the readback buffer are kept: this runs once per frame in windowed play, and a
// VkImage plus a VkBuffer (7 MB of device memory each at 720p) per frame is both a leak and a
// round trip through the allocator. `lit`, when asked for, counts components above 2% for the
// dump's log line.
// Xenos half to float: like IEEE half but exponent 31 is an ordinary exponent (no infinities or
// NaNs), so the range reaches 131008.
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits = 0;
    if (exp == 0)
        bits = sign;
    else if (exp == 31)
        bits = sign | 0x7F800000u | (man << 13);
    else
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// The present readback converts 921 600 pixels a frame; the branchy bit-exact conversion in that
// loop cost 2.4 ms of it once the readback stopped being uncached. Same conversion, tabulated.
const std::array<uint8_t, 65536>& HalfToByte()
{
    static const std::array<uint8_t, 65536> table = [] {
        std::array<uint8_t, 65536> t{};
        for (uint32_t h = 0; h < 65536; h++)
            t[h] = uint8_t(std::min(1.0f, std::max(0.0f, HalfToFloat(uint16_t(h)))) * 255.0f + 0.5f);
        return t;
    }();
    return table;
}

struct DisplayGamma
{
    std::array<float, 17> points{};
    std::array<uint8_t, 256> bytes{};
};

const DisplayGamma* DisplayGammaCurve()
{
    static LiveSwitch off("FM2_NO_GAMMA");
    static DisplayGamma curve;
    static std::array<float, 17> cached{};
    static bool active = false;
    float points[17];
    if (off || !RendererDisplayGamma(points))
    {
        active = false;
        return nullptr;
    }
    if (!active || memcmp(cached.data(), points, sizeof cached) != 0)
    {
        memcpy(curve.points.data(), points, sizeof curve.points);
        for (uint32_t i = 0; i < 256; i++)
        {
            const float f = i * 16.0f / 255.0f;
            const uint32_t k = std::min(uint32_t(f), 15u);
            const float v = std::fma(points[k + 1] - points[k], f - float(k), points[k]);
            curve.bytes[i] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        memcpy(cached.data(), points, sizeof cached);
        active = true;
    }
    return &curve;
}

const std::array<uint8_t, 65536>& HalfToDisplayByte()
{
    static std::array<uint8_t, 65536> graded{};
    static std::array<float, 17> cached{};
    static bool active = false;
    const DisplayGamma* gamma = DisplayGammaCurve();
    if (!gamma)
    {
        active = false;
        return HalfToByte();
    }
    if (!active || memcmp(cached.data(), gamma->points.data(), sizeof cached) != 0)
    {
        const auto& base = HalfToByte();
        for (uint32_t h = 0; h < 65536; h++)
            graded[h] = gamma->bytes[base[h]];
        memcpy(cached.data(), gamma->points.data(), sizeof cached);
        active = true;
    }
    return graded;
}

// The guest's window scissor in host pixels. The guest clips each draw to it — a tile, a HUD
// element, a letterbox bar — and a draw that ignores it covers the whole target: with a
// full-target scissor a small black quad painted out the entire menu screen. The guest's own
// pass is PassWidth x PassHeight; the host target is the surface, so that is the scale.
VkRect2D HostScissor(const ScissorRect& clip, const vk::Surface& surface)
{
    const double sx = double(surface.width) / double(std::max(1u, PassWidth()));
    const double sy = double(surface.height) / double(std::max(1u, PassHeight()));
    const int32_t left = std::clamp<int32_t>(int32_t(std::lround(clip.left * sx)), 0, int32_t(surface.width));
    const int32_t top = std::clamp<int32_t>(int32_t(std::lround(clip.top * sy)), 0, int32_t(surface.height));
    const int32_t right = std::clamp<int32_t>(int32_t(std::lround(clip.right * sx)), 0, int32_t(surface.width));
    const int32_t bottom = std::clamp<int32_t>(int32_t(std::lround(clip.bottom * sy)), 0, int32_t(surface.height));
    // An empty rect has to stay empty: Vulkan treats a zero-extent scissor as a no-op, which is
    // what the guest asked for when it clipped the draw away.
    if (right <= left || bottom <= top)
        return VkRect2D{ { left, top }, { 0, 0 } };
    return VkRect2D{ { left, top }, { uint32_t(right - left), uint32_t(bottom - top) } };
}

bool ConvertRgba(const void* readback, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba, size_t* lit, bool applyGamma)
{
    const std::array<uint8_t, 65536>& toByte = applyGamma ? HalfToDisplayByte() : HalfToByte();
    const uint16_t* src = static_cast<const uint16_t*>(readback);
    const size_t pixels = size_t(width) * height;
    rgba.resize(pixels * 4);
    size_t litCount = 0;
    for (size_t i = 0; i < pixels; i++)
    {
        const uint16_t* in = src + i * 4;
        uint8_t* o = rgba.data() + i * 4;
        o[0] = toByte[in[0]];
        o[1] = toByte[in[1]];
        o[2] = toByte[in[2]];
        o[3] = 255;
        if (lit)
            litCount += (o[0] > 5u) + (o[1] > 5u) + (o[2] > 5u);
    }
    if (lit)
        *lit = litCount;
    return true;
}

bool ReadSurfaceImage(vk::Surface& surface, std::vector<uint8_t>& rgba, size_t* lit);
bool ResolveToRgba8(vk::Surface& surface, std::vector<uint8_t>& rgba, size_t* lit = nullptr, bool applyGamma = true)
{
    static vk::Image resolved;
    static VkBuffer copy = VK_NULL_HANDLE;
    static void* readback = nullptr;
    static uint32_t cachedWidth = 0, cachedHeight = 0;
    if (cachedWidth != surface.width || cachedHeight != surface.height)
    {
        vk::DestroyImage(g.context, resolved);
        if (copy)
            vkDestroyBuffer(g.context.device, copy, nullptr); // ponytail: its VkDeviceMemory leaks, once per resolution change
        // HOST_CACHED, not just HOST_VISIBLE: the default host-visible type on this driver is
        // write-combined, and reading 7 MB of pixels back out of it ran at 30 MB/s — 242 ms of a
        // 246 ms windowed frame, all of it in the half-to-byte loop waiting on uncached loads.
        copy = vk::CreateBuffer(g.context, VkDeviceSize(surface.width) * surface.height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback,
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        resolved = vk::CreateImage(g.context, surface.width, surface.height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        cachedWidth = surface.width;
        cachedHeight = surface.height;
    }
    if (!copy || !resolved.image)
        return false;
    // vk::Submit records, submits and waits, so no wait of our own is needed here.
    ScopedMs resolveTimed(g.perf ? &g.perfPresentResolveMs : nullptr);
    const bool ok = vk::Submit(g.context, [&](VkCommandBuffer cmd) {
        vk::SurfaceResolve(g.context, surface, cmd, resolved.image, surface.width, surface.height);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { surface.width, surface.height, 1 };
        vkCmdCopyImageToBuffer(cmd, resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, 1, &region);
    });
    if (!ok)
        return false;
    resolveTimed.Stop(); // the rest of this function is the conversion
    ScopedMs convertTimed(g.perf ? &g.perfPresentConvertMs : nullptr);
    size_t localLit = 0;
    if (!lit)
        lit = &localLit;
    const bool converted = ConvertRgba(readback, surface.width, surface.height, rgba, lit, applyGamma);
    // SurfaceResolve can return nothing for a frame whose colour image does contain it. Fall back to reading that
    // image directly rather than present a black frame.
    if (converted && *lit == 0)
    {
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            Log("renderer_vk: resolve produced no pixels; reading the colour image directly");
        }
        return ReadSurfaceImage(surface, rgba, lit);
    }
    return converted;
}

// Copies the surface's colour image to host memory without going through SurfaceResolve.
bool ReadSurfaceImage(vk::Surface& surface, std::vector<uint8_t>& rgba, size_t* lit)
{
    static VkBuffer buffer = VK_NULL_HANDLE;
    static void* mapped = nullptr;
    static uint32_t width = 0, height = 0;
    if (width != surface.width || height != surface.height)
    {
        if (buffer)
            vkDestroyBuffer(g.context.device, buffer, nullptr);
        buffer = vk::CreateBuffer(g.context, VkDeviceSize(surface.width) * surface.height * 8,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mapped, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        width = surface.width;
        height = surface.height;
    }
    if (!buffer || !surface.color.image)
        return false;
    const bool ok = vk::Submit(g.context, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = surface.color.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { surface.width, surface.height, 1 };
        vkCmdCopyImageToBuffer(cmd, surface.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        // Put the image back and tell the surface, otherwise the next SurfaceBegin/SurfaceResolve barrier transitions
        // from a layout the image is no longer in - undefined behaviour that can discard the contents.
        VkImageMemoryBarrier back{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        back.srcQueueFamilyIndex = back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        back.image = surface.color.image;
        back.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            0, nullptr, 0, nullptr, 1, &back);
    });
    if (!ok)
        return false;
    surface.colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return ConvertRgba(mapped, surface.width, surface.height, rgba, lit, true);
}

bool ReadSurface(vk::Surface& surface, std::vector<uint8_t>& rgba, bool applyGamma = true) { return ResolveToRgba8(surface, rgba, nullptr, applyGamma); }

void EndFrame();

// The size of the pixels ReadResolved returns: the image's own (host) size.
void ResolvedSize(const State::ResolvedTexture& texture, uint32_t& width, uint32_t& height)
{
    width = texture.width;
    height = texture.height;
    if (texture.index < g.textures0.slots.size() && g.textures0.slots[texture.index].width)
    {
        width = g.textures0.slots[texture.index].width;
        height = g.textures0.slots[texture.index].height;
    }
}

bool ReadResolved(const State::ResolvedTexture& texture, std::vector<uint8_t>& rgba, size_t* lit = nullptr, std::vector<uint8_t>* alpha = nullptr)
{
    constexpr size_t kMaxBytes = 16u << 20;
    if (texture.index >= g.textures0.slots.size())
    {
        Log("read_resolved: index %u out of range (slots %zu)", texture.index, g.textures0.slots.size());
        return false;
    }
    vk::Image* image = &g.textures0.slots[texture.index];
    vk::Image& sourceImage = *image;
    // The image's own size, not the entry's guest size: resolves are host-size (ResolvedSize gives callers the same).
    const uint32_t imageWidth = image->width ? image->width : texture.width, imageHeight = image->height ? image->height : texture.height;
    const size_t pixels = size_t(imageWidth) * imageHeight;
    const bool hdr = !texture.depth && image->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const size_t bytesPerPixel = hdr ? 8 : 4;
    static VkBuffer copy = VK_NULL_HANDLE;
    static void* readback = nullptr;
    static size_t copyBytes = 0;
    if (pixels * bytesPerPixel > copyBytes)
    {
        if (copy)
        {
            vkQueueWaitIdle(g.context.queue);
            vkDestroyBuffer(g.context.device, copy, nullptr); // debug/capture path: its mapped memory is small and rare
        }
        copyBytes = std::max(kMaxBytes, pixels * bytesPerPixel);
        copy = vk::CreateBuffer(g.context, copyBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (!copy)
            copyBytes = 0;
    }
    const DisplayGamma* gamma = DisplayGammaCurve();
    const VkImageView sourceView = image->view;
    const float* gpuPoints = nullptr;
    if (gamma && !hdr && !texture.depth)
    {
        static vk::Image graded;
        static uint32_t gradedWidth = 0, gradedHeight = 0;
        if (gradedWidth != imageWidth || gradedHeight != imageHeight)
        {
            vk::DestroyImage(g.context, graded);
            graded = vk::CreateImage(g.context, imageWidth, imageHeight, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            gradedWidth = imageWidth;
            gradedHeight = imageHeight;
        }
        if (graded.image)
        {
            gpuPoints = gamma->points.data();
            image = &graded;
            gamma = nullptr;
        }
    }
    // Each failure below logs its own reason.
    if (!copy || !image->image)
    {
        Log("read_resolved: buffer %d image %d (index %u of %zu)", copy != VK_NULL_HANDLE, image->image != VK_NULL_HANDLE,
            texture.index, g.textures0.slots.size());
        return false;
    }
    VkImageMemoryBarrier before{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.image = image->image;
    before.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bool resolved = true;
    const bool ok = vk::Submit(g.context, [&](VkCommandBuffer cmd) {
        if (gpuPoints)
        {
            const VkRect2D full{ { 0, 0 }, { imageWidth, imageHeight } };
            resolved = vk::ResolveImage(g.context, g.resolve, cmd, sourceImage, *image, imageWidth, imageHeight, full, false,
                VK_IMAGE_LAYOUT_UNDEFINED, false, 1.0f, VK_FORMAT_R8G8B8A8_UNORM, gpuPoints);
            if (!resolved)
                return;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &before);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { imageWidth, imageHeight, 1 };
        vkCmdCopyImageToBuffer(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, 1, &region);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &after);
    });
    if (!ok || !resolved)
    {
        Log("read_resolved: submit ok %d resolved %d (index %u %ux%u hdr %d)", ok, resolved, texture.index, texture.width,
            texture.height, hdr ? 1 : 0);
        return false;
    }
    const auto& halfBytes = HalfToDisplayByte();
    const uint8_t* src = static_cast<const uint8_t*>(readback);
    rgba.resize(pixels * 4);
    if (alpha)
        alpha->resize(pixels);
    size_t litCount = 0;
    float minDepth = 1.0f, maxDepth = 0.0f;
    for (size_t i = 0; i < pixels; i++)
    {
        uint8_t* out = rgba.data() + i * 4;
        if (texture.depth)
        {
            float f = 0;
            memcpy(&f, src + i * bytesPerPixel, sizeof f);
            minDepth = std::min(minDepth, f);
            maxDepth = std::max(maxDepth, f);
            out[0] = out[1] = out[2] = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
            litCount += out[0] > 5u;
            if (alpha)
                (*alpha)[i] = 255;
        }
        else if (hdr)
        {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(src + i * bytesPerPixel);
            for (uint32_t c = 0; c < 3; c++)
            {
                out[c] = halfBytes[in[c]];
                litCount += out[c] > 5u;
            }
            if (alpha)
                (*alpha)[i] = halfBytes[in[3]];
        }
        else
            for (uint32_t c = 0; c < 3; c++)
            {
                out[c] = gamma ? gamma->bytes[src[i * bytesPerPixel + c]] : src[i * bytesPerPixel + c];
                litCount += out[c] > 5u;
                if (alpha)
                    (*alpha)[i] = gamma ? gamma->bytes[src[i * bytesPerPixel + 3]] : src[i * bytesPerPixel + 3];
            }
        out[3] = 255;
    }
    if (texture.depth)
        Log("renderer_vk: depth %ux%u min %g max %g", imageWidth, imageHeight, minDepth, maxDepth);
    if (lit)
        *lit = litCount;
    return true;
}

void DumpPixels(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, size_t lit, const char* prefix = "vk_frame")
{
    char name[64];
    snprintf(name, sizeof(name), "out/%s_%llu.png", prefix, (unsigned long long)g.frame);
    // The path is relative to the process working directory: create the directory, honour WritePng's result and
    // report the absolute path attempted.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(name).parent_path(), ec);
    if (WritePng(name, width, height, rgba))
    {
        Log("renderer_vk: wrote %s (%ux%u, %zu lit components)", std::filesystem::absolute(name).string().c_str(),
            width, height, lit);
    }
    else
    {
        Log("renderer_vk: PNG WRITE FAILED for %s (%ux%u): %s", std::filesystem::absolute(name).string().c_str(),
            width, height, ec.message().c_str());
    }
}

// Writes a surface out as a PNG (out/vk_frame_<n>.png): the first thing to look at when the draw
// path is being brought up.
void DumpSurface(vk::Surface& surface)
{
    std::vector<uint8_t> rgba;
    size_t lit = 0;
    if (!ResolveToRgba8(surface, rgba, &lit))
    {
        Log("renderer_vk: dump failed: %ux%u surface, %zu bytes read back", surface.width, surface.height, rgba.size());
        return;
    }
    DumpPixels(surface.width, surface.height, rgba, lit);
}

void DumpResolved(const State::ResolvedTexture& texture)
{
    std::vector<uint8_t> rgba;
    size_t lit = 0;
    if (!ReadResolved(texture, rgba, &lit))
    {
        Log("renderer_vk: dump failed: %ux%u resolved texture", texture.width, texture.height);
        return;
    }
    uint32_t width, height;
    ResolvedSize(texture, width, height);
    DumpPixels(width, height, rgba, lit);
}

void ClosePass()
{
    if (!g.passOpen)
        return;
    vk::SurfaceEnd(g.context, *g.openSurface, g.cmd);
    vk::DrawResetState(g.draw);
    g.passOpen = false;
    g.openSurface = nullptr;
}

// The constant blocks, filled the way the D3D12 path fills them (renderer.cpp: DrawImpl).
// FNV over the guest bytes a texture covers, sampled: enough to notice the game rewriting one.
uint64_t GuestTextureHash(const TextureFetch& fetch)
{
    ScopedMs timed(g.perf ? &g.perfHashMs : nullptr);
    g.perfHashCalls++;
    const size_t bytes = std::min<size_t>(TextureSourceSize(fetch), 4u << 20);
    if (!bytes)
        return 0;
    const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
    uint64_t hash = 0xCBF29CE484222325ull;
    const size_t step = std::max<size_t>(4, bytes / 512 / 4 * 4);
    for (size_t i = 0; i < bytes; i += step)
    {
        hash = (hash ^ source[i]) * 0x100000001B3ull;
    }
    return hash ^ bytes;
}

uint64_t ResolvedMemoryHash(uint32_t base, const State::ResolvedTexture& texture)
{
    const size_t bytes = std::min<size_t>(size_t(texture.width) * texture.height * texture.guestBpp, 4u << 20);
    const size_t sampled = std::max<size_t>(bytes, 64);
    const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(base));
    uint64_t hash = 0xCBF29CE484222325ull;
    const size_t step = std::max<size_t>(4, sampled / 512 / 4 * 4);
    for (size_t i = 0; i < sampled; i += step)
        hash = (hash ^ source[i]) * 0x100000001B3ull;
    return hash ^ sampled;
}

const State::ResolvedTexture* ResolvedFor(const TextureFetch& fetch)
{
    const char* name = TextureFormatName(fetch.Format());
    if (name && (!strncmp(name, "DXT", 3) || !strncmp(name, "DXN", 3) || !strncmp(name, "CTX", 3)))
        return nullptr;
    auto it = g.resolved.find(fetch.Address() & 0x1FFFFFFF);
    if (it == g.resolved.end() || !it->second.valid)
        return nullptr;
    State::ResolvedTexture& texture = it->second;
    static LiveSwitch noStaleCheck("FM2_VK_NO_STALE_RESOLVES");
    if (!noStaleCheck && texture.frame != ~0ull && texture.frame + 8 < g.frame)
    {
        if (!texture.stale && g.frame - texture.memChecked >= 32)
        {
            texture.memChecked = g.frame;
            if (ResolvedMemoryHash(it->first, texture) != texture.memHash)
            {
                texture.stale = true;
                Log("texture_vk: resolve at %08x (%ux%u) is stale: memory reused", it->first, texture.width, texture.height);
            }
        }
        if (texture.stale)
            return nullptr;
    }
    return &texture;
}

uint32_t UploadBcTexture(const TextureFetch& base, const uint8_t* source, uint32_t index, bool create)
{
    const uint32_t kind = TextureBCKind(base.Format());
    if (kind < 1 || kind > 4 || base.Width() % 4 || base.Height() % 4)
        return UINT32_MAX;
    const VkFormat format = kind == 1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : kind == 2 ? VK_FORMAT_BC2_UNORM_BLOCK : kind == 3 ? VK_FORMAT_BC3_UNORM_BLOCK
        : VK_FORMAT_BC4_UNORM_BLOCK;
    const uint32_t blockBytes = kind == 1 || kind == 4 ? 8u : 16u;
    std::vector<TextureFetch> levels{ base };
    for (uint32_t level = 1;; level++)
    {
        TextureFetch next;
        if (!TextureMipLevel(base, level, next))
            break;
        levels.push_back(next);
    }
    static const bool trace = getenv("FM2_VK_TRACE_BC") != nullptr;
    if (trace)
        Log("bc_vk: upload %08x %ux%u format %u, %zu levels, create %d, index %u", base.Address(), base.Width(), base.Height(),
            base.Format(), levels.size(), int(create), index);
    for (uint32_t level = 0; level < levels.size(); level++)
    {
        const TextureFetch& fetch = levels[level];
        // Whole blocks, rounded UP: since main's packed-tail mips the chain goes down to 1x1, and a 2x2 level is still
        // one block (rounding down sized the buffer at zero and CopyTextureBlocks wrote through a null pointer).
        const uint32_t rowPitch = ((fetch.Width() + 3) / 4) * blockBytes;
        std::vector<uint8_t> blocks(size_t(rowPitch) * ((fetch.Height() + 3) / 4));
        const uint8_t* bytes = source ? source : GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
        if (!CopyTextureBlocks(fetch, bytes, blocks.data(), rowPitch))
            return UINT32_MAX;
        if (level == 0 && create)
            index = vk::TextureAddBlocks(g.context, g.textures0, base.Width(), base.Height(), format, blocks.data(), rowPitch, 0,
                uint32_t(levels.size()));
        else if (index == UINT32_MAX || !vk::TextureReplaceBlocks(g.context, g.textures0, index, base.Width(), base.Height(), format,
                     blocks.data(), rowPitch, uint32_t(levels.size()), level, create ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            return UINT32_MAX;
    }
    return index;
}

uint32_t UploadRgbaTexture(const TextureFetch& base, const uint8_t* source, uint32_t index, bool create)
{
    std::vector<TextureFetch> levels{ base };
    for (uint32_t level = 1;; level++)
    {
        TextureFetch next;
        if (!TextureMipLevel(base, level, next))
            break;
        levels.push_back(next);
    }
    for (uint32_t level = 0; level < levels.size(); level++)
    {
        const TextureFetch& fetch = levels[level];
        std::vector<uint8_t> rgba;
        if (!DecodeTexture(fetch, source ? source : GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address())), rgba))
            return UINT32_MAX;
        if (level == 0 && create)
            index = vk::TextureAdd(g.context, g.textures0, base.Width(), base.Height(), rgba.data(), 0, uint32_t(levels.size()));
        else if (index == UINT32_MAX || !vk::TextureReplace(g.context, g.textures0, index, base.Width(), base.Height(), rgba.data(),
                     uint32_t(levels.size()), level, create ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            return UINT32_MAX;
    }
    return index;
}

// One fetch slot: decode on the CPU (every format the decoder knows) and hand the shader a
// bindless index. Cube maps and unsupported formats keep the null texture for now.
void BindTextureUncached(gpu::PSConstants& pc, uint32_t slot)
{
    const TextureFetch fetch = ReadTextureFetch(slot);
    if (fetch.Type() == 2 && fetch.Dimension() == 3 && TextureFormatSupported(fetch.Format()))
    {
        g.perfBindCube++;
        const uint64_t key = uint64_t(fetch.Address()) << 24 ^ uint64_t(fetch.Format()) << 16 ^ uint64_t(fetch.Width()) ^
            (uint64_t(fetch.Height()) << 40) ^ 0xC0BEull;
        uint32_t index = 0;
        if (auto it = g.textures.find(key); it != g.textures.end())
            index = it->second.index;
        else
        {
            // Each face starts one TextureFaceStride further into guest memory. Decoding from a
            // source pointer rather than rewriting the fetch's address field: that field is only
            // accurate to 4 KB, so a linear face stride of a few hundred bytes folded every face
            // back onto the first one.
            const uint32_t stride = TextureFaceStride(fetch);
            const uint8_t* base = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
            std::vector<std::vector<uint8_t>> faces(6);
            const uint8_t* pointers[6]{};
            for (uint32_t face = 0; face < 6; face++)
            {
                if (!DecodeTexture(fetch, base + size_t(face) * stride, faces[face]))
                {
                    g.texFailed++;
                    return;
                }
                pointers[face] = faces[face].data();
            }
            index = vk::TextureAddCube(g.context, g.cubes, fetch.Width(), fetch.Height(), pointers);
            if (index == UINT32_MAX)
                return;
            // A cube decoded from all-zero memory must NOT be cached: the atlas is assembled over the first frames, so
            // the first sample can precede its content and the memoised bind would then keep the empty copy forever.
            {
                const size_t mid = ((size_t(fetch.Height() / 2) * fetch.Width() + fetch.Width() / 2) * 4);
                const bool empty = faces[0].size() > mid + 2 && faces[0][mid] == 0 && faces[0][mid + 1] == 0 && faces[0][mid + 2] == 0;
                if (!empty)
                    g.textures[key] = { index, 0, ~0ull, 0 };
            }
        }
        const uint32_t stride = TextureFaceStride(fetch);
        const vk::Image* resolvedFaces[6]{};
        uint32_t resolvedCount = 0;
        VkFormat resolvedFormat = VK_FORMAT_R8G8B8A8_UNORM;
        for (uint32_t face = 0; face < 6; face++)
        {
            auto it = g.resolved.find((fetch.Address() + face * stride) & 0x1FFFFFFF);
            if (it == g.resolved.end() || !it->second.valid || it->second.depth || it->second.width < fetch.Width() ||
                it->second.height < fetch.Height() || it->second.index >= g.textures0.slots.size())
                continue;
            resolvedFaces[face] = &g.textures0.slots[it->second.index];
            if (!resolvedCount++)
                resolvedFormat = resolvedFaces[face]->format;
        }
        auto cached = g.textures.find(key);
        if (resolvedCount && (cached == g.textures.end() || cached->second.checkedFrame != g.frame))
        {
            ClosePass();
            // Faces the title rendered are host-size; the cube matches them (the first one found).
            uint32_t cubeWidth = fetch.Width(), cubeHeight = fetch.Height();
            for (const vk::Image* face : resolvedFaces)
                if (face && face->width && face->height)
                {
                    cubeWidth = face->width;
                    cubeHeight = face->height;
                    break;
                }
            if (!vk::TextureCopyCube(g.context, g.cubes, index, g.cmd, resolvedFaces, cubeWidth, cubeHeight, resolvedFormat))
                return;
            if (cached == g.textures.end())
                cached = g.textures.emplace(key, State::CachedTexture{ index, 0, ~0ull, 0 }).first;
            cached->second.checkedFrame = g.frame;
        }
        const uint32_t clampX = (fetch.dwords[0] >> 10) & 7, clampY = (fetch.dwords[0] >> 13) & 7;
        const bool clamp = clampX != 0 && clampY != 0;
        const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
        pc.tinfo[slot][0] = (point ? (clamp ? 2u : 3u) : (clamp ? 1u : 0u)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
        if (!clamp)
            pc.tinfo[slot][0] |= (clampX ? 1u << 18 : 0u) | (clampY ? 1u << 19 : 0u);
        // Mirrored addressing (1 repeating, odd modes from 3 once), folded by the shader - as the Direct3D 12 path sets it.
        pc.tinfo[slot][0] |= (clampX == 1 ? 1u << 20 : 0u) | (clampY == 1 ? 1u << 21 : 0u) | (clampX >= 3 && (clampX & 1) ? 1u << 22 : 0u) |
            (clampY >= 3 && (clampY & 1) ? 1u << 23 : 0u);
        if (clampX >= 6 && clampY >= 6)
            pc.tinfo[slot][0] |= (1u << 16) | ((fetch.dwords[5] & 3) == 1 ? 1u << 17 : 0u);
        uint32_t swizzle = fetch.Swizzle();
        auto firstFace = g.resolved.find(fetch.Address() & 0x1FFFFFFF);
        if (firstFace != g.resolved.end() && firstFace->second.valid && firstFace->second.swapped)
            for (uint32_t c = 0; c < 4; c++)
            {
                const uint32_t sel = (swizzle >> (3 * c)) & 7;
                if (sel == 0 || sel == 2)
                    swizzle ^= (sel ^ (2 - sel)) << (3 * c);
            }
        pc.tinfo[slot][1] = fetch.Width();
        pc.tinfo[slot][2] = fetch.Height();
        // The dimension derives from the guest fetch, as on the Direct3D 12 path: a 2D texture declared a cube samples wrongly.
        pc.tinfo[slot][3] = TinfoWord(fetch, resolvedCount ? 1u : 0u);
        pc.tbias[slot][0] = fetch.LodBias();
        memcpy(&pc.tbias[slot][1], &index, 4);
        pc.tbias[slot][2] = pc.tbias[slot][3] = 1.0f;
        TraceBind(slot, fetch, g.psHash);
        g.texBound++;
        return;
    }
    // A resolve the renderer kept instead of writing to memory: sample that.
    if (const State::ResolvedTexture* found = ResolvedFor(fetch))
    {
        g.perfBindResolved++;
        const State::ResolvedTexture& resolved = *found;
        uint32_t index = resolved.index;
        if (fetch.MipMaxLevel() > 0 && fetch.MipMaxLevel() < 6 && fetch.MipAddress())
        {
            const vk::Image* levels[6]{};
            uint32_t count = 1;
            levels[0] = &g.textures0.slots[resolved.index];
            const uint32_t w = std::min(fetch.Width(), resolved.width), h = std::min(fetch.Height(), resolved.height);
            uint32_t address = fetch.MipAddress();
            for (uint32_t level = 1; level <= fetch.MipMaxLevel() && (w >> level || h >> level); level++)
            {
                const uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
                auto it = g.resolved.find(address & 0x1FFFFFFF);
                if (it == g.resolved.end() || !it->second.valid || it->second.format != resolved.format || it->second.width < lw ||
                    it->second.height < lh || it->second.index >= g.textures0.slots.size())
                    break;
                levels[count++] = &g.textures0.slots[it->second.index];
                const uint32_t bpp = resolved.format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
                const uint32_t bytes = ((lw + 31) & ~31u) * ((lh + 31) & ~31u) * bpp;
                address += std::max((bytes + 4095) & ~4095u, 4096u);
            }
            if (count > 1)
            {
                State::MipChain& chain = g.mipChains[fetch.Address()];
                if (chain.checkedFrame != g.frame)
                {
                    // The levels are host-size textures: the chain is too.
                    const uint32_t chainWidth = levels[0]->width ? levels[0]->width : w, chainHeight = levels[0]->height ? levels[0]->height : h;
                    if (chain.index == UINT32_MAX)
                        chain.index = vk::TextureReserve(g.context, g.textures0, chainWidth, chainHeight, resolved.format);
                    if (chain.index == UINT32_MAX)
                        return;
                    ClosePass();
                    if (!vk::TextureCopyMipChain(g.context, g.textures0, chain.index, g.cmd, levels, count, chainWidth, chainHeight, resolved.format))
                        return;
                    chain.checkedFrame = g.frame;
                }
                index = chain.index;
            }
        }
        const uint32_t clampX = (fetch.dwords[0] >> 10) & 7, clampY = (fetch.dwords[0] >> 13) & 7;
        const bool clamp = clampX != 0 && clampY != 0;
        const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
        pc.tinfo[slot][0] = (point ? (clamp ? 2u : 3u) : (clamp ? 1u : 0u)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
        if (!clamp)
            pc.tinfo[slot][0] |= (clampX ? 1u << 18 : 0u) | (clampY ? 1u << 19 : 0u);
        // Mirrored addressing (1 repeating, odd modes from 3 once), folded by the shader - as the Direct3D 12 path sets it.
        pc.tinfo[slot][0] |= (clampX == 1 ? 1u << 20 : 0u) | (clampY == 1 ? 1u << 21 : 0u) | (clampX >= 3 && (clampX & 1) ? 1u << 22 : 0u) |
            (clampY >= 3 && (clampY & 1) ? 1u << 23 : 0u);
        if (clampX >= 6 && clampY >= 6)
            pc.tinfo[slot][0] |= (1u << 16) | ((fetch.dwords[5] & 3) == 1 ? 1u << 17 : 0u);
        uint32_t swizzle = fetch.Swizzle();
        if (resolved.swapped)
            for (uint32_t c = 0; c < 4; c++)
            {
                const uint32_t sel = (swizzle >> (3 * c)) & 7;
                if (sel == 0 || sel == 2)
                    swizzle ^= (sel ^ (2 - sel)) << (3 * c);
            }
        pc.tinfo[slot][1] = fetch.Width();
        pc.tinfo[slot][2] = fetch.Height();
        pc.tinfo[slot][3] = 1 | (1u << 31) | (swizzle << 8);
        pc.tbias[slot][0] = fetch.LodBias();
        memcpy(&pc.tbias[slot][1], &index, 4);
        // No sub-rect UV scaling, matching the Direct3D 12 path: a zero here keeps ps.hlsl's "declared image inside a
        // wider resolved texture" branch off. With the fetch/resolved ratio it scaled every UV, and menu text (a strip
        // inside a wider target) sampled outside its glyphs.
        pc.tbias[slot][2] = 0.0f;
        pc.tbias[slot][3] = 0.0f;
        g.texBound++;
        return;
    }
    // A format the decoder refuses: write its bytes out so the layout can be worked out offline
    // instead of by guessing at the enum.
    if (fetch.Type() == 2 && fetch.Dimension() != 3 && !TextureFormatSupported(fetch.Format()))
    {
        static std::unordered_map<uint32_t, uint32_t> dumped;
        if (dumped[fetch.Format()]++ < 2)
        {
            // The size helper needs the format info we are missing, so assume the widest element:
            // 4 bytes per texel for an uncompressed guess is a safe upper bound to read back.
            size_t bytes = TextureSourceSize(fetch);
            if (!bytes)
                bytes = size_t(fetch.Width()) * fetch.Height() * 4;
            bytes = std::min<size_t>(bytes, 4u << 20);
            char path[96];
            snprintf(path, sizeof(path), "out/raw_fmt%u_%ux%u.bin", fetch.Format(), fetch.Width(), fetch.Height());
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(GuestPtr<const void>(GpuPhysToGuest(fetch.Address())), 1, bytes, f);
                fclose(f);
            }
            Log("texture_vk: dumped %s (%zu bytes, %ux%u format %u)", path, bytes, fetch.Width(), fetch.Height(), fetch.Format());
        }
    }
    // A zero dimension is a corrupt fetch constant, not a small texture (the shader divides by the texel size), so
    // count those explicitly.
    if (fetch.Width() == 0 || fetch.Height() == 0)
    {
        g.texFailed++;
        static uint32_t zeroDimsLogged = 0;
        if (zeroDimsLogged++ < 6)
            Log("texture_vk: ZERO DIMENSION bind %08x fmt %u %ux%u ps %016llx slot %u",
                fetch.Address(), fetch.Format(), fetch.Width(), fetch.Height(), (unsigned long long)g.psHash, slot);
    }
    if (fetch.Type() != 2 || fetch.Dimension() == 3 || !TextureFormatSupported(fetch.Format()))
    {
        g.perfBindRefused++;
        g.texFailed++;
        static std::unordered_map<uint32_t, uint32_t> byFormat;
        if (byFormat[fetch.Format()]++ < 3)
            // Name the shader: these fire with format 0 / type 0 / dimension 0, i.e. an all-zero fetch constant.
            Log("texture_vk: refused format %u (%s) type %u dimension %u ps %016llx slot %u addr %08x",
                fetch.Format(), TextureFormatName(fetch.Format()), fetch.Type(), fetch.Dimension(),
                (unsigned long long)g.psHash, slot, fetch.Address());
        return;
    }
    // Cache first: decoding is milliseconds (tiling, format conversion) and a draw samples the
    // same textures every frame, so only a new or rewritten one goes through the decoder.
    const uint64_t key = uint64_t(fetch.Address()) << 24 ^ uint64_t(fetch.Format()) << 16 ^ uint64_t(fetch.Width()) ^
        uint64_t(fetch.Height()) << 40 ^ uint64_t(fetch.Swizzle()) << 8;
    g.perfBind2D++;
    uint32_t index = 0;
    auto cached = g.textures.find(key);
    const bool known = cached != g.textures.end();
    // Block-compressed uploads (unless FM2_VK_NO_BC): the same images the Direct3D 12 backend samples.
    // Block-aligned sizes only, as that path requires, because a block copy cannot express a
    // partial block row.
    if (g.bcUploads && fetch.Width() % 4 == 0 && fetch.Height() % 4 == 0 && !ResolvedFor(fetch) && TextureBCKind(fetch.Format()) >= 1 &&
        TextureBCKind(fetch.Format()) <= 4)
    {
        g.perfBind2D++;
        auto it = g.textures.find(key);
        // Hash only when the answer is used: not for a texture already checked this frame, and - the policy just below,
        // the one the Direct3D 12 path uses - only every 4th, then every 16th frame for one that has stopped changing.
        // Hashing on every bind that misses the bind cache (about 600 a frame) costs 11% of the render thread.
        const bool settled = it != g.textures.end() && it->second.checkedFrame != g.frame && it->second.unchanged >= 30 &&
            ((g.frame + (key >> 32)) & (it->second.unchanged >= 300 ? 15ull : 3ull)) != 0;
        const bool checkNow = it == g.textures.end() || (it->second.checkedFrame != g.frame && !settled);
        const uint64_t hash = checkNow ? GuestTextureHash(fetch) : it->second.hash;
        uint32_t bcIndex = it != g.textures.end() ? it->second.index : 0;
        if (settled)
            it->second.checkedFrame = g.frame;
        else if (it == g.textures.end() || (it->second.checkedFrame != g.frame && it->second.hash != hash))
        {
            bcIndex = UploadBcTexture(fetch, nullptr, it == g.textures.end() ? UINT32_MAX : it->second.index, it == g.textures.end());
            if (bcIndex == UINT32_MAX)
                return;
            if (it != g.textures.end())
            {
                it->second.hash = hash;
                it->second.unchanged = 0;
            }
            else
                g.textures[key] = { bcIndex, hash, g.frame, 0 };
        }
        else if (it->second.checkedFrame != g.frame)
        {
            it->second.checkedFrame = g.frame;
            it->second.unchanged++;
        }
        if (it != g.textures.end())
            it->second.checkedFrame = g.frame;
        const uint32_t bcx = (fetch.dwords[0] >> 10) & 7, bcy = (fetch.dwords[0] >> 13) & 7;
        const bool bclamp = bcx != 0 && bcy != 0, bcpoint = ((fetch.dwords[3] >> 19) & 3) == 0;
        pc.tinfo[slot][0] = (bcpoint ? (bclamp ? 2u : 3u) : (bclamp ? 1u : 0u)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
        if (!bclamp)
            pc.tinfo[slot][0] |= (bcx ? 1u << 18 : 0u) | (bcy ? 1u << 19 : 0u);
        if (bcx >= 6 && bcy >= 6)
            pc.tinfo[slot][0] |= (1u << 16) | ((fetch.dwords[5] & 3) == 1 ? 1u << 17 : 0u);
        pc.tinfo[slot][1] = fetch.Width();
        pc.tinfo[slot][2] = fetch.Height();
        // The dimension is derived, as on the fresh path: a cube served from the cache must not be declared 2D.
        pc.tinfo[slot][3] = TinfoWord(fetch, 1u);
        pc.tbias[slot][0] = fetch.LodBias();
        memcpy(&pc.tbias[slot][1], &bcIndex, 4);
        pc.tbias[slot][2] = pc.tbias[slot][3] = 1.0f;
        TraceBind(slot, fetch, g.psHash);
        g.texBound++;
        return;
    }
    // The same policy the D3D12 path arrived at (renderer.cpp, the 2D texture cache), and it
    // matters here far more than it did there: hashing the guest bytes on every bind of every
    // draw was 3.4 ms of a 3.6 ms frame at 40 draws, and a race draws ~1 900 of them. A texture
    // already checked this frame is not hashed again; one that has not changed for 30 checks is
    // only re-checked on a staggered frame, every 4th, then every 16th.
    if (known && cached->second.checkedFrame == g.frame)
    {
        index = cached->second.index;
    }
    else if (known && cached->second.unchanged >= 30 &&
        ((g.frame + (key >> 32)) & (cached->second.unchanged >= 300 ? 15ull : 3ull)) != 0)
    {
        cached->second.checkedFrame = g.frame;
        index = cached->second.index;
    }
    else
    {
        const uint64_t hash = GuestTextureHash(fetch);
        if (known && cached->second.hash == hash)
        {
            cached->second.checkedFrame = g.frame;
            cached->second.unchanged++;
            index = cached->second.index;
        }
        else
        {
            index = UploadRgbaTexture(fetch, nullptr, known ? cached->second.index : UINT32_MAX, !known);
            if (index == UINT32_MAX)
                return;
            g.texDecoded++;
            if (known)
            {
                cached->second.hash = hash;
                cached->second.checkedFrame = g.frame;
                cached->second.unchanged = 0;
            }
            else
                g.textures[key] = { index, hash, g.frame, 0 };
        }
    }

    // The tables the pixel shader reads (see the D3D12 path's fill in renderer.cpp).
    const uint32_t clampX = (fetch.dwords[0] >> 10) & 7, clampY = (fetch.dwords[0] >> 13) & 7;
    const bool clamp = clampX != 0 && clampY != 0;
    const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
    pc.tinfo[slot][0] = (point ? (clamp ? 2u : 3u) : (clamp ? 1u : 0u)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
    if (!clamp)
        pc.tinfo[slot][0] |= (clampX ? 1u << 18 : 0u) | (clampY ? 1u << 19 : 0u);
    // Mirrored addressing (1 repeating, odd modes from 3 once), folded by the shader - as the Direct3D 12 path sets it.
    pc.tinfo[slot][0] |= (clampX == 1 ? 1u << 20 : 0u) | (clampY == 1 ? 1u << 21 : 0u) | (clampX >= 3 && (clampX & 1) ? 1u << 22 : 0u) |
        (clampY >= 3 && (clampY & 1) ? 1u << 23 : 0u);
    if (clampX >= 6 && clampY >= 6)
        pc.tinfo[slot][0] |= (1u << 16) | ((fetch.dwords[5] & 3) == 1 ? 1u << 17 : 0u);
    pc.tinfo[slot][1] = fetch.Width();
    pc.tinfo[slot][2] = fetch.Height();
    pc.tinfo[slot][3] = 1; // 2D, swizzled on the CPU while decoding
    pc.tbias[slot][0] = fetch.LodBias();
    memcpy(&pc.tbias[slot][1], &index, 4);
    pc.tbias[slot][2] = pc.tbias[slot][3] = 1.0f;
    g.texBound++;
}

void BindTexture(gpu::PSConstants& pc, uint32_t slot)
{
    // No ScopedMs here: the caller already wraps the whole slot loop in perfTexMs, and timing each bind as well
    // costs two clock reads per bind.
    g.texSlots++;
    const TextureFetch fetch = ReadTextureFetch(slot);
    uint64_t bindingKey = 0xCBF29CE484222325ull;
    for (uint32_t d : fetch.dwords)
        bindingKey = (bindingKey ^ d) * 0x100000001B3ull;
    State::SlotBinding& memo = g.bindings[(bindingKey ^ bindingKey >> 29) & (State::kBindingSlots - 1)];
    // A memo entry serves the rest of the frame while the resolve epoch and the fetch constant are unchanged.
    if (memo.frame == g.frame && memo.epoch == g.resolveEpoch && memcmp(memo.fetch, fetch.dwords, sizeof(memo.fetch)) == 0)
    {
        g.bindReused++;
        memcpy(pc.tinfo[slot], memo.tinfo, sizeof(memo.tinfo));
        memcpy(pc.tbias[slot], memo.tbias, sizeof(memo.tbias));
        return;
    }
    BindTextureUncached(pc, slot);
    memcpy(memo.fetch, fetch.dwords, sizeof(memo.fetch));
    memo.frame = g.frame;
    memo.epoch = g.resolveEpoch;
    memcpy(memo.tinfo, pc.tinfo[slot], sizeof(memo.tinfo));
    memcpy(memo.tbias, pc.tbias[slot], sizeof(memo.tbias));
}

void BuildConstants(const vk::VkShader* vs, const vk::VkShader* ps, gpu::VSConstants& vc, gpu::PSConstants& pc)
{
    const uint8_t* banks = gpureg::File() + gpureg::AluConstants * 4;
    // The translated shaders read constants well past what their "constants used" count says: the
    // sky shader's final colour is pc[254] + pc[255] and friends, while the count stops far below
    // that. The whole bank is uploaded: 4 KB per draw.
    {
        ScopedMs bankTimed(g.perf ? &g.perfBankMs : nullptr);
        const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        const auto* src = reinterpret_cast<const __m128i*>(banks);
        auto* dv = reinterpret_cast<__m128i*>(vc.c);
        auto* dp = reinterpret_cast<__m128i*>(pc.c);
        for (uint32_t i = 0; i < 256; i++)
            _mm_storeu_si128(dv + i, _mm_shuffle_epi8(_mm_loadu_si128(src + i), swap));
        for (uint32_t i = 0; i < 256; i++)
            _mm_storeu_si128(dp + i, _mm_shuffle_epi8(_mm_loadu_si128(src + 256 + i), swap));
    }
    for (uint32_t i = 0; i < 8; i++)
        vc.b[i] = pc.b[i] = gpureg::Get(gpureg::BoolConstants + i);
    // Vertex fetch: where each buffer is (vf) and the shader's layout of it (vfl).
    for (uint32_t k : vs->vertexFetchConstants)
    {
        const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + k * 2), d1 = gpureg::Get(gpureg::FetchConstants + k * 2 + 1);
        vc.vf[k][0] = (d0 & 0xFFFFFFFC) & 0x1FFFFFFF; // physical offset inside gphys
        vc.vf[k][1] = d1 & 3;                         // endian
        vc.vf[k][2] = 1;                              // read from gphys
        vc.vf[k][3] = ((d1 >> 2) & 0xFFFFFF) * 4;     // buffer size in bytes (DrawList copies small ones)
    }
    const size_t layoutBytes = std::min(vs->vertexFetchLayout.size() * sizeof(uint32_t), sizeof(vc.vfl));
    if (layoutBytes)
        memcpy(vc.vfl, vs->vertexFetchLayout.data(), layoutBytes);
    vc.info[1] = 0; // position mode: filled from the initiator in OnDraw
    vc.vscale[0] = RegFloat(0x210F);
    vc.vscale[1] = RegFloat(0x2111);
    vc.vscale[2] = RegFloat(0x2113);
    vc.vscale[3] = 0;
    vc.voffset[0] = RegFloat(0x2110);
    vc.voffset[1] = RegFloat(0x2112);
    vc.voffset[2] = RegFloat(0x2114);
    vc.voffset[3] = 0;
    vc.vte[0] = gpureg::Get(0x2206); // PA_CL_VTE_CNTL
    vc.vte[1] = vc.vte[2] = vc.vte[3] = 0;
    vc.vwide[0] = vc.vwide[1] = vc.vwide[2] = vc.vwide[3] = 1.0f;
    // The target size and the point-sprite sizes, as the D3D12 path sets them: 2D draws and
    // point sprites are placed from these, not from the viewport.
    uint32_t targetWidth = 0, targetHeight = 0;
    RenderSize(targetWidth, targetHeight);
    // The guest's pass size, not the host target (see PassWidth above).
    vc.info[2] = PassWidth();
    vc.info[3] = PassHeight();
    const uint32_t pointSize = gpureg::Get(0x2085 /* PA_SU_POINT_SIZE */), pointMinMax = gpureg::Get(0x2089 /* PA_SU_POINT_MINMAX */);
    vc.point[0] = float(pointSize >> 16) / 8.0f;
    vc.point[1] = float(pointSize & 0xFFFF) / 8.0f;
    vc.point[2] = float(pointMinMax & 0xFFFF) / 8.0f;
    vc.point[3] = float(pointMinMax >> 16) / 8.0f;
    // Pixel shader: alpha test and the texture tables. Indices point at the null texture until
    // the texture capture exists (decode + upload), so a draw samples white rather than garbage.
    const uint32_t colorControl = gpureg::Get(0x2202 /* RB_COLORCONTROL */);
    pc.info[0] = (colorControl >> 3) & 1 ? colorControl & 7 : 7;
    // The register the pixel shader reads its generated parameter in — the pixel position. The
    // guest enables it in SQ_PROGRAM_CNTL and names the register in SQ_CONTEXT_MISC (this is the
    // D3D12 path's expression, renderer.cpp). Saying "none" leaves that register zero, and a
    // shader that takes its texture coordinates from it samples one texel for the whole surface:
    // the title screen came out black with only its text on top.
    pc.info[1] = (gpureg::Get(0x2180 /* SQ_PROGRAM_CNTL */) >> 18) & 1 ? (gpureg::Get(0x2181 /* SQ_CONTEXT_MISC */) >> 8) & 0xFF : 0xFF;
    // The fields main added to the shader contract (gpu/constants.h), filled as the D3D12 path fills them.
    // Loop constants, for both stages.
    for (uint32_t i = 0; i < 32; i++)
        vc.l[i] = pc.l[i] = gpureg::Get(gpureg::LoopConstants + i);
    // Forced absolute LOD: -1 means implicit. Left at zero it would force mip 0 on every fetch.
    static const float forceLod = getenv("FM2_FORCE_LOD") ? float(atof(getenv("FM2_FORCE_LOD"))) : -1.0f;
    pc.forceLod[0] = forceLod;
    pc.forceLod[1] = pc.forceLod[2] = pc.forceLod[3] = 0.0f;
    // Colour exponent bias (RB_COLOR_INFO bits 20-25, signed) and the 16_16 targets' -32..32 range (pinfo.z bit 3).
    {
        static const bool noColorExpBias = getenv("FM2_NO_COLOR_EXP_BIAS") != nullptr;
        const uint32_t colorInfo = gpureg::Get(0x2001 /* RB_COLOR_INFO */);
        pc.info[3] = noColorExpBias ? 0u : uint32_t(int32_t(colorInfo << 6) >> 26);
        const uint32_t colorFormat16 = (colorInfo >> 16) & 0xF;
        pc.info[2] = !noColorExpBias && (colorFormat16 == 4 || colorFormat16 == 5) ? 8u : 0u;
    }
    pc.info2[0] = RegFloat(0x210E /* RB_ALPHA_REF */);
    // Pixel position back to game pixels: the guest's pass, over the host target (1.0 at native).
    pc.info2[1] = float(PassWidth()) / float(targetWidth ? targetWidth : 1);
    pc.info2[2] = float(PassHeight()) / float(targetHeight ? targetHeight : 1);
    const uint32_t colorFormat = (gpureg::Get(0x2001 /* RB_COLOR_INFO */) >> 16) & 0xF;
    pc.info2[3] = colorFormat == 3 || colorFormat == 12 ? 31.875f : 0.0f;
    for (int slot = 0; slot < 32; slot++)
    {
        pc.tinfo[slot][0] = 0; // linear wrap, 2D
        pc.tinfo[slot][1] = 0;
        pc.tinfo[slot][2] = 0;
        pc.tinfo[slot][3] = 0;
        pc.tbias[slot][0] = 0.0f;
        pc.tbias[slot][1] = 0.0f; // bindless index 0
        pc.tbias[slot][2] = 0.0f;
        pc.tbias[slot][3] = 0.0f;
    }
    // Every fetch slot either shader samples: decode, upload, and put the bindless index in
    // the tables the shader reads.
    bool texNeedsPatch = false;
    {
        ScopedMs texTimed(g.perf ? &g.perfTexMs : nullptr);
        // Walk only the set bits of the used-slot mask: the mask is fixed for the shader pair.
        const uint32_t usedMask = ps->usedTextureSlots | vs->usedTextureSlots;
        for (uint32_t bits = usedMask; bits; bits &= bits - 1)
        {
                const uint32_t slot = uint32_t(__builtin_ctz(bits));
                g.perfTexCalls++;
                BindTexture(pc, slot);
                // The second pass below only patches slots whose fetch reported a 0x0 size: note whether any did,
                // so the common case (all bound) skips it.
                if (pc.tinfo[slot][1] == 0)
                    texNeedsPatch = true;
            }
    }
    // A slot the shader declares but the draw never supplies a fetch for keeps index 0 — the white
    // null texture — but a tinfo size of 0x0, and a sample scaled by zero is not a white pixel: it
    // lands on whatever texel the arithmetic produces, which draws a 1-pixel stripe pattern of the
    // wrong texture across the surface. Give every unbound slot a 1x1 size so sampling it is a
    // constant, which is what a missing texture should look like.
    if (texNeedsPatch)
    for (uint32_t slot = 0; slot < 32; slot++)
        if (((ps->usedTextureSlots | vs->usedTextureSlots) & (1u << slot)) && pc.tinfo[slot][1] == 0)
        {
            pc.tinfo[slot][1] = 1;
            pc.tinfo[slot][2] = 1;
            pc.tinfo[slot][3] = 1;
        }
}
}

bool VulkanRendererEnabled()
{
    static const bool enabled = GetSettings().renderer == Settings::Renderer::Vulkan;
    return enabled;
}

// Called from RendererSaveCaches so the Vulkan pipeline cache actually lands on disk. Guarded: the
// device may not exist (the seam can be enabled without a successful Init).
void RendererVkSavePipelineCache()
{
    // Log the guard's inputs, so a missing pipelines.bin can be told from a save that never ran.
    fprintf(stderr, "vk: save pipeline cache: initialised %d failed %d device %d path %s\n", int(g.initialised), int(g.failed),
        g.context.device != VK_NULL_HANDLE, g.context.pipelineCachePath.string().c_str());
    if (g.initialised && !g.failed && g.context.device)
        vk::SavePipelineCache(g.context);
}

namespace
{
// FM2_VK_SAMPLE=1: a sampling profile of the render thread, for a build that has no profiler to hand (Wine, no perf).
// A second thread suspends this one about 2000 times a second, notes its instruction pointer and resumes it; at
// 5000 samples (and every 5000 after) the histogram goes to out/vk_samples.txt as "count address" with addresses
// relative to the executable's base (-1 = outside it: the driver, ntdll, the C runtime). Resolve them with
// llvm-symbolizer --obj=fm2.exe. The FM2_PERF timers cost 17% of the frame and are not a substitute.
void StartSampler()
{
#if defined(_WIN32)
    if (!getenv("FM2_VK_SAMPLE"))
        return;
    HANDLE thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    std::thread([thread] {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const uintptr_t size = nt->OptionalHeader.SizeOfImage;
        std::unordered_map<uint64_t, uint32_t> counts;
        // Samples outside the executable, by the nearest return address inside it found on the stack: who called out.
        std::unordered_map<uint64_t, uint32_t> callers;
        // The first few thousand samples inside the executable, with the code addresses found on the stack above
        // them: enough to see who calls a hot leaf (memcpy, getenv).
        std::vector<std::array<uint64_t, 10>> stacks;
        uint64_t total = 0;
        const double startAfter = getenv("FM2_VK_SAMPLE_AFTER") ? atof(getenv("FM2_VK_SAMPLE_AFTER")) : 0.0;
        const auto begin = std::chrono::steady_clock::now();
        for (;;)
        {
            Sleep(0);
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(500);
            while (std::chrono::steady_clock::now() < until)
                YieldProcessor();
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() < startAfter)
                continue;
            if (SuspendThread(thread) == DWORD(-1))
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            uint64_t rip = 0, caller = 0;
            if (GetThreadContext(thread, &context))
            {
                rip = context.Rip;
                if (rip - base < size && stacks.size() < 6000)
                {
                    std::array<uint64_t, 10> chain{};
                    chain[0] = rip - base;
                    uint32_t n = 1;
                    const uint64_t* stack = reinterpret_cast<const uint64_t*>(context.Rsp);
                    MEMORY_BASIC_INFORMATION info{};
                    if (VirtualQuery(stack, &info, sizeof info) && info.State == MEM_COMMIT)
                    {
                        const uint64_t* end = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize);
                        for (uint32_t i = 0; i < 400 && stack + i < end && n < chain.size(); i++)
                            if (stack[i] - base < size && stack[i] - base >= 0x1000)
                                chain[n++] = stack[i] - base;
                    }
                    stacks.push_back(chain);
                }
                if (rip - base >= size)
                {
                    // Not ours: look up the stack for the first value that points into the executable's code.
                    const uint64_t* stack = reinterpret_cast<const uint64_t*>(context.Rsp);
                    MEMORY_BASIC_INFORMATION info{};
                    if (VirtualQuery(stack, &info, sizeof info) && info.State == MEM_COMMIT)
                    {
                        const uint64_t* end = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize);
                        for (uint32_t i = 0; i < 512 && stack + i < end; i++)
                            if (stack[i] - base < size && stack[i] - base >= 0x1000)
                            {
                                caller = stack[i];
                                break;
                            }
                    }
                }
            }
            ResumeThread(thread);
            if (!rip)
                continue;
            if (rip - base < size)
                counts[rip - base]++;
            else
            {
                counts[~0ull]++;
                callers[caller ? caller - base : ~0ull]++;
            }
            if (++total % 5000 == 0)
            {
                std::filesystem::create_directories("out");
                if (FILE* f = fopen("out/vk_samples.txt", "w"))
                {
                    fprintf(f, "# %llu samples, image base %llx size %llx\n", (unsigned long long)total, (unsigned long long)base,
                        (unsigned long long)size);
                    for (const auto& [address, count] : counts)
                        fprintf(f, "%u %llx\n", count, (unsigned long long)address);
                    for (const auto& [address, count] : callers)
                        fprintf(f, "caller %u %llx\n", count, (unsigned long long)address);
                    for (const auto& chain : stacks)
                    {
                        fprintf(f, "stack");
                        for (uint64_t address : chain)
                            if (address)
                                fprintf(f, " %llx", (unsigned long long)address);
                        fprintf(f, "\n");
                    }
                    fclose(f);
                }
            }
        }
    }).detach();
#endif
}

// First boot: the game waits here, with the compile screen up, until every shader in the game data is in the disk
// cache - as the Direct3D 12 path does (PrecompileAll). Compiling them behind a running game left a multi-second
// frame at the first menu. A boot whose cache is already complete only checks it (a few seconds, nothing
// shown): every shader is checked rather than a sample, because an interrupted first boot leaves the head of the list
// cached and the rest not. FM2_VK_NO_BOOT_COMPILE=1 skips the wait (the warm-up
// threads carry on in the background).
void FirstBootShaderWait()
{
    if (getenv("FM2_VK_NO_BOOT_COMPILE") || getenv("FM2_HEADLESS"))
        return;
    const auto start = std::chrono::steady_clock::now();
    auto lastShown = std::chrono::steady_clock::time_point{};
    uint32_t done = 0, total = 0, compiled = 0;
    bool shown = false;
    uint32_t outWidth = 0, outHeight = 0;
    RenderSize(outWidth, outHeight);
    std::vector<uint8_t> screen, scaled;
    while (vk::ShaderWarmProgress(done, total, compiled))
    {
        const auto now = std::chrono::steady_clock::now();
        const double waited = std::chrono::duration<double>(now - start).count();
        // Show the screen once there is something to wait for: a real compile, or a list that is slow to arrive.
        if ((compiled || waited > 1.0) && now - lastShown > std::chrono::milliseconds(100))
        {
            lastShown = now;
            if (BootScreenCompose(total ? L"COMPILING SHADERS" : L"PREPARING SHADERS", done, total, screen) &&
                screen.size() == size_t(kWidth) * kHeight * 4)
            {
                shown = true;
                if (outWidth == kWidth && outHeight == kHeight)
                    PresenterSubmitImage(kWidth, kHeight, std::vector<uint8_t>(screen));
                else if (outWidth && outHeight)
                {
                    // The screen is composed at 1280x720; stretch it to the window (bilinear).
                    scaled.resize(size_t(outWidth) * outHeight * 4);
                    for (uint32_t y = 0; y < outHeight; y++)
                    {
                        const float fy = std::max(0.0f, (y + 0.5f) * kHeight / outHeight - 0.5f);
                        const uint32_t y0 = std::min<uint32_t>(uint32_t(fy), kHeight - 1), y1 = std::min<uint32_t>(y0 + 1, kHeight - 1);
                        const uint32_t wy = uint32_t((fy - float(y0)) * 256.0f);
                        const uint8_t* row0 = screen.data() + size_t(y0) * kWidth * 4;
                        const uint8_t* row1 = screen.data() + size_t(y1) * kWidth * 4;
                        uint8_t* out = scaled.data() + size_t(y) * outWidth * 4;
                        for (uint32_t x = 0; x < outWidth; x++)
                        {
                            const float fx = std::max(0.0f, (x + 0.5f) * kWidth / outWidth - 0.5f);
                            const uint32_t x0 = std::min<uint32_t>(uint32_t(fx), kWidth - 1), x1 = std::min<uint32_t>(x0 + 1, kWidth - 1);
                            const uint32_t wx = uint32_t((fx - float(x0)) * 256.0f);
                            for (uint32_t c = 0; c < 4; c++)
                            {
                                const uint32_t top = row0[x0 * 4 + c] * (256 - wx) + row0[x1 * 4 + c] * wx;
                                const uint32_t bottom = row1[x0 * 4 + c] * (256 - wx) + row1[x1 * 4 + c] * wx;
                                out[x * 4 + c] = uint8_t((top * (256 - wy) + bottom * wy) >> 16);
                            }
                        }
                    }
                    PresenterSubmitImage(outWidth, outHeight, std::vector<uint8_t>(scaled));
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (compiled)
        Log("renderer_vk: first boot: %u shaders compiled before the game went on (%.1f s, %u of %u checked, screen %s)", compiled,
            seconds, done, total, shown ? "shown" : "not available");
    else if (seconds > 0.5)
        Log("renderer_vk: shader cache complete (%.1f s to check)", seconds);
}

bool Init()
{
    if (g.initialised)
        return !g.failed;
    g.initialised = true;
    g.perf = getenv("FM2_PERF") && atoi(getenv("FM2_PERF")) != 0;
    g.perfWindow = std::chrono::steady_clock::now();
    // Echo the environment the renderer actually received. Cold path, one line per run.
    {
        const char* const perfEnv = getenv("FM2_PERF");
        const char* const drawsEnv = getenv("FM2_VK_DRAWS");
        const char* const rendererEnv = getenv("FM2_RENDERER");
        const char* const tourEnv = getenv("FM2_TOUR");
        Log("renderer_vk: env FM2_PERF=%s FM2_VK_DRAWS=%s FM2_RENDERER=%s FM2_TOUR=%s",
            perfEnv ? perfEnv : "unset", drawsEnv ? drawsEnv : "unset",
            rendererEnv ? rendererEnv : "unset", tourEnv ? "set" : "unset");
    }
    // One root for both caches, so neither can be left behind on the old vendor: vk::Init appends
    // <vendorID>-<deviceID> to the pipeline cache name, and the shader root below joins the same
    // root. FM2_VK_CACHE_DIR moves BOTH.
    const std::filesystem::path cacheRoot = getenv("FM2_VK_CACHE_DIR")
        ? std::filesystem::path(getenv("FM2_VK_CACHE_DIR"))
        : UserDir() / "cache" / "vulkan";
    if (!vk::Init(g.context, cacheRoot))
    {
        Log("renderer_vk: no Vulkan device");
#if defined(_WIN32)
        if (!getenv("FM2_HEADLESS") && !getenv("FM2_TOUR"))
            MessageBoxA(nullptr,
                "Vulkan could not start. Install or update your graphics driver, or select Direct3D 12 in the launcher. See out\\play.log for details.",
                "Forza Motorsport 2 ReGPU", MB_OK | MB_ICONERROR);
#endif
        g.failed = true;
        return false;
    }
    Log("renderer_vk: %s, Vulkan renderer ready", g.context.props.deviceName);
    // Shader caches are per device, and the device is known only after vk::Init.
    g.shaders.cacheDir = cacheRoot / (std::to_string(g.context.props.vendorID) + "-" + std::to_string(g.context.props.deviceID));
    if (!getenv("FM2_VK_NO_BC"))
    {
        VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        vkGetPhysicalDeviceFeatures2(g.context.physical, &query);
        g.bcUploads = query.features.textureCompressionBC == VK_TRUE;
        Log("renderer_vk: block-compressed uploads %s", g.bcUploads ? "on" : "unavailable, using decoded uploads");
    }
    if (!vk::TextureCacheInit(g.context, g.textures0, getenv("FM2_SMALL_DESCRIPTOR_LAYOUT") ? uint32_t(atoi(getenv("FM2_SMALL_DESCRIPTOR_LAYOUT"))) : 1u << 16))
    {
        g.failed = true;
        return false;
    }
    if (!vk::TextureCacheInit(g.context, g.cubes, 64, true))
    {
        g.failed = true;
        return false;
    }
    {
        // Slot 0 of the cube array is white too, so a draw that has no cube map samples white
        // rather than an unbound descriptor.
        const uint8_t white[4] = { 255, 255, 255, 255 };
        const uint8_t* faces[6] = { white, white, white, white, white, white };
        vk::TextureAddCube(g.context, g.cubes, 1, 1, faces);
    }
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.maxLod = 32.0f;
    static LiveSwitch noAniso("FM2_NO_ANISO");
    for (uint32_t i = 0; i < 4; i++)
    {
        // The launcher's anisotropic filtering setting (0 off, 2-16), as the Direct3D 12 path reads it. It matters now that
        // pixel shaders sample with mips: 16x is a real cost on an integrated GPU.
        const uint32_t wanted = GetSettings().anisotropy;
        const bool anisotropic = i == 0 && !noAniso && wanted >= 2 && g.context.props.limits.maxSamplerAnisotropy > 1.0f;
        info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = anisotropic ? std::min(float(std::min(wanted, 16u)), g.context.props.limits.maxSamplerAnisotropy) : 1.0f;
        info.magFilter = info.minFilter = i < 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.mipmapMode = i < 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = i == 1 || i == 2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
            : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (vkCreateSampler(g.context.device, &info, nullptr, &g.samplers[i]) != VK_SUCCESS)
        {
            g.failed = true;
            return false;
        }
    }

    // Bindless index 0 is the null texture: white, so a draw whose textures are not captured yet
    // reads white instead of an unbound descriptor.
    const uint8_t white[4] = { 255, 255, 255, 255 };
    if (vk::TextureAdd(g.context, g.textures0, 1, 1, white) != 0)
    {
        Log("renderer_vk: could not create the null texture");
        g.failed = true;
        return false;
    }
    // The guest's physical memory window, wrapped rather than copied: the vertex shaders read
    // vertex data straight out of it (gpu/memory.h maps the guest's 4 GB at g_base).
    // Importing the whole 512 MB window can fail with VK_ERROR_OUT_OF_DEVICE_MEMORY on an integrated GPU, and the
    // import is what makes vertex data readable at all. FM2_VK_GP_MB shrinks the window.
    {
        static const char* gpMb = getenv("FM2_VK_GP_MB");
        VkDeviceSize window = 0x20000000ull;
        if (gpMb)
        {
            const unsigned mb = unsigned(strtoul(gpMb, nullptr, 10));
            if (mb >= 16 && mb <= 512)
                window = VkDeviceSize(mb) << 20;
        }
        g.gphys = vk::CreateHostImportedBuffer(g.context, GuestPtr<uint8_t>(0xA0000000), window);
        if (gpMb)
            Log("renderer_vk: guest window %u MB", unsigned(window >> 20));
    }
    if (!g.gphys)
        Log("renderer_vk: guest memory is not importable; vertex data will read as zero");
    vk::ShaderWarmStart(g.shaders);
    FirstBootShaderWait();
    StartSampler();
    if (!vk::ResolveInit(g.context, g.resolve, g.shaders.cacheDir))
    {
        Log("renderer_vk: resolve pass failed");
        g.failed = true;
        return false;
    }
    const VkDeviceSize listBytes = getenv("FM2_VK_GUEST_RING_MB")
        ? std::clamp<VkDeviceSize>(strtoull(getenv("FM2_VK_GUEST_RING_MB"), nullptr, 10), 16, 1024) << 20
        : 384u << 20;
    if (!vk::DrawInit(g.context, g.draw, g.gphys, g.samplers, g.textures0.set, g.cubes.set, 128u << 20, listBytes))
    {
        Log("renderer_vk: draw context failed");
        g.failed = true;
        return false;
    }
    if (UseAsyncPresent())
    {
        if (!vk::DrawInit(g.context, g_asyncOther, g.gphys, g.samplers, g.textures0.set, g.cubes.set, 128u << 20, listBytes))
        {
            Log("renderer_vk: async draw context failed");
            g.failed = true;
            return false;
        }
        for (AsyncFrame& frame : g_async)
        {
            VkCommandBufferAllocateInfo asyncAllocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            asyncAllocate.commandPool = g.context.pool;
            asyncAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            asyncAllocate.commandBufferCount = 1;
            const VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkAllocateCommandBuffers(g.context.device, &asyncAllocate, &frame.cmd) != VK_SUCCESS ||
                AsyncFences().create(g.context.device, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS)
            {
                g.failed = true;
                return false;
            }
        }
        g_asyncIndex = 0;
        g.cmd = g_async[0].cmd;
    }
    VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = g.context.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!UseAsyncPresent() && vkAllocateCommandBuffers(g.context.device, &allocate, &g.cmd) != VK_SUCCESS)
    {
        g.failed = true;
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(g.cmd, &beginInfo);
    return true;
}

void SubmitAsyncFrame(const State::ResolvedTexture* front)
{
    g.perfPreFetchMs += g.draw.perfPreFetchMs;
    g.perfFetchLoopMs += g.draw.perfFetchLoopMs;
    g.perfVertexMs += g.draw.perfVertexMs;
    g.perfConstantCopyMs += g.draw.perfConstantsMs;
    g.perfDescriptorMs += g.draw.perfBindMs;
    g.perfDrawCallMs += g.draw.perfCallMs;
    AsyncFrame& current = g_async[g_asyncIndex];
    const bool haveFront = front && front->index < g.textures0.slots.size();
    current.hasImage = haveFront || g.lastDrawSurface;
    if (current.hasImage)
    {
        // The window's image is made on the GPU: the front buffer stretched to the output size with the display gamma
        // curve, as RGBA8.
        uint32_t width = 0, height = 0;
        RenderSize(width, height);
        width = std::clamp<uint32_t>(width, 16, 8192);
        height = std::clamp<uint32_t>(height, 16, 8192);
        if (current.width != width || current.height != height)
        {
            vk::DestroyImage(g.context, current.resolved);
            if (current.copy)
                vkDestroyBuffer(g.context.device, current.copy, nullptr);
            current.resolved = vk::CreateImage(g.context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            current.copy = vk::CreateBuffer(g.context, VkDeviceSize(width) * height * 4,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, &current.readback, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            current.width = width;
            current.height = height;
            current.resolvedUsed = false;
        }
        if (!current.resolved.image || !current.copy)
        {
            g.failed = true;
            return;
        }
        const DisplayGamma* gamma = DisplayGammaCurve();
        vk::Image* presentSource = nullptr;
        if (haveFront)
            presentSource = &g.textures0.slots[front->index];
        else
        {
            vk::SurfaceShaderRead(g.context, *g.lastDrawSurface, g.cmd);
            presentSource = &g.lastDrawSurface->color;
        }
        if (!vk::PresentImage(g.context, g.resolve, g.cmd, *presentSource, current.resolved,
                current.resolvedUsed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, gamma ? gamma->points.data() : nullptr))
        {
            g.failed = true;
            return;
        }
        current.resolvedUsed = true;
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { current.width, current.height, 1 };
        vkCmdCopyImageToBuffer(g.cmd, current.resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current.copy, 1, &region);
    }
    FrameGpuTimesEndQuery();
    current.gpuQuery = g.frameGpuStarted ? g.frameGpuQuery : UINT32_MAX;
    current.framebufferCount = g.resolve.pendingFramebuffers.size();
    current.viewCount = g.resolve.pendingViews.size();
    current.setCount = g.resolve.pendingSets.size();
    VkResult ended;
    {
        ScopedMs timed(g.perf ? &g.perfEndMs : nullptr);
        ended = vkEndCommandBuffer(g.cmd);
    }
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.cmd;
    const VkResult reset = AsyncFences().reset(g.context.device, 1, &current.fence);
    vk::FlushUploads(g.context); // this frame's texture uploads must be queued ahead of the draws that sample them
    VkResult submitted;
    {
        ScopedMs timed(g.perf ? &g.perfQueueMs : nullptr);
        submitted = ended == VK_SUCCESS && reset == VK_SUCCESS
            ? vkQueueSubmit(g.context.queue, 1, &submit, current.fence)
            : ended == VK_SUCCESS ? reset : ended;
    }
    if (ended != VK_SUCCESS || submitted != VK_SUCCESS)
    {
        Log("renderer_vk: async submit end %d, submit %d", int(ended), int(submitted));
        g.failed = true;
        return;
    }
    current.pending = true;
    g_asyncIndex ^= 1;
    g.cmd = g_async[g_asyncIndex].cmd;
    AsyncPresentPrevious(true);
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(g.cmd, &beginInfo);
    vk::DrawResetState(g.draw);
}

void AsyncPresentPrevious(bool swapDraw)
{
    AsyncFrame& previous = g_async[g_asyncIndex];
    if (!previous.pending)
    {
        if (swapDraw)
            std::swap(g.draw, g_asyncOther);
        return;
    }
    VkResult waited;
    {
        ScopedMs timed(g.perf ? &g.perfWaitMs : nullptr);
        waited = AsyncFences().wait(g.context.device, 1, &previous.fence, VK_TRUE, UINT64_MAX);
    }
    if (waited != VK_SUCCESS)
    {
        Log("renderer_vk: async wait %d", int(waited));
        g.failed = true;
        return;
    }
    vk::ResolveFrameComplete(g.context, g.resolve, previous.framebufferCount, previous.viewCount, previous.setCount);
    previous.pending = false;
    if (previous.hasImage)
    {
        ScopedMs convertTimed(g.perf ? &g.perfPresentConvertMs : nullptr);
        // Already RGBA8 with the gamma curve applied (PresentImage): one copy out of the mapped buffer.
        const uint8_t* pixels = static_cast<const uint8_t*>(previous.readback);
        // The storage comes back from the presenter (the image it showed before): allocating 8 MB a frame and paging
        // it in cost more than the copy itself.
        static std::vector<uint8_t> rgba;
        const size_t bytes = size_t(previous.width) * previous.height * 4;
        if (rgba.size() != bytes)
            rgba.resize(bytes);
        memcpy(rgba.data(), pixels, bytes);
        {
            ScopedMs handoffTimed(g.perf ? &g.perfPresentHandoffMs : nullptr);
            RendererOverlayImage(previous.width, previous.height, rgba);
            rgba = PresenterExchangeImage(previous.width, previous.height, std::move(rgba));
        }
    }
    if (swapDraw)
    {
        vk::DrawBeginFrame(g_asyncOther);
        std::swap(g.draw, g_asyncOther);
    }
}

void EndFrame()
{
    if (UseAsyncPresent())
        AsyncPresentPrevious(false);
    if (!g.cmd)
        return;
    FrameGpuTimesEndQuery();
    const VkResult ended = vkEndCommandBuffer(g.cmd);
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.cmd;
    vk::FlushUploads(g.context); // as in SubmitAsyncFrame
    const VkResult submitted = ended == VK_SUCCESS ? vkQueueSubmit(g.context.queue, 1, &submit, VK_NULL_HANDLE) : ended;
    // This path stalls the GPU once per frame: the submit carries no fence, so a full idle is the only
    // way to know the frame is done - needed both for ResolveFrameComplete's recycling and before the
    // single command buffer is reset below. The asynchronous path (the default) waits on the previous
    // frame's fence instead.
    // Accumulate the per-draw marks as SubmitAsyncFrame does: DrawBeginFrame zeroes draw.perf* every frame.
    if (g.perf && !UseAsyncPresent())
    {
        g.perfPreFetchMs += g.draw.perfPreFetchMs;
        g.perfFetchLoopMs += g.draw.perfFetchLoopMs;
        g.perfVertexMs += g.draw.perfVertexMs;
        g.perfConstantCopyMs += g.draw.perfConstantsMs;
        g.perfDescriptorMs += g.draw.perfBindMs;
        g.perfDrawCallMs += g.draw.perfCallMs;
    }
    static bool warned = false;
    if (!warned && !UseAsyncPresent())
    {
        warned = true;
        Log("renderer_vk: synchronous present: one vkQueueWaitIdle per frame");
    }
    // The wait cannot be skipped when nothing is pending: the same single command buffer is reset and
    // re-begun below, so the idle is also what stops the CPU from rewriting a buffer the GPU is still
    // executing. Timed into perfWaitMs, the same counter the async fence wait uses.
    VkResult waited = submitted;
    if (submitted == VK_SUCCESS)
    {
        ScopedMs idleTimed(g.perf ? &g.perfWaitMs : nullptr);
        waited = vkQueueWaitIdle(g.context.queue);
    }
    if (waited == VK_SUCCESS)
        vk::ResolveFrameComplete(g.context, g.resolve);
    static int reported = 0;
    if ((submitted != VK_SUCCESS || waited != VK_SUCCESS) && reported++ < 3)
        Log("renderer_vk: frame submit end %d, submit %d, wait %d", int(ended), int(submitted), int(waited));
    if (submitted != VK_SUCCESS || waited != VK_SUCCESS)
    {
        g.failed = true;
        return;
    }
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(g.cmd, &beginInfo);
    vk::DrawResetState(g.draw);
}
}

// ---- Shaders ----------------------------------------------------------------------------
void RendererVkOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords)
{
    if (!Init())
        return;
    std::vector<uint32_t>& code = type == 0 ? g.vsCode : g.psCode;
    // Only the dwords the shader uses, as the Direct3D 12 path and the shader scanners take them: a load packet can
    // carry more, and the untrimmed copy was a different shader to the cache and to the warm-up.
    code.assign(microcodeBE, microcodeBE + ucode::UsedDwords(microcodeBE, dwords));
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t w : code)
        hash = (hash ^ w) * 0x100000001B3ull;
    hash ^= type ? 1u : 0u;
    (type == 0 ? g.vsHash : g.psHash) = hash;
}

void RendererVkSetShader(uint32_t type, const uint32_t* microcodeBE, uint32_t usedDwords, uint64_t hash)
{
    std::vector<uint32_t>& code = type == 0 ? g.vsCode : g.psCode;
    code.assign(microcodeBE, microcodeBE + usedDwords);
    (type == 0 ? g.vsHash : g.psHash) = hash;
}

// ---- Draws ------------------------------------------------------------------------------
void RendererVkOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    // Read once, not per draw: a getenv() in the per-draw path is measurable.
    static const char* env_FM2_VK_DIRECT_FETCH_KB = getenv("FM2_VK_DIRECT_FETCH_KB");
    static const char* env_FM2_VK_DRAWS = getenv("FM2_VK_DRAWS");
    static const char* env_FM2_VK_HDR_RESOLVES = getenv("FM2_VK_HDR_RESOLVES");
    static const char* env_FM2_VK_SYNC_PRESENT = getenv("FM2_VK_SYNC_PRESENT");
    g.drawsEntered++; // every draw the guest hands us, before any filtering
    std::unique_lock asyncLock(g_asyncMutex, std::defer_lock);
    if (UseAsyncPresent())
        asyncLock.lock();
    // Init before anything else, resolves included: bindless slot 0 is the white null texture and
    // TextureReserve hands out slots from the same counter, so a resolve that runs first takes
    // slot 0, destroys the null texture, and every later unbound sample reads an EDRAM copy.
    if (!Init())
        return;
    FrameGpuTimesBegin();
    // FM2_PERF: the renderer's own per-draw cost — the one part of a frame this backend owns.
    ScopedMs timed(g.perf ? &g.perfDrawsMs : nullptr);
    if (g.perf)
        g.perfDraws++;
    // Resolve packets (RB_MODECONTROL 6) copy EDRAM into memory: they are not geometry and must
    // not be drawn as triangles. They become a texture the title samples later, as on the D3D12 path.
    if ((gpureg::Get(0x2208 /* RB_MODECONTROL */) & 7) == 6)
    {
        g.resolveEpoch++;
        ScopedMs resolveTimed(g.perf ? &g.perfResolveMs : nullptr);
        g.perfResolves++;
        const uint32_t pitchReg = gpureg::Get(0x231A /* RB_COPY_DEST_PITCH */);
        const uint32_t dest = gpureg::Get(0x2319 /* RB_COPY_DEST_BASE */) & 0x1FFFFFFF;
        const uint32_t info = gpureg::Get(0x231B /* RB_COPY_DEST_INFO */);
        const uint32_t control = gpureg::Get(0x2318 /* RB_COPY_CONTROL */);
        const uint32_t sourceSelect = control & 7;
        const bool swapRedBlue = (info >> 24) & 1;

        // The resolve rectangle is three float2 vertices in vertex fetch constant 0.
        const uint32_t d0 = gpureg::Get(gpureg::FetchConstants), d1 = gpureg::Get(gpureg::FetchConstants + 1);
        const uint32_t vertices = GpuPhysToGuest(d0 & 0xFFFFFFFC);
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < 3; i++)
        {
            uint32_t xv = *GuestPtr<uint32_t>(vertices + i * 8), yv = *GuestPtr<uint32_t>(vertices + i * 8 + 4);
            if ((d1 & 3) == 2)
            {
                xv = __builtin_bswap32(xv);
                yv = __builtin_bswap32(yv);
            }
            float x, y;
            memcpy(&x, &xv, 4);
            memcpy(&y, &yv, 4);
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        const ScissorRect tile = WindowScissor();
        const ScissorRect rect{ std::clamp<int32_t>(std::lround(x0), tile.left, tile.right),
            std::clamp<int32_t>(std::lround(y0), tile.top, tile.bottom), std::clamp<int32_t>(std::lround(x1), tile.left, tile.right),
            std::clamp<int32_t>(std::lround(y1), tile.top, tile.bottom) };
        if (rect.right <= rect.left || rect.bottom <= rect.top)
            return;
        const uint32_t pitch = pitchReg & 0x3FFF;
        const uint32_t base = (dest - uint32_t(rect.top) * pitch * 4) & 0x1FFFFFFF;
        const uint32_t width = std::clamp<uint32_t>(pitch, 1, std::max<uint32_t>(kWidth, BigPassSize()));
        uint32_t height = std::clamp<uint32_t>(((pitchReg >> 16) & 0x3FFF) + uint32_t(rect.top), 1, 2048);
        vk::Surface* source = g.openSurface ? g.openSurface : g.lastDrawSurface;
        if (sourceSelect > 4 || !source)
            return;
        const bool depth = sourceSelect == 4;
        if (depth)
        {
            const uint32_t depthBase = gpureg::Get(0x2002 /* RB_DEPTH_INFO */) & 0xFFF;
            if (depthBase != source->depthBase)
                source = vk::SurfaceAcquire(g.context, g.surfaces, source->base, source->width, source->height, source->samples, depthBase);
            if (!source)
                return;
        }
        // The copy samples the surface, so its pass has to end first; the next draw opens it
        // again and SurfaceBegin brings it back to an attachment layout.
        auto finishResolve = [&] { PhaseMarkWrite(2, base, source->width, source->height); };
        ClosePass();
        PhaseMarkWrite(1, base, source->width, source->height);
        if (depth)
        {
            if (source->depthState->layout == VK_IMAGE_LAYOUT_UNDEFINED)
            {
                vk::SurfaceBegin(g.context, *source, g.cmd, nullptr);
                vk::SurfaceEnd(g.context, *source, g.cmd);
            }
            vk::SurfaceDepthShaderRead(g.context, *source, g.cmd);
        }
        else
            vk::SurfaceShaderRead(g.context, *source, g.cmd);
        static const bool hdrResolves = env_FM2_VK_HDR_RESOLVES == nullptr || atoi(env_FM2_VK_HDR_RESOLVES) != 0;
        const uint32_t destFormat = (info >> 7) & 0x3F;
        const VkFormat format = depth ? VK_FORMAT_R32_SFLOAT
            : hdrResolves && destFormat != 6 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const int bias = int32_t(info << 10) >> 26;
        const float scale = !depth ? std::ldexp(1.0f, bias) : 1.0f;
        State::ResolvedTexture& entry = g.resolved[base];
        if (entry.valid && rect.top > 0 && rect.bottom <= int32_t(entry.height) && entry.height < height)
            height = entry.height;
        // Host-size targets: the image is the guest size times this pass's render scale (the ratio HostScissor used to
        // place the draws in the surface), so a resolve is a 1:1 copy and what samples it - the next pass, and finally
        // the window - sees every pixel that was rendered. Guest-size targets box-filtered 1920x1080 down to 1280x720
        // and the window stretched it back up. FM2_VK_GUEST_SIZE_RESOLVES=1 restores that.
        static const bool guestSizeResolves = getenv("FM2_VK_GUEST_SIZE_RESOLVES") != nullptr;
        const double hostScaleX = double(source->width) / double(std::max(1u, PassWidth()));
        const double hostScaleY = double(source->height) / double(std::max(1u, PassHeight()));
        const uint32_t hostWidth = guestSizeResolves ? width : std::clamp<uint32_t>(uint32_t(std::lround(width * hostScaleX)), 1, 8192);
        const uint32_t hostHeight = guestSizeResolves ? height : std::clamp<uint32_t>(uint32_t(std::lround(height * hostScaleY)), 1, 8192);
        bool fresh = !entry.valid || entry.width != width || entry.height != height || entry.depth != depth || entry.format != format ||
            entry.hostWidth != hostWidth || entry.hostHeight != hostHeight;
        if (fresh)
        {
            auto swapVariant = [](State::ResolvedTexture& a, State::ResolvedTexture& b) {
                std::swap(a.index, b.index);
                std::swap(a.width, b.width);
                std::swap(a.height, b.height);
                std::swap(a.hostWidth, b.hostWidth);
                std::swap(a.hostHeight, b.hostHeight);
                std::swap(a.depth, b.depth);
                std::swap(a.format, b.format);
            };
            for (State::ResolvedTexture& spare : entry.spares)
                if (spare.index && spare.width == width && spare.height == height && spare.depth == depth && spare.format == format &&
                    spare.hostWidth == hostWidth && spare.hostHeight == hostHeight)
                {
                    swapVariant(entry, spare);
                    fresh = false;
                    break;
                }
            if (fresh)
            {
                if (entry.valid)
                {
                    if (entry.spares.size() >= 6)
                    {
                        g.retiredTextures.push_back(entry.spares.front().index);
                        entry.spares.erase(entry.spares.begin());
                    }
                    entry.spares.emplace_back();
                    swapVariant(entry, entry.spares.back());
                }
                g.perfResolveAllocations++;
                const uint32_t index = vk::TextureReserve(g.context, g.textures0, hostWidth, hostHeight, format);
                if (index == UINT32_MAX)
                {
                    finishResolve();
                    return;
                }
                entry.index = index;
                entry.width = width;
                entry.height = height;
                entry.hostWidth = hostWidth;
                entry.hostHeight = hostHeight;
                entry.valid = true;
                entry.depth = depth;
                entry.format = format;
                entry.swapped = false;
            }
        }
        vk::Image& target = g.textures0.slots[entry.index];
        const VkRect2D resolveRect{ { rect.left, rect.top }, { uint32_t(rect.right - rect.left), uint32_t(rect.bottom - rect.top) } };
        // The same rectangle in the host-size target: rounded as HostScissor rounds, so tiles meet without a seam.
        VkRect2D hostRect = resolveRect;
        if (!guestSizeResolves)
        {
            // Never empty: the guest rectangle is not, and a fresh target must be written once to leave UNDEFINED.
            const int32_t left = std::clamp<int32_t>(int32_t(std::lround(rect.left * hostScaleX)), 0, int32_t(hostWidth) - 1);
            const int32_t top = std::clamp<int32_t>(int32_t(std::lround(rect.top * hostScaleY)), 0, int32_t(hostHeight) - 1);
            const int32_t right = std::clamp<int32_t>(int32_t(std::lround(rect.right * hostScaleX)), left + 1, int32_t(hostWidth));
            const int32_t bottom = std::clamp<int32_t>(int32_t(std::lround(rect.bottom * hostScaleY)), top + 1, int32_t(hostHeight));
            hostRect = { { left, top }, { uint32_t(right - left), uint32_t(bottom - top) } };
        }
        vk::ResolveImage(g.context, g.resolve, g.cmd, depth ? source->depth : source->color, target, hostWidth, hostHeight,
            hostRect, false, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth, scale, format, nullptr,
            // Guest-size targets only: host texels per guest pixel. A host-size target is copied 1:1.
            guestSizeResolves ? float(hostScaleX) : 1.0f, guestSizeResolves ? float(hostScaleY) : 1.0f);
        entry.swapped = !depth && swapRedBlue;
        entry.guestBpp = depth ? 4
            : destFormat == 2 ? 1
            : (destFormat >= 3 && destFormat <= 5) || destFormat == 10 || destFormat == 15 || destFormat == 24 ? 2
            : destFormat == 26 || destFormat == 37 ? 8
            : destFormat == 38 ? 16 : 4;
        if (entry.frame != g.frame)
        {
            entry.memHash = ResolvedMemoryHash(base, entry);
            entry.memChecked = g.frame;
            entry.stale = false;
        }
        entry.frame = g.frame;
        if (!depth && (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R16G16B16A16_SFLOAT) && width == kWidth && height == kHeight)
            g.presentable[base] = entry;
        if (control & 0x300)
        {
            ScissorRect clearRect = rect;
            const uint32_t surfacePitch = gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF;
            if (surfacePitch && uint32_t(rect.right - rect.left) >= std::min<uint32_t>(surfacePitch, kWidth) && rect.left == 0)
                clearRect.bottom = int32_t(PassHeight());
            vk::SurfaceBegin(g.context, *source, g.cmd, nullptr);
            vk::DrawResetState(g.draw);
            VkClearAttachment attachments[2]{};
            uint32_t clearCount = 0;
            if ((control >> 8) & 1)
            {
                const uint32_t c = gpureg::Get(0x231E /* RB_COLOR_CLEAR */);
                VkClearAttachment& clear = attachments[clearCount++];
                clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                clear.colorAttachment = 0;
                clear.clearValue.color = { { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f,
                    ((c >> 24) & 0xFF) / 255.0f } };
            }
            if ((control >> 9) & 1)
            {
                const uint32_t v = gpureg::Get(0x231D /* RB_DEPTH_CLEAR */), d = v >> 8;
                float depth = float(d) / 16777215.0f;
                if ((gpureg::Get(0x2002 /* RB_DEPTH_INFO */) >> 16) & 1)
                {
                    const uint32_t e = d >> 20, m = d & 0xFFFFF;
                    depth = e ? std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15) : std::ldexp(float(m) / 1048576.0f, -14);
                    depth = std::clamp(depth, 0.0f, 1.0f);
                }
                VkClearAttachment& clear = attachments[clearCount++];
                clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                clear.clearValue.depthStencil = { depth, uint8_t(v) };
            }
            // Guest pixels to host pixels: the surface is larger than the pass at render resolutions above 1280x720, and
            // an unscaled rect left the right and bottom of it uncleared (stale colour and depth).
            const VkRect2D hostRect = HostScissor(clearRect, *source);
            const VkClearRect rectToClear{ hostRect, 0, 1 };
            vkCmdClearAttachments(g.cmd, clearCount, attachments, 1, &rectToClear);
            vk::SurfaceEnd(g.context, *source, g.cmd);
            g.passOpen = false;
            g.openSurface = nullptr;
        }
        finishResolve();
        return;
    }
    ScopedMs preludeTimed(g.perf ? &g.perfPreludeMs : nullptr);
    const uint32_t edramMode = gpureg::Get(0x2208 /* RB_MODECONTROL */) & 7;
    if (edramMode != 4 && edramMode != 5)
        return;
    // The device line goes to the log: SelectDevice prints the name to stderr only.
    {
        static bool once = false;
        if (!once)
        {
            once = true;
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(g.context.physical, &props);
            Log("vk: device \"%s\" api %u.%u.%u | maxImage2D %u | maxSamplerAniso %g", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
                props.limits.maxImageDimension2D, double(props.limits.maxSamplerAnisotropy));
        }
    }
    // FM2_VK_DRAWS=0 runs the seam and the shader service against the real game without drawing.
    static const bool drawEnabled = !env_FM2_VK_DRAWS || atoi(env_FM2_VK_DRAWS) != 0;
    if (!g.vsHash || !g.psHash)
        return;
    const uint64_t vsHash = g.vsHash, psHash = g.psHash;
    preludeTimed.Stop();
    const vk::VkShader* vs;
    const vk::VkShader* ps;
    {
        ScopedMs shaderTimed(g.perf ? &g.perfShaderMs : nullptr);
        vs = vk::ShaderGet(g.context, g.shaders, vsHash, g.vsCode.data(), uint32_t(g.vsCode.size()), false);
        ps = vk::ShaderGet(g.context, g.shaders, psHash, g.psCode.data(), uint32_t(g.psCode.size()), true);
    }
    if (!vs || !vs->ok || !ps || !ps->ok)
    {
        if (g.skippedShader++ < 4)
            Log("renderer_vk: shader not ready (vs %d/%s, ps %d/%s)", vs ? int(vs->ok) : -1, vs ? vs->error.c_str() : "?",
                ps ? int(ps->ok) : -1, ps ? ps->error.c_str() : "?");
        return;
    }
    if (!g.reportedConfig)
    {
        g.reportedConfig = true;
        Log("renderer_vk: config sync-present %s | direct-fetch %u KB", env_FM2_VK_SYNC_PRESENT ? "ON" : "off",
            env_FM2_VK_DIRECT_FETCH_KB ? uint32_t(atoi(env_FM2_VK_DIRECT_FETCH_KB)) : 16u);
    }
    if (!drawEnabled)
    {
        if (!g.reportedDraws)
        {
            g.reportedDraws = true;
            Log("renderer_vk: shaders compile and cache (%u compiled, %u failed); draws are disabled by FM2_VK_DRAWS=0",
                g.shaders.compiled, g.shaders.failed);
        }
        return;
    }

    // The surface the guest renders into: EDRAM base from RB_COLOR_INFO, sized like the D3D12
    // path sizes its surfaces — the render resolution from the settings, not the register's
    // pitch/height fields (those describe the tiled pass, not the host target).
    ScopedMs surfaceTimed(g.perf ? &g.perfSurfaceMs : nullptr);
    const uint32_t base = gpureg::Get(0x2001) & 0xFFF;
    const uint32_t depthBase = gpureg::Get(0x2002 /* RB_DEPTH_INFO */) & 0xFFF;
    uint32_t width = 0, height = 0;
    RenderSize(width, height);
    vk::Surface* surface = vk::SurfaceAcquire(g.context, g.surfaces, base, width, height, VK_SAMPLE_COUNT_1_BIT, depthBase);
    g.lastDrawSurface = surface;
    surfaceTimed.Stop();
    if (!surface)
    {
        if (g.skippedSurface++ < 4)
            Log("renderer_vk: no surface for base %u (%ux%u)", base, width, height);
        return;
    }

    ScopedMs setupTimed(g.perf ? &g.perfSetupMs : nullptr);
    const uint32_t prim = initiator & 0x3F, indexSource = (initiator >> 6) & 3, indexCount = initiator >> 16;
    const bool restartOn = (gpureg::Get(gpureg::PA_SU_SC_MODE_CNTL) >> 21) & 1;
    static LiveSwitch noDirectIndex("FM2_NO_DIRECT_INDEX");
    const bool directIndex = g.gphys && !noDirectIndex && (indexSource == 0 || indexSource == 2) &&
        (prim == 4 || prim == 13 || (prim == 6 && !restartOn));
    const uint32_t directVertices = prim == 4 ? indexCount / 3 * 3 : prim == 13 ? indexCount / 4 * 6 : (indexCount >= 3 ? (indexCount - 2) * 3 : 0);
    // Expanded index list: the vertex shader fetches it out of guest memory. Direct lists and
    // restart-free strips instead read the original indices, as the D3D12 backend does.
    thread_local std::vector<uint32_t> list;
    bool haveList = directIndex;
    if (!directIndex)
    {
        ScopedMs indexTimed(g.perf ? &g.perfIndexMs : nullptr);
        {
            // Timed on its own: the expansion runs between the record timer and DrawList.
            ScopedMs indicesTimed(g.perf ? &g.perfIndicesMs : nullptr);
            haveList = gpu::BuildIndexList(initiator, dmaBase, dmaSize, list, true);
        }
    }
    else
        list.clear();
    if ((!directIndex && (!haveList || list.empty())) || (directIndex && directVertices == 0))
    {
        if (g.skippedIndex++ < 4)
            Log("renderer_vk: no index list (initiator %08x prim %u source %u count %u dma %08x/%08x, %zu entries)",
                initiator, initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16, dmaBase, dmaSize, list.size());
        return;
    }

    const uint32_t primitive = initiator & 0x3F;
    vk::StateKey key = MakeKey(vsHash, psHash, primitive != 8 && primitive != 1, primitive);
    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        ScopedMs timed(g.perf ? &g.perfPipelineMs : nullptr);
        pipeline = PipelineFor(key, vs, ps, *surface);
    }
    if (!pipeline)
    {
        if (g.skippedPipeline++ < 4)
            Log("renderer_vk: no pipeline (blend %08x depth %08x mask %08x flags %u)", key.blend, key.depth, key.stencilMaskMode,
                key.flags);
        return;
    }
    gpu::VSConstants vc{};
    gpu::PSConstants pc{};
    setupTimed.Stop();
    ScopedMs preConstTimed(g.perf ? &g.perfPreConstMs : nullptr);
    preConstTimed.Stop();
    {
        ScopedMs constantsTimed(g.perf ? &g.perfConstantsMs : nullptr);
        BuildConstants(vs, ps, vc, pc);
        // Polygon offset (window-space depth), as main's D3D12 path: without it a decal z-fights the surface under it.
        {
            static const bool noPolyOffset = getenv("FM2_NO_POLY_OFFSET") != nullptr;
            const uint32_t sc = gpureg::Get(gpureg::PA_SU_SC_MODE_CNTL), polyPrim = initiator & 0x3F;
            const bool tri = polyPrim == 4 || polyPrim == 5 || polyPrim == 6;
            const bool enabled = tri ? (sc & (3u << 11)) != 0 : ((sc >> 13) & 1) != 0;
            float offset = 0;
            if (enabled && !noPolyOffset)
            {
                offset = (sc >> 11) & 1 ? RegFloat(0x2381) : RegFloat(0x2383);
                if ((gpureg::Get(0x2002) >> 16) & 1)
                    offset *= 0.5f;
            }
            vc.vpoly[0] = offset;
        }
    }
    ScopedMs postConstTimed(g.perf ? &g.perfPostConstMs : nullptr);
    if (directIndex)
    {
        const bool is32 = (initiator >> 11) & 1;
        vc.vte[1] = 1u | (prim == 6 ? 2u : 0u) | (prim == 13 ? 64u : 0u) | (is32 ? 4u : 0u) | (indexSource == 2 ? 8u : 0u) | ((dmaSize >> 30) << 4);
        vc.vte[2] = dmaBase & 0x1FFFFFFF & ~(is32 ? 3u : 1u);
        vc.vte[3] = gpureg::Get(gpureg::VGT_INDX_OFFSET);
    }
    // Position mode, from the initiator: 2 means point sprites. hlsl.cpp reads it as
    // "(vinfo.y & 2) != 0" to expand each point into a screen-space quad; with it always 0 every
    // point sprite collapsed to a single degenerate vertex.
    vc.info[1] = (initiator & 0x3F) == 1 ? 2 : 0;
    if ((initiator & 0x3F) == 8 && (gpureg::Get(0x2104 /* RB_COLOR_MASK */) & 0xF) == 0 &&
        (gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF) < 1000)
    {
        const uint32_t msaa = (gpureg::Get(0x2000 /* RB_SURFACE_INFO */) >> 16) & 3;
        if (msaa == 2)
            vc.vscale[3] = vc.voffset[3] = 2.0f;
        else if (msaa == 1)
        {
            vc.vscale[3] = 1.0f;
            vc.voffset[3] = 2.0f;
        }
    }

    if (g.openSurface != surface || g.openDepthBase != depthBase)
    {
        ClosePass();
        // Clear on the first draw of each frame, and preserve within the frame so that reopening the pass after a
        // resolve keeps its content.
        VkClearColorValue clear{};
        const bool firstDraw = surface->drawnFrame != g.frame;
        surface->drawnFrame = g.frame;
        vk::SurfaceBegin(g.context, *surface, g.cmd, firstDraw ? &clear : nullptr);
        g.passOpen = true;
        g.openSurface = surface;
        g.openDepthBase = depthBase;
    }
    g.drawsThisFrame++; // this draw's ordinal in the frame
    {
        // Which viewport the frame is actually drawn with (the dump reports the top ones).
        const uint32_t scaleX = uint32_t(int32_t(vc.vscale[0])), scaleY = uint32_t(int32_t(vc.vscale[1]));
        g.viewports[(uint64_t(scaleX) << 32) | uint64_t(scaleY)]++;
        g.viewportTarget = vc.info[2];
    }
    ScissorRect clip = WindowScissor();
    float shiftY = 0.0f;
    {
        // Tiled draws stay in screen space. A full-pass rectangle that follows them is
        // tile-relative, so shift its viewport by the tracked tile origin and clip it to that tile.
        const uint32_t offsetReg = gpureg::Get(0x2080 /* PA_SC_WINDOW_OFFSET */);
        const int32_t wy = ((gpureg::Get(gpureg::PA_SU_SC_MODE_CNTL) >> 16) & 1) ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
        const bool full = clip.left <= 0 && clip.top <= 0 && clip.right >= int32_t(PassWidth()) && clip.bottom >= int32_t(PassHeight());
        if (!full)
        {
            g_tile.originY = -wy;
            g_tile.rect = clip;
            g_tile.surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
        }
        else if (g_tile.originY != 0 && (g_tile.surface == ~0u || g_tile.surface == gpureg::Get(0x2000)))
        {
            shiftY = float(g_tile.originY + wy);
            clip.left = std::max(clip.left, g_tile.rect.left);
            clip.top = std::max(clip.top, g_tile.rect.top);
            clip.right = std::min(clip.right, g_tile.rect.right);
            clip.bottom = std::min(clip.bottom, g_tile.rect.bottom);
        }
    }
    VkRect2D scissor = HostScissor(clip, *surface);
    const float viewportY = float(surface->height) + shiftY * float(surface->height) / float(PassHeight());
    VkViewport viewport{ 0, viewportY, float(surface->width), -float(surface->height), 0, 1 };
    vk::DrawSetViewport(g.context, g.draw, g.cmd, viewport);
    ScopedMs dynamicTimed(g.perf ? &g.perfDynamicMs : nullptr);
    vk::DrawSetStencilReference(g.context, g.draw, g.cmd, gpureg::Get(0x210D /* RB_STENCILREFMASK */) & 0xFF);
    const float blendFactor[4] = { RegFloat(0x2105), RegFloat(0x2106), RegFloat(0x2107), RegFloat(0x2108) };
    vk::DrawSetBlendConstants(g.context, g.draw, g.cmd, blendFactor);
    dynamicTimed.Stop();
    const uint32_t drawCount = directIndex ? directVertices : uint32_t(list.size());
    bool recorded;
    {
        // The record span has its own bucket as well as being part of the draw cost.
        postConstTimed.Stop();
        ScopedMs recordTimed(g.perf ? &g.perfRecordMs : nullptr);
        // The draw's shader identity, for FM2_VK_DEBUG_PS and FM2_VK_DEBUG_VS.
        g.draw.psHashDebug = psHash;
        g.draw.vsHashDebug = vsHash;
        recorded = vk::DrawList(g.context, g.draw, g.cmd, pipeline, g.textures0.set, vc, pc, list.data(), drawCount, scissor,
            &vs->vertexFetchConstants);
    }
    if (recorded && g_phasePerDraw && g_phaseStarted)
        PhaseMarkWrite(4, base, surface->width, surface->height, psHash, drawCount);
    if (!recorded)
    {
        if (g.skippedDraw++ < 4)
            Log("renderer_vk: DrawList refused (%zu vertices, draw %u, constants %llu/%llu, guest %llu/%llu, vte %08x)",
                list.size(), drawCount, (unsigned long long)g.draw.constants.used, (unsigned long long)g.draw.constants.size,
                (unsigned long long)g.draw.guestMemory.used, (unsigned long long)g.draw.guestMemory.size, vc.vte[1]);
        return;
    }
}

// Draws handed to vkCmdDraw* since the previous summary line. A per-frame reset read zero because the summary runs
// before the frame's draw phase; a delta against our own last sample has no such ordering hazard.
uint64_t SubmittedSinceLastSummary()
{
    static uint64_t previous = 0;
    const uint64_t now = vk::g_cmdDrawCount;
    const uint64_t delta = now - previous;
    previous = now;
    return delta;
}

void RendererVkOnSwap(uint64_t frame, uint32_t frontBuffer)
{
    const auto swapNow = std::chrono::steady_clock::now();
    static auto previousSwap = swapNow;
    const double swapMs = std::chrono::duration<double, std::milli>(swapNow - previousSwap).count();
    previousSwap = swapNow;
    static const double slowMs = getenv("FM2_SLOW_MS") ? atof(getenv("FM2_SLOW_MS")) : 25.0;
    static double lastDrawMs = 0, lastShaderMs = 0, lastPipelineMs = 0, lastTextureMs = 0, lastResolveMs = 0, lastWaitMs = 0;
    static uint64_t lastUploadSubmits = 0, lastUploadBytes = 0, lastUploadFlushes = 0, lastImageCreates = 0;
    static double lastUploadRecordMs = 0, lastImageCreateMs = 0, lastSlotWriteMs = 0;
    static uint64_t lastSlotWrites = 0;
    if (swapMs >= slowMs)
        Log("slow_vk: frame %llu, %.2f ms | draw %.2f shader %.2f pipeline %.2f textures %.2f resolve %.2f wait %.2f | compiled %u pipelines %zu | uploads %llu (%llu KB, %llu submits, record %.2f ms) images %llu (%.2f ms) slots %llu (%.2f ms)",
            (unsigned long long)frame, swapMs, std::max(0.0, g.perfDrawsMs - lastDrawMs), std::max(0.0, g.perfShaderMs - lastShaderMs),
            std::max(0.0, g.perfPipelineMs - lastPipelineMs), std::max(0.0, g.perfTexMs - lastTextureMs),
            std::max(0.0, g.perfResolveMs - lastResolveMs), std::max(0.0, g.perfWaitMs - lastWaitMs), g.shaders.compiled,
            g.pipelines.size(), (unsigned long long)(vk::g_uploadSubmits - lastUploadSubmits),
            (unsigned long long)((vk::g_uploadBytes - lastUploadBytes) / 1024),
            (unsigned long long)(vk::g_uploadFlushes - lastUploadFlushes), vk::g_uploadRecordMs - lastUploadRecordMs,
            (unsigned long long)(vk::g_imageCreates - lastImageCreates), vk::g_imageCreateMs - lastImageCreateMs,
            (unsigned long long)(vk::g_slotWrites - lastSlotWrites), vk::g_slotWriteMs - lastSlotWriteMs);
    lastSlotWrites = vk::g_slotWrites;
    lastSlotWriteMs = vk::g_slotWriteMs;
    lastUploadRecordMs = vk::g_uploadRecordMs;
    lastImageCreates = vk::g_imageCreates;
    lastImageCreateMs = vk::g_imageCreateMs;
    lastUploadFlushes = vk::g_uploadFlushes;
    lastUploadSubmits = vk::g_uploadSubmits;
    lastUploadBytes = vk::g_uploadBytes;
    lastDrawMs = g.perfDrawsMs;
    lastShaderMs = g.perfShaderMs;
    lastPipelineMs = g.perfPipelineMs;
    lastTextureMs = g.perfTexMs;
    lastResolveMs = g.perfResolveMs;
    lastWaitMs = g.perfWaitMs;
    std::unique_lock asyncLock(g_asyncMutex, std::defer_lock);
    if (UseAsyncPresent())
        asyncLock.lock();
    DebugOnSwap(frame);
    if (!Init())
        return;
    const uint32_t draws = g.drawsThisFrame;
    // g.drawsThisFrame is zeroed at the end of this function, so this is the frame's count.
    vk::Surface* last = g.lastDrawSurface;
    const auto resolved = g.presentable.find(frontBuffer & 0x1FFFFFFF);
    const State::ResolvedTexture* front = resolved != g.presentable.end() && resolved->second.valid && resolved->second.width == 1280 &&
            resolved->second.height == 720
        ? &resolved->second
        : nullptr;
    {
        ScopedMs timed(g.perf ? &g.perfSubmitMs : nullptr);
        ClosePass();
        // Submit the frame and start the next one. The command buffer is reused: waiting keeps it
        // simple, and the first frames are not where the time goes. It is still a whole frame of
        // GPU work serialised against the CPU, which is what FM2_PERF reports here.
        if (UseAsyncPresent())
            SubmitAsyncFrame(front);
        else
            EndFrame();
    }
    FrameGpuTimesEnd();
    // FM2_VK_DUMP_AT=<seconds>: dump once, at that point in time, so a Vulkan frame can be
    // compared with a D3D12 one taken at the same moment (FM2_DUMP_AT).
    static const double dumpAt = getenv("FM2_VK_DUMP_AT") ? atof(getenv("FM2_VK_DUMP_AT")) : -1.0;
    static const auto started = std::chrono::steady_clock::now();
    static bool dumpedOnce = false;
    const bool timeDump = dumpAt >= 0 && !dumpedOnce &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= dumpAt;
    if (timeDump)
    {
        Log("present_vk: front buffer %08x, resolved %08x format %u, last surface %03x", frontBuffer,
            resolved != g.presentable.end() ? resolved->first : 0, front ? front->format : 0, last ? last->base : ~0u);
        for (const auto& [base, texture] : g.presentable)
            Log("present_vk: candidate %08x format %u frame %llu", base, texture.format, (unsigned long long)texture.frame);
    }
    // Windowed: hand the frame to the presenter (which still uploads and blits it on the D3D12
    // side — a real Vulkan swap chain is the platform layer's job, this is for looking at it now).
    static const bool windowed = getenv("FM2_HEADLESS") == nullptr;
    if (windowed && !UseAsyncPresent() && (front || last) && draws)
    {
        ScopedMs timed(g.perf ? &g.perfPresentMs : nullptr);
        std::vector<uint8_t> rgba;
        const bool read = front ? ReadResolved(*front, rgba) : ReadSurface(*last, rgba);
        uint32_t imageWidth = last ? last->width : 0, imageHeight = last ? last->height : 0;
        if (front)
            ResolvedSize(*front, imageWidth, imageHeight);
        if (read && imageWidth && rgba.size() == size_t(imageWidth) * imageHeight * 4)
        {
            ScopedMs handoffTimed(g.perf ? &g.perfPresentHandoffMs : nullptr);
            RendererOverlayImage(imageWidth, imageHeight, rgba);
            PresenterSubmitImage(imageWidth, imageHeight, std::move(rgba));
        }
    }
    // FM2_VK_DUMP_FRAME=<n>: dump that exact frame. Comparing two builds needs the same frame,
    // not the same wall-clock moment — the two land in different scenes otherwise.
    static const uint64_t dumpFrame = getenv("FM2_VK_DUMP_FRAME") ? strtoull(getenv("FM2_VK_DUMP_FRAME"), nullptr, 10) : 0;
    // A capture asked for by the tour (out/capture.request) dumps this frame, so the two backends
    // can be put on the same screen instead of the same frame counter — the menus advance on their
    // own timers and frame numbers do not line up between them.
    static bool captureFileSeen = false;
    // Looked for ten times a second, not every frame: opening a file that is not there was 5% of the render thread.
    static auto lastCaptureCheck = std::chrono::steady_clock::time_point{};
    const bool checkCaptureFile = !captureFileSeen && swapNow - lastCaptureCheck >= std::chrono::milliseconds(100);
    if (checkCaptureFile)
        lastCaptureCheck = swapNow;
    FILE* cf = checkCaptureFile ? std::fopen("out/capture.request", "rb") : nullptr;
    if (cf)
    {
        std::fclose(cf);
        std::remove("out/capture.request");
        captureFileSeen = true;
    }
    // "At or after" rather than "exactly": the chosen frame may have no resolved surface, and then nothing would
    // be written.
    static bool dumpFrameDone = false;
    const bool frameDump = (dumpFrame && !dumpFrameDone && frame >= dumpFrame) || captureFileSeen;
    if (frameDump && dumpFrame) dumpFrameDone = true;
    captureFileSeen = false;
    if (frameDump || timeDump)
        Log("renderer_vk: capture frame %llu, front buffer %08x, source %s", (unsigned long long)frame, frontBuffer,
            front ? "resolved" : "surface");
    if ((front || last) && (frameDump || timeDump) && draws)
    {
        dumpedOnce |= timeDump;
        if (last)
            EndFrame();
        {
            std::vector<std::pair<uint64_t, uint64_t>> top(g.viewports.begin(), g.viewports.end());
            std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < std::min<size_t>(top.size(), 3); i++)
                Log("renderer_vk: dumped frame %llu: viewport scale %d %d, %u draws (target %u)",
                    (unsigned long long)frame, int32_t(top[i].first >> 32), int32_t(top[i].first), top[i].second, g.viewportTarget);
        }
        if (front)
            DumpResolved(*front);
        else
            DumpSurface(*last);
        if ((timeDump || frameDump) && UseAsyncPresent())
        {
            AsyncFrame& presented = g_async[g_asyncIndex ^ 1];
            if (presented.hasImage)
            {
                const uint8_t* pixels = static_cast<const uint8_t*>(presented.readback);
                std::vector<uint8_t> rgba(pixels, pixels + size_t(presented.width) * presented.height * 4);
                size_t lit = 0;
                for (size_t i = 0; i < rgba.size(); i += 4)
                    lit += (rgba[i] > 5u) + (rgba[i + 1] > 5u) + (rgba[i + 2] > 5u);
                DumpPixels(presented.width, presented.height, rgba, lit, "vk_present");
            }
        }
        if (frameDump && !dumpFrame)
            RendererMarkCaptureComplete();
    }
    std::string shotPath;
    if ((front || last) && draws && DebugShotPending(shotPath))
    {
        std::vector<uint8_t> rgba;
        if (front ? ReadResolved(*front, rgba) : ReadSurface(*last, rgba))
        {
            uint32_t shotWidth = last ? last->width : 0, shotHeight = last ? last->height : 0;
            if (front)
                ResolvedSize(*front, shotWidth, shotHeight);
            WritePng(shotPath, shotWidth, shotHeight, rgba);
            DebugShotDone(shotPath);
            Log("renderer_vk: wrote %s", shotPath.c_str());
        }
    }
    // FM2_PERF: every 60 frames, where the time went. This backend records a whole frame into one
    // command buffer and waits for it at the swap, so a frame is four things: the renderer's own
    // per-draw work, the submit with its wait, the present readback (windowed only), and the rest
    // of the game. Only the first three belong to this backend, so only those are worth fixing.
    if (g.perf && ++g.perfFrames >= 60)
    {
        const auto now = std::chrono::steady_clock::now();
        const double frames = double(g.perfFrames);
        const double period = std::chrono::duration<double, std::milli>(now - g.perfWindow).count() / frames;
        const double own = (g.perfDrawsMs + g.perfSubmitMs + g.perfPresentMs) / frames;
        Log("perf_vk: %.1f fps, %.2f ms/frame = %.2f draws (%u/frame) + %.2f submit/wait + %.2f present + %.2f elsewhere",
            1000.0 / period, period, g.perfDrawsMs / frames, uint32_t(double(g.perfDraws) / frames), g.perfSubmitMs / frames,
            g.perfPresentMs / frames, period - own);
        // Phase warning, next to the figure it qualifies: a race runs 1400-2200 draws per frame, so a menu-scale
        // count means this fps describes the menus.
        {
            const uint32_t perFrame = uint32_t(double(g.perfDraws) / frames);
            if (perFrame < 600)
                Log("perf_vk:   MENU-SCALE (%u draws/frame) - not a race; do not quote this fps as race performance",
                    perFrame);
        }
        // An idle frame spends most of its wall time outside draws/submit/present - waiting on the guest, not
        // rendering - and its fps is not comparable with a busy frame's.
        {
            const double elsewhere = period - own;
            if (elsewhere > 0.5 * period)
                Log("perf_vk: IDLE %.0f%% of the frame is outside draws/submit/present - this fps reflects waiting, not "
                    "rendering; do not compare it with a busy-frame number", 100.0 * elsewhere / period);
        }
        Log("perf_vk:   draws: setup %.2f | bank %.2f | textures %.2f (of which hash %.2f) | surface %.2f | pipeline %.2f | tail %.2f | record %.2f (indices %.2f, indexed %.2f) | other %.2f | preconst %.2f | postconst %.2f ms/frame",
            g.perfSetupMs / frames, g.perfBankMs / frames, g.perfTexMs / frames, g.perfHashMs / frames, g.perfSurfaceMs / frames,
            g.perfPipelineMs / frames, g.perfTailMs / frames, g.perfRecordMs / frames, g.perfIndicesMs / frames, g.perfIndexedMs / frames,
            // Only top-level spans may be subtracted: perfBankMs/perfTexMs are inside perfConstantsMs, and
            // perfSurfaceMs/perfPipelineMs are inside perfPostConstMs.
            (g.perfDrawsMs - g.perfPreludeMs - g.perfDynamicMs - g.perfSetupMs - g.perfConstantsMs - g.perfPostConstMs -
                g.perfRecordMs) / frames,
            g.perfPreConstMs / frames, g.perfPostConstMs / frames);
        {
            // Closure check: the record span beside the sum of every child that is supposed to explain it.
            const double children = g.perfPreFetchMs + g.perfFetchLoopMs + g.perfVertexMs + g.perfConstantCopyMs
                + g.perfDescriptorMs + g.perfDrawCallMs + g.perfIndicesMs + g.perfIndexedMs;
            // The DrawList marks and the two entry-point timers are different instruments: name them separately.
            const double marks = children - g.perfIndicesMs - g.perfIndexedMs;
            Log("perf_vk:   closure: record %.2f | marks %.2f | indices %.2f | indexed %.2f | unaccounted %.2f (%.0f%%)",
                g.perfRecordMs / frames, marks / frames, g.perfIndicesMs / frames, g.perfIndexedMs / frames,
                (g.perfRecordMs - children) / frames,
                100.0 * (g.perfRecordMs - children) / (g.perfRecordMs > 0 ? g.perfRecordMs : 1));
        }
        {
            // Closure for the whole draw bucket, top-level spans only (bank/tex live inside constants, surface/pipeline
            // inside postconst). The gap is zero by algebra, so a non-zero value means a bucket was added or removed
            // without updating the residual.
            const double parts = g.perfPreludeMs + g.perfDynamicMs + g.perfSetupMs + g.perfConstantsMs
                + g.perfPostConstMs + g.perfRecordMs;
            // The residual printed on the bucket line is draws - (top-level parts), so this gap equals `other`.
            const double gap = (g.perfDrawsMs - parts) / frames;
            const double other = g.perfDrawsMs / frames - (g.perfPreludeMs + g.perfDynamicMs + g.perfSetupMs
                + g.perfConstantsMs + g.perfPostConstMs + g.perfRecordMs) / frames;
            Log("perf_vk:   closure2: draws %.2f | parts %.2f | gap %.2f (= other %.2f) %s",
                g.perfDrawsMs / frames, parts / frames, gap, other,
                std::abs(gap - other) > 0.01 ? "MISMATCH - a bucket was added or double-counted" : "ok");
        }
        Log("perf_vk:   prelude %.2f | dynamic state %.2f ms/frame", g.perfPreludeMs / frames, g.perfDynamicMs / frames);
        const bool async = UseAsyncPresent();
        const double vertexMs = async ? g.perfVertexMs : g.draw.perfVertexMs;
        const double constantCopyMs = async ? g.perfConstantCopyMs : g.draw.perfConstantsMs;
        const double descriptorMs = async ? g.perfDescriptorMs : g.draw.perfBindMs;
        const double drawCallMs = async ? g.perfDrawCallMs : g.draw.perfCallMs;
        const double preFetchMs = async ? g.perfPreFetchMs : g.draw.perfPreFetchMs;
        const double fetchLoopMs = async ? g.perfFetchLoopMs : g.draw.perfFetchLoopMs;
        Log("perf_vk:   record split: pre-fetch %.2f | fetch-loop %.2f | vertices %.2f | copies %.2f | descriptors %.2f | draw calls %.2f ms/frame",
            preFetchMs / frames, fetchLoopMs / frames,
            vertexMs / frames, constantCopyMs / frames, descriptorMs / frames, drawCallMs / frames);
        Log("perf_vk:   present: resolve+copy %.2f | convert %.2f | handoff %.2f ms/frame", g.perfPresentResolveMs / frames,
            g.perfPresentConvertMs / frames, g.perfPresentHandoffMs / frames);
        if (async)
            Log("perf_vk:   async: end %.2f | queue %.2f | fence wait %.2f ms/frame", g.perfEndMs / frames, g.perfQueueMs / frames,
                g.perfWaitMs / frames);
        else
            // Only perfWaitMs is measured on the synchronous path: perfEndMs/perfQueueMs are filled by the async submit.
            Log("perf_vk:   sync: waitIdle stall %.2f ms/frame (end/queue timers are async-only)", g.perfWaitMs / frames);
        Log("perf_vk:   %.1f texture binds/frame, %.1f hashed/frame", g.perfTexCalls / frames, double(g.perfHashCalls) / frames);
        Log("perf_vk:   bind body %.2f ms/frame = cube %.1f | resolved %.1f | refused %.1f | 2D %.1f per frame",
            g.perfBindMs / frames, double(g.perfBindCube) / frames, double(g.perfBindResolved) / frames,
            double(g.perfBindRefused) / frames, double(g.perfBind2D) / frames);
        Log("perf_vk:   bind cache reused %.1f per frame", double(g.bindReused) / frames);
        Log("perf_vk:   resolves %.2f ms (%llu, %llu allocated) | indices %.2f ms | constants %.2f ms",
            g.perfResolveMs / frames, (unsigned long long)g.perfResolves, (unsigned long long)g.perfResolveAllocations,
            g.perfIndexMs / frames, g.perfConstantsMs / frames);
        Log("perf_vk:   shaders %.2f ms/frame", g.perfShaderMs / frames);
        if (g.perfFrameGpuSamples)
            Log("perf_vk:   GPU frame %.3f ms (%u samples)", g.perfFrameGpuMs / g.perfFrameGpuSamples, g.perfFrameGpuSamples);
        g.perfFrameGpuMs = 0;
        g.perfFrameGpuSamples = 0;
        g.perfDrawsMs = g.perfSubmitMs = g.perfPresentMs = 0;
        g.perfEndMs = g.perfQueueMs = g.perfWaitMs = 0;
        g.perfBankMs = g.perfTexMs = g.perfPipelineMs = g.perfRecordMs = g.perfSetupMs = 0; g.perfIndicesMs = 0; g.perfIndexedMs = 0;
        g.perfSurfaceMs = g.perfHashMs = 0;
        g.perfPreFetchMs = g.perfFetchLoopMs = g.perfVertexMs = g.perfConstantCopyMs = g.perfDescriptorMs = g.perfDrawCallMs = 0;
        g.perfResolveMs = g.perfIndexMs = g.perfConstantsMs = g.perfShaderMs = g.perfTailMs = g.perfSurfaceMs = 0;
        g.perfPreConstMs = g.perfPostConstMs = 0;
        g.perfPreludeMs = g.perfDynamicMs = 0;
        g.perfResolves = g.perfResolveAllocations = 0;
        g.perfHashMs = 0;
        g.perfPresentResolveMs = g.perfPresentConvertMs = g.perfPresentHandoffMs = 0;
        g.perfBindMs = 0;
        g.perfTexCalls = g.perfHashCalls = 0;
        g.perfBindCube = g.perfBindResolved = g.perfBindRefused = g.perfBind2D = 0;
        g.bindReused = 0;
        g.perfFrames = 0;
        g.perfDraws = 0;
        g.perfWindow = now;
    }
    g.frame = frame;
    if (g.perf && frame % 60 == 0)
    {
        const vk::DrawContext& frameDraw = UseAsyncPresent() ? g_asyncOther : g.draw;
        Log("renderer_vk: frame %llu, %u recorded, %llu submitted, %u shaders compiled, %zu surfaces, %u pipelines, textures bound %u "
            "(%u re-uploaded), refused %u of %u slots, entered %llu, rejects %u [draw %u shader %u surface %u index %u pipeline %u], overflows %llu/%llu, rings %llu/%llu KB",
            (unsigned long long)frame, draws, SubmittedSinceLastSummary(), g.shaders.compiled, g.surfaces.surfaces.size(), unsigned(g.pipelines.size()),
            g.texBound, g.texDecoded, g.texFailed, g.texSlots,
            // "rejects" is the sum of every drop reason, not just skippedDraw.
            (unsigned long long)g.drawsEntered,
            g.skippedDraw + g.skippedShader + g.skippedSurface + g.skippedIndex + g.skippedPipeline,
            g.skippedDraw, g.skippedShader, g.skippedSurface, g.skippedIndex, g.skippedPipeline,
            (unsigned long long)frameDraw.constantOverflows, (unsigned long long)frameDraw.guestOverflows,
            (unsigned long long)(frameDraw.constants.used / 1024), (unsigned long long)(frameDraw.guestMemory.used / 1024));
    }
    // Reclaim displaced variants after this frame's GPU work and readbacks have finished.
    for (auto& [base, texture] : g.presentable)
        for (uint32_t index : g.retiredTextures)
            if (texture.index == index)
            {
                const auto current = g.resolved.find(base);
                if (current != g.resolved.end() && current->second.valid && current->second.width == texture.width &&
                    current->second.height == texture.height)
                    texture = current->second;
                else
                    texture.valid = false;
            }
    for (uint32_t index : g.retiredTextures)
        vk::TextureFree(g.context, g.textures0, index);
    g.retiredTextures.clear();
    if (!UseAsyncPresent())
        vk::DrawBeginFrame(g.draw);
    g.drawsThisFrame = 0;
    g.drawsEntered = 0;
    g_tile = TileState{};
    g.texBound = g.texFailed = g.texSlots = g.texDecoded = 0;
    g.viewports.clear();
}
