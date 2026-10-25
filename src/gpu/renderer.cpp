// Native D3D12 renderer driven by the command stream (phase 2, first cut).
//
// Runs on the command processor thread. For each draw it translates the
// active Xenos shaders to HLSL (cached), uploads the referenced vertex data
// and an expanded index list into a per-frame upload ring, decodes the
// bound textures (cached by content), and draws into one 1280x720 colour
// target with depth. Resolves and other non-colour EDRAM passes are
// skipped; render-target configuration (EDRAM surfaces, MSAA, tiles) comes
// next.

#include <algorithm>
#include <bit>
#include <immintrin.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <shared_mutex>
#include <regex>
#include <unordered_set>
#include <unordered_map>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include "gpu/hlsl.h"
#include "gpu/dlss.h"
#include "gpu/fsr3.h"
#include "gpu/recorded_list.h"
#include "fsr1_cs.h"
#include <DirectXPackedVector.h>
#include "live_switch.h"
#include "settings.h"
#include "gpu/regs.h"
#include "gpu/texture.h"
#include "gpu/ucode.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "debug.h"
#include "log.h"
#include "png.h"
#include "render/presenter.h"
#include "gpu/renderer.h"
#include "gpu/shader_scan.h"
#include "gpu/boot_screen.h"
#include "gpu/static_shaders.h"

using Microsoft::WRL::ComPtr;


void LogAllHostThreadStacks(const char* why); // hang.cpp
extern std::atomic<bool> g_cameraAspectWidened; // hooks/camera_aspect.cpp
void DealerCarDraw(uint64_t vs, const float c[256][4], float squash[4][4], bool body); // hooks/dealer.cpp: the easter egg, per car draw

namespace
{
constexpr UINT kWidth = 1280, kHeight = 720;
// Output size (RenderSize: FM2_RESOLUTION or FM2_RENDER_SCALE). Everything else
// (registers, scissors, resolve and clear rectangles, shader constants) stays in the game's
// own pixels; only the D3D calls scale. Vertically that is output height / 720. A pass at
// the game's full 1280-pixel width (surface pitch 1280: the main view and its full-screen
// post-processing) scales horizontally to the full output width, wider than 16:9 on an
// ultrawide; every other pass (reflection faces, shadow maps, reduced-size post passes)
// scales uniformly, so square targets stay square. Draws, clears and resolves of a pass
// all read the same pitch, so they agree.
uint32_t OutputWidth()
{
    static const uint32_t w = [] { uint32_t w, h; RenderSize(w, h); return w; }();
    return w;
}
uint32_t OutputHeight()
{
    static const uint32_t h = [] { uint32_t w, h; RenderSize(w, h); return h; }();
    return h;
}
// Upscaling: the 3D scene of a frame renders at this fraction of the output (1 otherwise);
// after the scene is upscaled (UpscaleScene), the rest of the frame renders at full size.
float g_sceneScale = 1.0f;
// The scene of this frame is still being drawn and is upscaled when it is done (any
// upscaler; with a temporal one, also at native scale for anti-aliasing).
bool g_scenePhase = false;
// A temporal upscaler this frame: the scene's main view draws with motion vectors and jitter.
bool g_temporal = false;
struct Upscale
{
    bool tried = false, ready = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> easu, rcas, depthCopy, gamma, enhance;
    ComPtr<ID3D12Resource> dlaaOut; // DLAA output (scene size), copied back into the scene image
    ComPtr<ID3D12Resource> depth; // temporal: the scene's depth as R32 (render size, top left)
    ComPtr<ID3D12Resource> temp; // EASU output (RGBA16F)
    ComPtr<ID3D12Resource> presentOut; // FSR 1 on the whole frame: what is shown (the title's image stays as it is)
    uint32_t tempWidth = 0, tempHeight = 0;
    bool switchedThisFrame = false;
    // The scene image whose last tile was resolved: upscaled at the next draw. FM2 resolves each
    // tile twice (the HDR image, then a copy for bloom); upscaling at the first resolve switched
    // to full size before the second, which then read the small scene at full-size coordinates
    // (the box of wrong picture over the car).
    uint32_t pendingBase = 0;
    bool pending = false;
    uint64_t retryFrame = 0; // after a frame with no scene to upscale (menus): native until then
    // Temporal upscalers: the motion target (RG16F, output size; UV offsets to last frame),
    // this frame's jitter (pixels and NDC) and the scene's render size.
    ComPtr<ID3D12Resource> motion;
    ComPtr<ID3D12DescriptorHeap> motionRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE motionRtv{};
    float jitterPx[2] = {}, jitterNdc[2] = {};
    uint32_t renderWidth = 0, renderHeight = 0;
    bool reset = true; // no usable history (first frame, after menus)
    // Each draw's vertex constants, kept for the same draw in the next frame (keyed by
    // shaders, vertex and index data and occurrence).
    struct DrawConstants
    {
        uint32_t frame = 0;                    // the temporal frame that last drew it
        D3D12_GPU_VIRTUAL_ADDRESS address[2]{}; // its vertex constants in that frame's ring, by frame parity
    };
    std::unordered_map<uint64_t, DrawConstants> constants;
    std::unordered_map<uint64_t, uint32_t> occurrences;
    uint32_t constantsFrame = 0;
} g_upscale;

// A pass taller than the screen (the paint editor's 1024x1024 livery target) is scaled as a whole
// by kHeight / its height so it fits the surface, and the vertex shader divides by its height.
// It is found by its window scissor (WindowScissor) and recognised by its surface info.
uint32_t g_tallInfo = 0, g_tallHeight = 0, g_tallKey = ~0u;
std::atomic<bool> g_tallSeen{ false }; // FM2_CAPTURE_TALL: a tall pass was drawn
// A surface wider than the screen (the paint editor's 2048x2048 livery composite, drawn in 256-row
// tiles) is a square pass of its pitch less the tile padding (2080 -> 2048, 1040 -> 1024).
// The MSAA layout each EDRAM surface is drawn in (its last draw that is not a texture-less
// rectangle), by colour base and row width in samples: views of the same EDRAM with another
// MSAA mode (clears) are mapped onto it. Samples per pixel: 1x 1x1, 2x 1x2, 4x 2x2.
std::atomic<uint32_t> g_nativeMsaa[0x1000]; // key << 2 | MSAA mode
uint32_t MsaaAcross(uint32_t msaa) { return msaa == 2 ? 2u : 1u; }
uint32_t MsaaDown(uint32_t msaa) { return msaa ? 2u : 1u; }
uint32_t LayoutKey(uint32_t surface)
{
    return (gpureg::Get(0x2001 /* RB_COLOR_INFO */) & 0xFFF) | (surface & 0x3FFF) * MsaaAcross((surface >> 16) & 3) << 12;
}
std::atomic<uint32_t>& NativeSlot(uint32_t key) { return g_nativeMsaa[(key ^ key >> 12) & 0xFFF]; }
std::atomic<uint32_t> g_nativeMsaaGeneration{ 0 }; // bumped when an entry changes (BigPassSize's memo)
void SetNativeMsaa(uint32_t key, uint32_t msaa)
{
    if (NativeSlot(key).exchange(key << 2 | msaa, std::memory_order_relaxed) != (key << 2 | msaa))
        g_nativeMsaaGeneration.fetch_add(1, std::memory_order_relaxed);
}
// The surface's MSAA layout when the current view is another one of an MSAA surface, else -1.
int ForeignNativeMsaa()
{
    static LiveSwitch off("FM2_NO_FOREIGN_MSAA"); // A/B
    if (off)
        return -1;
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */), key = LayoutKey(surface);
    const uint32_t native = NativeSlot(key).load(std::memory_order_relaxed);
    return native >> 2 == key && (native & 3) != ((surface >> 16) & 3) && (native & 3) != 0 ? int(native & 3) : -1;
}

uint32_t BigPassSizeUncached()
{
    uint32_t pitch = gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF;
    // A view of a big MSAA surface in another MSAA mode belongs to the same pass.
    if (const int native = ForeignNativeMsaa(); native >= 0)
        pitch = pitch * MsaaAcross((gpureg::Get(0x2000) >> 16) & 3) / MsaaAcross(uint32_t(native));
    static LiveSwitch off("FM2_NO_BIG_PASS");
    return !off && pitch > kWidth ? std::min<uint32_t>(2048, pitch * 64 / 65) : 0;
}
// Asked several times per draw (PassWidth, PassHeight, ScaleX, ScaleY): the answer only changes
// with the surface registers or the layout table.
uint32_t BigPassSize()
{
    struct Memo { uint32_t surface = ~0u, color = ~0u, generation = ~0u, result = 0; };
    thread_local Memo memo;
    const uint32_t surface = gpureg::Get(0x2000), color = gpureg::Get(0x2001) & 0xFFF, generation = g_nativeMsaaGeneration.load(std::memory_order_relaxed);
    if (surface != memo.surface || color != memo.color || generation != memo.generation)
        memo = { surface, color, generation, BigPassSizeUncached() };
    return memo.result;
}
uint32_t PassWidth() { const uint32_t big = BigPassSize(); return big ? big : kWidth; }
// Another MSAA view of the tall pass's EDRAM: the livery editor clears the stencil of its 1x
// 1024x1024 target through a 4x view (480x512, pitch 520) while a layer is being moved.
bool TallView()
{
    static LiveSwitch off("FM2_NO_TALL_VIEW"); // A/B
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    return !off && g_tallHeight && surface != g_tallInfo && ((surface ^ g_tallInfo) >> 16 & 3) && LayoutKey(surface) == g_tallKey;
}
uint32_t PassHeight()
{
    if (const uint32_t big = BigPassSize())
        return big;
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    return g_tallHeight && (surface == g_tallInfo || TallView()) ? g_tallHeight : kHeight;
}
float PassScale()
{
    // (Asked for every scaled coordinate: memoised on what it depends on.)
    struct Memo { uint32_t surface = ~0u, color = ~0u, generation = ~0u, tallInfo = ~0u, tallHeight = ~0u; float result = 1; };
    thread_local Memo memo;
    const uint32_t surface = gpureg::Get(0x2000), color = gpureg::Get(0x2001) & 0xFFF, generation = g_nativeMsaaGeneration.load(std::memory_order_relaxed);
    if (surface != memo.surface || color != memo.color || generation != memo.generation || g_tallInfo != memo.tallInfo || g_tallHeight != memo.tallHeight)
        memo = { surface, color, generation, g_tallInfo, g_tallHeight,
            std::min(float(kWidth) / float(PassWidth()), float(kHeight) / float(PassHeight())) };
    return memo.result;
}
float ScaleY() { return float(OutputHeight()) / float(kHeight) * g_sceneScale * PassScale(); }
float ScaleWide() { return float(OutputWidth()) / float(kWidth) * g_sceneScale * PassScale(); }
bool WidePass() { return (gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF) == kWidth; }
float ScaleX() { return WidePass() ? ScaleWide() : ScaleY(); }
LONG HostX(LONG v) { return LONG(std::lround(double(v) * ScaleX())); }
LONG HostY(LONG v) { return LONG(std::lround(double(v) * ScaleY())); }
D3D12_RECT ToHost(const D3D12_RECT& r)
{
    return { HostX(r.left), HostY(r.top), HostX(r.right), HostY(r.bottom) };
}

// Wider than 16:9: the output is `wide` times as wide as 16:9 for its height. The view
// extends horizontally by `extent` (1 + (wide - 1) x FM2_WIDE_FOV%): 100 is full Hor+ (the
// most side view, but rectilinear stretch at the edges grows with FM2's wide camera); 0
// keeps the 16:9 horizontal view and zooms in vertically instead.
float OutputWide() { return float(OutputWidth()) * kHeight / (float(OutputHeight()) * kWidth); }
float ViewExtent()
{
    static const float widePercent = [] {
        const char* v = getenv("FM2_WIDE_FOV");
        return v ? std::clamp(float(atof(v)), 0.0f, 100.0f) : 100.0f;
    }();
    return 1.0f + (std::max(OutputWide(), 1.0f) - 1.0f) * widePercent / 100.0f;
}
constexpr UINT kFrames = 3;
// The per-frame upload ring. Without guest memory mapped for the GPU (drivers without
// ID3D12Device13, e.g. older Windows 10 builds) every vertex buffer is copied into it: the
// menus alone then need well over 64 MB a frame, and draws past the end were dropped (black
// bands and missing letters). FM2_RING_MB overrides.
UINT64 kRingSize = 64ull << 20;
UINT64 g_ringPeak = 0;
constexpr UINT kDescriptorsPerFrame = 8192; // per frame: ring + guest memory views, then resolve-blit views
constexpr UINT kBindless = 1u << 16;      // permanent texture descriptors (0 null 2D, 1 null cube)
constexpr UINT kBindlessBase = kDescriptorsPerFrame * 3;
// The EDRAM colour target: half floats hold the title's HDR (7e3) range.
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr UINT kMaxSurfaces = 64; // EDRAM bases with host surfaces
// Samples per pixel of the scene surfaces (anti-aliasing, Settings::msaa). The blits that
// read them use multisampled shaders when this is above 1.
uint32_t g_msaa = 1;
constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002;
constexpr uint32_t kDirectFetchBytes = 16 * 1024; // buffers that change (UI quads, particles) are all under 3 KB (FM2_TRACE_MIDSIZE); FM2_DIRECT_FETCH_KB overrides

// Registers.
constexpr uint32_t RB_STENCILREFMASK = 0x210D;
constexpr uint32_t RB_COLOR_MASK = 0x2104, RB_ALPHA_REF = 0x210E, RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208, VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111, PA_CL_VPORT_YOFFSET = 0x2112;
constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_COPY_DEST_INFO = 0x231B;
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180, SQ_CONTEXT_MISC = 0x2181, PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281;
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E;

void LogDebugMessages();

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        LogDebugMessages();
        Fatal("renderer: %s failed (%08lX)", what, hr);
    }
}

float RegFloat(uint32_t index)
{
    const uint32_t v = gpureg::Get(index);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

struct CompiledShader
{
    enum State { Pending, Ready, Failed };
    std::atomic<int> state{ Pending }; // set by the compile worker
    uint64_t hash = 0, cacheKey = 0;
    bool pixel = false;
    std::vector<uint32_t> ucodeBE; // saved to the shader cache once a pipeline uses it
    ComPtr<ID3DBlob> code;
    TranslatedShader info;
    const CompiledShader* hlslSource = nullptr; // canonical variants: the shader whose code (and HLSL) this reuses
    mutable const CompiledShader* motion = nullptr; // its motion-vector variant, once looked up
    bool carBody = false; // a car-body vertex shader (ApplyDirtSeamFix): takes the easter egg's squash
    bool carPart = false; // another car-mesh vertex shader (tyres, brakes): moves with its car in the egg
    bool IsReady() const { return state.load(std::memory_order_acquire) == Ready; }
};

struct CachedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint64_t contentHash = 0;
    uint64_t checkedFrame = ~0ull; // content already validated in this frame
    uint32_t unchanged = 0;        // checks in a row that found the same content
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // cubes: matches their faces
    bool native = false; // block-compressed on the GPU: the shader applies the fetch swizzle
};

// A resolve destination backed by a host texture: fetches of this guest
// address read the copy of the render target instead of guest memory.
struct ResolvedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;          // game pixels
    uint32_t hostWidth = 0, hostHeight = 0;  // the texture (see ScaleX/ScaleY)
    uint64_t frame = ~0ull;                  // renderer frame of the last resolve into it
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // R32_FLOAT for depth resolves
    bool swapped = false; // resolved with the copy swap bit: guest memory has red and blue exchanged
    // Guest memory under the image when it was resolved (our resolves don't write it): if the title
    // later puts something else there (a CPU-made texture), the resolve is stale (see ResolvedFor).
    uint64_t memHash = 0, memChecked = 0;
    bool stale = false;
    uint32_t guestBpp = 4; // bytes per pixel of the guest image: the hash covers only its memory
    // The title resolves differently sized images (bloom chain) to one address within a
    // frame: keep earlier sizes here and swap them in rather than recreating them.
    std::vector<ResolvedTexture> spares;
};

// FM2_DUMP_RESOLVES readback of one resolved texture.
struct ResolveDump
{
    ComPtr<ID3D12Resource> buffer;
    uint32_t base = 0;
    int variant = 0;
    UINT width = 0, height = 0, pitch = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float minDepth = 0, maxDepth = 0;
    uint64_t midCount = 0;
};

struct FrameResources
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> ring;
    uint8_t* ringCpu = nullptr;
    std::atomic<UINT64> ringUsed{ 0 }; // draw workers allocate concurrently
    // Guest ranges already uploaded this frame: (address << 32 | bytes) -> (offset, sample hash).
    std::unordered_map<uint64_t, std::pair<UINT64, uint64_t>> uploads;
    UINT descriptorBase = 0;
    UINT descriptorsUsed = 0;
    UINT64 fence = 0;
    std::vector<ComPtr<ID3D12Resource>> garbage; // released once this frame's work completes
    bool timed = false; // FM2_PERF: GPU timestamps were written for this frame
    // Extra command lists of this frame (parallel draw pieces, main list continuations).
    std::vector<std::pair<ComPtr<ID3D12CommandAllocator>, ComPtr<ID3D12GraphicsCommandList>>> pool;
    size_t poolUsed = 0;
};

struct PipelineKey
{
    uint64_t vs, ps;
    uint32_t blend, depth, stencilMaskMode, flags;
    bool operator==(const PipelineKey&) const = default;
};
struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& k) const
    {
        uint64_t h = k.vs * 0x9E3779B97F4A7C15ull ^ k.ps;
        h ^= (uint64_t(k.blend) << 32 | k.depth) * 0xC2B2AE3D27D4EB4Full;
        h ^= (uint64_t(k.stencilMaskMode) << 32 | k.flags) * 0x165667B19E3779F9ull;
        return size_t(h ^ (h >> 29));
    }
};

struct BoundTexture
{
    ID3D12Resource* texture;
    DXGI_FORMAT format;
    bool cube;
};

// A pipeline state being (or already) built. The driver compiles the translated
// shaders to GPU code inside CreateGraphicsPipelineState, which takes 40-700 ms for
// the larger ones: that is done on worker threads and draws wait until it is ready.
struct PipelineEntry
{
    ComPtr<ID3D12PipelineState> pso;
    std::atomic<int> state{ 0 }; // 0 building, 1 ready, 2 failed
};

struct Renderer
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12GraphicsCommandList> realList;
    RecordedList recorder; // the main thread's command list (see recorded_list.h)
    ComPtr<ID3D12Heap> physicalHeap;     // the guest's physical memory section, mapped for the GPU
    ComPtr<ID3D12Resource> physical;     // 512 MB raw buffer over it (null if unsupported)
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 nextFence = 1;
    FrameResources frames[kFrames];
    UINT frameIndex = 0;
    bool listOpen = false;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12DescriptorHeap> srvHeap;     // shader visible
    ComPtr<ID3D12DescriptorHeap> stagingHeap; // CPU: null texture SRV
    UINT srvStride = 0;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    ComPtr<ID3D12DescriptorHeap> blitRtvHeap;             // one RTV, rewritten per depth resolve
    ComPtr<ID3D12RootSignature> blitRootSignature;
    ComPtr<ID3D12RootSignature> motionRootSignature; // root signature + b2 (motion-vector pipelines)
    ComPtr<ID3D12PipelineState> depthBlit;                // depth buffer -> R32_FLOAT
    ComPtr<ID3D12PipelineState> colorBlit8, colorBlit16;  // colour -> 8888 / RGBA16F, scaled
    ComPtr<ID3D12PipelineState> depthBlitMs, colorBlitMs8, colorBlitMs16; // the same from multisampled surfaces
    // EDRAM surfaces: one host colour and one host depth target per EDRAM base
    // (RB_COLOR_INFO / RB_DEPTH_INFO bits 0-11), so passes that the title
    // places at different EDRAM offsets do not overwrite each other.
    struct Surface
    {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_CPU_DESCRIPTOR_HANDLE view{};
    };
    std::unordered_map<uint32_t, Surface> colorSurfaces, depthSurfaces;
    ComPtr<ID3D12Resource> readback;

    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> shaders;
    std::unordered_map<PipelineKey, std::shared_ptr<PipelineEntry>, PipelineKeyHash> pipelines;
    // Keyed by address, format, width and height (TextureKey): looked up for every bound
    // texture of every draw.
    std::unordered_map<uint64_t, CachedTexture> textures;
    std::unordered_map<uint64_t, CachedTexture> cubes;
    std::unordered_map<uint32_t, CachedTexture> mipChains; // resolved textures with their mips, by base
    // Bindless texture descriptors.
    struct BindlessEntry
    {
        UINT index;
        ComPtr<ID3D12Resource> resource; // kept alive while the descriptor may be used
        uint64_t lastFrame;
    };
    std::unordered_map<uint64_t, BindlessEntry> bindless; // by resource pointer, format, cube
    std::vector<UINT> bindlessFree;
    UINT bindlessNext = 2;
    std::unordered_map<uint32_t, ResolvedTexture> resolved; // by physical address

    uint64_t frame = 0;
    std::atomic<uint32_t> drawsThisFrame{ 0 }, skippedThisFrame{ 0 };
    bool ok = false;
};

Renderer g_r;

// Shared caches used by draws on several threads (see DrawContext).
std::recursive_mutex g_shaderMutex;  // shaders, pipelines, the compiler's maps
std::recursive_mutex g_textureMutex; // textures, cubes, mip chains, bindless, texture memory, index arena
std::mutex g_surfaceMutex;           // EDRAM surfaces
// Parallel segments: texture uploads and copies a worker makes go to this list, executed
// before the segment's draws (another worker's earlier draws may use the same texture).
RecordedList* g_prepList = nullptr;

// What a thread recording draws keeps for itself: its command list, the bound surfaces and
// shaders. The command processor thread uses the main context (the frame's list); draw
// workers each have their own (see the parallel draw segments in gpu.cpp).
struct DrawContext
{
    RecordedList* list = nullptr;
    uint32_t colorBase = ~0u, depthBase = ~0u;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
    // The bound surfaces.
    ComPtr<ID3D12Resource> color, depth;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // Active shaders: the last microcode loaded per stage.
    std::vector<uint32_t> vsCode, psCode;
    uint64_t vsHash = 0, psHash = 0;
    bool worker = false; // a draw worker: frame-level work (begin, resolves, swaps) is not its job
};
DrawContext g_mainContext;
thread_local DrawContext* t_context = nullptr;
inline DrawContext& C() { return t_context ? *t_context : g_mainContext; }

// Holds the texture lock and, on a draw worker, records into the segment's prep list.
struct TextureScope
{
    std::lock_guard<std::recursive_mutex> lock{ g_textureMutex };
    RecordedList* saved = nullptr;
    TextureScope()
    {
        if (C().worker && g_prepList)
        {
            saved = C().list;
            C().list = g_prepList;
        }
    }
    ~TextureScope()
    {
        if (saved)
            C().list = saved;
    }
};

void LogDebugMessages()
{
    if (g_r.device)
        Log("renderer: device removed reason %08lX", g_r.device->GetDeviceRemovedReason());
    ComPtr<ID3D12InfoQueue> queue;
    if (!g_r.device || FAILED(g_r.device->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    const UINT64 n = queue->GetNumStoredMessages();
    for (UINT64 i = n > 20 ? n - 20 : 0; i < n; i++)
    {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        queue->GetMessage(i, message, &size);
        Log("d3d12: %s", message->pDescription);
    }
    queue->ClearStoredMessages();
}
double g_pipelineMs = 0; // FM2_PERF: time spent creating pipeline states this second
uint32_t g_pipelinesCreated = 0;
uint32_t g_skippedPipeline = 0;
const char* g_waitWhat = "?";
// The current draw samples the full-screen image the title resolved this frame (the
// post-processing composite): its flat geometry is a screen layer, not HUD (see vwide.w).
thread_local bool g_drawSamplesResolve = false;
// FM2_PERF: GPU time per frame from timestamps at the start and end of each frame's list.
ComPtr<ID3D12QueryHeap> g_timestamps;

// FM2_GPU_PASSES=1: GPU time per pass (between render target switches and resolves),
// summed by pass and logged every 2 s, to find where GPU time goes.
// FM2_GPU_SHADERS=1 also marks every draw, labelled by its shaders (approximate: the GPU overlaps draws).
const bool g_gpuShaders = getenv("FM2_GPU_SHADERS") != nullptr;
const bool g_gpuPasses = getenv("FM2_GPU_PASSES") != nullptr || g_gpuShaders;
constexpr UINT kPassMarks = 4096;
ComPtr<ID3D12QueryHeap> g_passQueries;
ComPtr<ID3D12Resource> g_passReadback;
struct PassMarkInfo
{
    char label[48];
    uint32_t draws; // draws in the pass this mark starts (filled at the next mark)
};
std::vector<PassMarkInfo> g_passMarks[3];
uint32_t g_passDraws = 0;

// Occlusion queries (EVENT_WRITE_ZPD: the sun flare's visibility). Each guest query becomes a
// D3D12 one; the guest gets the result of the same query (by order in the frame) from the
// last frame that has completed, so the command processor never waits for the GPU.
constexpr UINT kOcclusionPerFrame = 64;
ComPtr<ID3D12QueryHeap> g_occlusionHeap;
ComPtr<ID3D12Resource> g_occlusionReadback;
std::vector<std::pair<UINT, UINT>> g_occlusionPending[3]; // per frame slot: (order in the frame, query index)
std::vector<int64_t> g_occlusionResult;                     // by order in the frame: host samples, -1 none yet
uint64_t g_occlusionFrame = ~0ull;
UINT g_occlusionCount = 0, g_occlusionIndex = 0, g_occlusionOrder = 0;
bool g_occlusionOpen = false;
std::map<std::string, std::pair<double, uint64_t>> g_passTotals; // ms, draws
uint64_t g_passFrames = 0;
ComPtr<ID3D12Resource> g_timestampReadback;
double g_gpuFrameMs = 0, g_gpuFrameMaxMs = 0;
uint32_t g_gpuFrames = 0;
uint32_t g_waitBlend = 0;
std::map<std::string, uint64_t> g_created; // FM2_PERF: committed resources created, by kind
// FM2_PERF: what happened during the current frame, for the slow-frame log.
struct FrameEvents
{
    double waitMs = 0;          // CP thread waiting on shader/pipeline builds
    uint32_t waits = 0, pipelines = 0, created = 0;
    double section[8] = {};     // draw path sections (see Section), ms
    double worstDrawMs = 0;     // the single slowest draw
} g_frameEvents;
void CountCreate(const char* what)
{
    g_created[what]++;
    g_frameEvents.created++;
}

const bool g_dumpHlsl = getenv("FM2_DUMP_HLSL") != nullptr;

// Texture memory: textures are placed in large heaps instead of each getting a committed
// resource (its own heap). Creating a committed resource costs the driver milliseconds,
// which showed as hitches whenever new textures streamed in on the road ahead. Blocks are
// 64 KB multiples, reused by size once the GPU is done with the texture that held them.
struct TexturePool
{
    static constexpr uint64_t kHeapBytes = 128ull << 20;
    struct Block
    {
        ID3D12Heap* heap = nullptr;
        uint64_t offset = 0, size = 0;
    };
    std::vector<ComPtr<ID3D12Heap>> heaps;
    uint64_t used = kHeapBytes; // in heaps.back()
    std::unordered_map<uint64_t, std::vector<Block>> free; // by size
    std::unordered_map<ID3D12Resource*, Block> live;
    std::deque<std::pair<uint64_t, Block>> retired; // frame retired, block
} g_texturePool;

// A texture in the pool (or committed, if it does not fit a heap). Starts in `state`.
ComPtr<ID3D12Resource> CreateTexture(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, const char* kind)
{
    static LiveSwitch committed("FM2_COMMITTED_TEXTURES"); // A/B
    TexturePool& pool = g_texturePool;
    const D3D12_RESOURCE_ALLOCATION_INFO info = g_r.device->GetResourceAllocationInfo(0, 1, &desc);
    const uint64_t size = (info.SizeInBytes + 0xFFFF) & ~0xFFFFull;
    ComPtr<ID3D12Resource> r;
    if (!committed && size <= TexturePool::kHeapBytes && info.Alignment <= 0x10000)
    {
        TexturePool::Block block;
        auto& list = pool.free[size];
        if (!list.empty())
        {
            block = list.back();
            list.pop_back();
        }
        else
        {
            if (pool.used + size > TexturePool::kHeapBytes)
            {
                D3D12_HEAP_DESC hd{};
                hd.SizeInBytes = TexturePool::kHeapBytes;
                hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                hd.Alignment = 0x10000;
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                ComPtr<ID3D12Heap> heap;
                Check(g_r.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "CreateHeap(textures)");
                CountCreate("texture heap");
                pool.heaps.push_back(heap);
                pool.used = 0;
            }
            block = { pool.heaps.back().Get(), pool.used, size };
            pool.used += size;
        }
        if (SUCCEEDED(g_r.device->CreatePlacedResource(block.heap, block.offset, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        {
            pool.live[r.Get()] = block;
            return r;
        }
        pool.free[size].push_back(block);
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
              IID_PPV_ARGS(&r)),
        kind);
    CountCreate(kind);
    return r;
}

FrameResources& Frame();

// Hands a texture to the frame's garbage; its pool block is reused once no frame in flight
// can still read it.
void RetireTexture(ComPtr<ID3D12Resource>&& r)
{
    if (!r)
        return;
    if (auto it = g_texturePool.live.find(r.Get()); it != g_texturePool.live.end())
    {
        g_texturePool.retired.push_back({ g_r.frame, it->second });
        g_texturePool.live.erase(it);
    }
    Frame().garbage.push_back(std::move(r));
}

void ReclaimTextureBlocks()
{
    auto& retired = g_texturePool.retired;
    while (!retired.empty() && retired.front().first + kFrames + 2 < g_r.frame)
    {
        g_texturePool.free[retired.front().second.size].push_back(retired.front().second);
        retired.pop_front();
    }
}

// Bumped by every resolve: the per-slot texture bindings below are only reused while no
// resolve has changed what an address maps to.
uint64_t g_resolveEpoch = 0;

// The constants a texture fetch slot produced for the previous draw: draws usually bind
// the same fetch constants as their neighbours, and a repeat within the frame (same fetch,
// no resolve since) gives the same binding.
struct SlotBinding
{
    uint32_t fetch[6] = {};
    uint64_t frame = ~0ull, epoch = ~0ull;
    uint32_t tinfo[4] = {};
    float tbias[4] = {};
    bool samplesResolve = false;
};
// A cache valid for one frame: entries carry the frame they were made in instead of the map
// being cleared each frame (re-inserting freed nodes cost a heap allocation per entry).
template <class V>
struct FrameMap
{
    struct Entry
    {
        uint64_t frame;
        V value;
    };
    std::unordered_map<uint64_t, Entry> map;
    V* Find(uint64_t key, uint64_t frame)
    {
        auto it = map.find(key);
        return it != map.end() && it->second.frame == frame ? &it->second.value : nullptr;
    }
    void Put(uint64_t key, uint64_t frame, const V& value)
    {
        if (map.size() > (1u << 15)) // (stale entries of old frames)
            map.clear();
        map[key] = { frame, value };
    }
};

// Bindings by fetch constant: a texture bound by many draws (in any slot) is looked up once
// per frame and resolve epoch (textures are still checked once per frame for changes).
// (A direct-mapped table: an entry holds the fetch words it was made for, so a collision only
// recomputes. The hash map it replaced was 2-3% of the command processor.)
constexpr uint32_t kBindingSlots = 8192;
thread_local std::unique_ptr<SlotBinding[]> g_bindings;
thread_local uint64_t g_bindingsFrame = ~0ull, g_bindingsEpoch = ~0ull;

// The frame to trace (draws and resolves logged, the image and resolves written out):
// FM2_TRACE_DRAWS=<frame>, or the one after F12 is pressed in the game window.
std::atomic<uint64_t> g_hotkeyFrame{ 0 };
// (Read once at startup: a function-local static checks its initialisation guard on every call, and
// RendererTracing asks several times a draw.)
const uint64_t g_traceDrawsEnv = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
uint64_t TraceFrame()
{
    const uint64_t hot = g_hotkeyFrame.load(std::memory_order_relaxed);
    return hot ? hot : g_traceDrawsEnv;
}

// Why GetTexture last found nothing to bind (the draw then samples the white fallback): shown in
// the F12 capture (out/capture/textures.txt).
thread_local char g_textureFailure[96] = "";
void NoteTextureFailure(const char* why, const TextureFetch& fetch)
{
    snprintf(g_textureFailure, sizeof(g_textureFailure), "%s (type %u dim %u format %u)", why, fetch.Type(), fetch.Dimension(), fetch.Format());
}

// Per-draw timing (several clock reads per draw) only for FM2_PERF.
const bool g_perfTiming = getenv("FM2_PERF") != nullptr;

// The last draw state set on the command list: draws skip setting what has not changed
// (each D3D12 call costs driver time, and most draws share their neighbours' state).
// Invalidated wherever the list is reset or the root signature changes.
struct DrawState
{
    D3D12_VIEWPORT viewport{ -1, -1, -1, -1, -1, -1 };
    D3D12_RECT scissor{ -1, -1, -1, -1 };
    uint32_t stencilRef = ~0u;
    float blendFactor[4] = { -1, -1, -1, -1 };
    ID3D12PipelineState* pso = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE table2{}, table3{};
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool motionTargets = false; // the motion target is bound as the second render target
    bool motionRoot = false;    // the motion root signature is set
    D3D12_INDEX_BUFFER_VIEW indexBuffer{};
    void Invalidate() { *this = DrawState{}; }
};
thread_local DrawState g_drawState;

// Texture cache key: address (4 KB aligned), format (6 bits), width and height (13 bits each).
uint64_t TextureKey(uint32_t address, uint32_t format, uint32_t w, uint32_t h)
{
    return uint64_t(address >> 12) << 32 | uint64_t(format & 0x3F) << 26 | uint64_t((w - 1) & 0x1FFF) << 13 | ((h - 1) & 0x1FFF);
}

uint64_t Fnv(const void* data, size_t bytes, uint64_t h = 0xCBF29CE484222325ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Sampled hash for large texture sources: every 64th dword plus the ends.
// (Word-wise: it runs for every small buffer of every draw.)
uint64_t ContentHash(const uint8_t* data, size_t bytes, size_t stride = 256)
{
    auto mix = [](uint64_t h, uint64_t v) {
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        return h ^ (h >> 32);
    };
    uint64_t h = mix(0xCBF29CE484222325ull, bytes);
    for (size_t i = 0; i + 4 <= bytes; i += stride)
    {
        uint32_t v;
        memcpy(&v, data + i, 4);
        h = mix(h, v);
    }
    if (bytes >= 64)
        for (size_t i = bytes - 64; i < bytes; i += 8)
        {
            uint64_t v;
            memcpy(&v, data + i, 8);
            h = mix(h, v);
        }
    return h;
}

// Texture checks hashed ahead on worker threads: each frame starts by hashing the textures the
// last one checked, and the draw takes that hash when 32 spot reads (taken by the worker before
// its hash) still match memory, else hashes there as before. Hashing every texture in use on the
// command processor took ~5 ms a frame (PGR4; FM2 on an FX-8350: 21-34 ms a frame in textures); the staggered checks that avoided it showed
// streamed textures late. FM2_NO_TEXTURE_PREHASH hashes on the command processor (A/B).
namespace prehash
{
struct Job
{
    uint64_t key = 0;
    const uint8_t* source = nullptr;
    size_t bytes = 0, stride = 0;
    uint64_t spots = 0, hash = 0;
    std::atomic<uint32_t> state{ 0 }; // 0 waiting, 1 being hashed, 2 done
};
std::unique_ptr<Job[]> g_jobs;
uint32_t g_count = 0;
std::unordered_map<uint64_t, uint32_t> g_index; // key -> job, this frame (command processor only)
struct Pending
{
    uint64_t key;
    const uint8_t* source;
    size_t bytes, stride;
};
std::vector<Pending> g_next; // the textures this frame checks (for the next)
std::atomic<uint32_t> g_cursor{ 0 }, g_busy{ 0 };
std::atomic<uint64_t> g_generation{ 0 };
std::mutex g_mutex;
std::condition_variable g_cv;

uint64_t Spots(const uint8_t* data, size_t bytes)
{
    uint64_t h = bytes;
    for (uint32_t i = 0; i < 32 && bytes >= 8; i++)
    {
        uint64_t v;
        memcpy(&v, data + (bytes - 8) * i / 31, 8);
        h = (h ^ v) * 0x100000001B3ull;
    }
    return h;
}

uint64_t ContentHashFwd(const uint8_t* data, size_t bytes, size_t stride) { return ContentHash(data, bytes, stride); }

void Work(Job& job)
{
    uint32_t expected = 0;
    if (!job.state.compare_exchange_strong(expected, 1))
        return;
    job.spots = Spots(job.source, job.bytes);
    job.hash = ContentHashFwd(job.source, job.bytes, job.stride);
    job.state.store(2, std::memory_order_release);
}

void Worker()
{
    uint64_t seen = 0;
    for (;;)
    {
        {
            std::unique_lock lock(g_mutex);
            g_cv.wait(lock, [&] { return g_generation.load() != seen; });
            seen = g_generation.load();
            g_busy++;
        }
        for (uint32_t i; (i = g_cursor.fetch_add(1)) < g_count;)
            Work(g_jobs[i]);
        g_busy--;
    }
}

bool Enabled()
{
    static LiveSwitch off("FM2_NO_TEXTURE_PREHASH");
    return !off;
}

// At the start of a frame: last frame's textures go to the workers.
void StartFrame()
{
    static std::once_flag started;
    std::call_once(started, [] {
        for (int i = 0; i < 3; i++)
            std::thread(Worker).detach();
    });
    // The workers stop taking jobs and finish the one in hand before the jobs are replaced.
    g_cursor.store(~0u >> 1);
    while (g_busy.load())
        _mm_pause();
    g_index.clear();
    g_count = Enabled() ? uint32_t(g_next.size()) : 0;
    g_jobs.reset(g_count ? new Job[g_count] : nullptr);
    for (uint32_t i = 0; i < g_count; i++)
    {
        Job& j = g_jobs[i];
        j.key = g_next[i].key;
        j.source = g_next[i].source;
        j.bytes = g_next[i].bytes;
        j.stride = g_next[i].stride;
        g_index[j.key] = i;
    }
    g_next.clear();
    {
        std::lock_guard lock(g_mutex);
        g_cursor.store(0);
        g_generation++;
    }
    g_cv.notify_all();
}

// The command processor's thread (draw workers hash for themselves and queue nothing).
bool Owner()
{
    static const std::thread::id owner = std::this_thread::get_id();
    return std::this_thread::get_id() == owner;
}

LiveSwitch g_noAhead("FM2_NO_TEXTURE_AHEAD");

// A texture that skips its check this frame and is due next frame, queued for the workers. Hash
// queues the textures checked this frame, but with staggered checks (every 4th or 16th frame by
// key) those are not next frame's: the command processor was hashing most textures itself (~5% of
// its time in FM2's race). FM2_NO_TEXTURE_AHEAD=1: not queued (A/B).
void Ahead(uint64_t key, const uint8_t* source, size_t bytes, size_t stride)
{
    if (Enabled() && !g_noAhead && Owner())
        g_next.push_back({ key, source, bytes, stride });
}

// The texture's hash, from the workers when they have it and memory still matches.
uint64_t Hash(uint64_t key, const uint8_t* source, size_t bytes, size_t stride)
{
    if (!Enabled())
        return ContentHashFwd(source, bytes, stride);
    g_next.push_back({ key, source, bytes, stride });
    if (const auto it = g_index.find(key); it != g_index.end())
    {
        Job& job = g_jobs[it->second];
        if (job.source == source && job.bytes == bytes && job.stride == stride)
        {
            Work(job); // not reached yet: hashed here (and spot-checked, harmlessly)
            while (job.state.load(std::memory_order_acquire) != 2)
                _mm_pause();
            if (Spots(source, bytes) == job.spots)
                return job.hash;
        }
    }
    return ContentHashFwd(source, bytes, stride);
}
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, UINT index, UINT stride)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * stride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* heap, UINT index, UINT stride)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * stride;
    return h;
}

ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource(buffer)"); CountCreate("buffer");
    return r;
}

// Whether the GPU sees guest memory through the placed buffer over it. A driver can open the heap
// and still not alias it (vkd3d-proton on RADV: the calls succeed and the GPU reads zeros, so
// in-place vertex fetches draw nothing): a value written by the CPU is copied out by the GPU and
// compared. The block is ours, not memory the game may be using.
bool PhysicalHeapAliasesGuest(ID3D12Device* d, ID3D12Resource* physical)
{
    const uint32_t address = GuestAlloc(Heap::Physical, 4096, 4096);
    if (!address)
        return true; // nothing to test with: trust the mapping, as before
    uint32_t pattern[4];
    for (uint32_t i = 0; i < 4; i++)
        pattern[i] = 0x9E3779B9u * (i + 1) ^ uint32_t(GetTickCount64()) ^ address;
    memcpy(GuestPtr<uint8_t>(address), pattern, sizeof(pattern));

    ComPtr<ID3D12Resource> readback = CreateBuffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return true;
    list->CopyBufferRegion(readback.Get(), 0, physical, address & 0x1FFFFFFF, sizeof(pattern));
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    g_r.queue->ExecuteCommandLists(1, lists);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_r.queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, event);
    WaitForSingleObject(event, 5000);
    CloseHandle(event);
    if (fence->GetCompletedValue() < 1)
        return false; // the copy never finished: don't read through this heap
    uint32_t seen[4] = {};
    void* mapped = nullptr;
    const D3D12_RANGE range{ 0, sizeof(seen) };
    if (SUCCEEDED(readback->Map(0, &range, &mapped)))
    {
        memcpy(seen, mapped, sizeof(seen));
        readback->Unmap(0, nullptr);
    }
    if (memcmp(seen, pattern, sizeof(pattern)) == 0)
        return true;
    Log("renderer: guest memory self-test at %08X wrote %08X %08X, the GPU read %08X %08X", address, pattern[0], pattern[1], seen[0], seen[1]);
    return false;
}

// Depth resolves: a depth-stencil buffer can only be copied whole, so the
// resolve rectangle is drawn into an R32_FLOAT texture by a tiny pass that
// reads the depth buffer at the same pixel.
void InitDepthBlit()
{
    auto* d = g_r.device;
    // Anti-aliasing: the highest sample count the setting asks for that the device supports
    // for both the colour and depth formats (1 when none).
    g_msaa = 1;
    for (uint32_t want = GetSettings().msaa; want > 1; want /= 2)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS colour{ kColorFormat, want }, depth{ DXGI_FORMAT_D32_FLOAT_S8X24_UINT, want };
        if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &colour, sizeof colour)) && colour.NumQualityLevels &&
            SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &depth, sizeof depth)) && depth.NumQualityLevels)
        {
            g_msaa = want;
            break;
        }
    }
    Log("renderer: anti-aliasing %ux (setting %u)", g_msaa, GetSettings().msaa);
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 2;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: blit root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.blitRootSignature)),
        "CreateRootSignature(blit)");

    static const char kSource[] = R"(
Texture2D<float> depthTex : register(t0);
Texture2D<float4> colorTex : register(t0);
cbuffer Blit : register(b0) { float scale; uint swapRB; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target
{
    return depthTex.Load(int3(int2(pos.xy), 0));
}
float4 psColor(float4 pos : SV_Position) : SV_Target
{
    float4 c = colorTex.Load(int3(int2(pos.xy), 0)) * scale;
    return swapRB ? c.bgra : c;
}
)";
    ComPtr<ID3DBlob> vs, ps;
    Check(D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &error), "D3DCompile(blit vs)");
    Check(D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &error), "D3DCompile(blit ps)");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_r.blitRootSignature.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    static LiveSwitch noCull("FM2_NO_CULL");
    if (noCull)
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    pd.SampleDesc.Count = 1;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.depthBlit)), "CreateGraphicsPipelineState(depth blit)");
    ComPtr<ID3DBlob> psColor;
    Check(D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "psColor", "ps_5_0", 0, 0, &psColor, &error), "D3DCompile(blit psColor)");
    pd.PS = { psColor->GetBufferPointer(), psColor->GetBufferSize() };
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlit8)), "CreateGraphicsPipelineState(colour blit 8)");
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlit16)), "CreateGraphicsPipelineState(colour blit 16)");
    if (g_msaa > 1)
    {
        // The same passes reading a multisampled surface: depth takes sample 0, colour averages all samples.
        const std::string msSource = "#define SAMPLES " + std::to_string(g_msaa) + R"(
Texture2DMS<float, SAMPLES> depthMs : register(t0);
Texture2DMS<float4, SAMPLES> colorMs : register(t0);
cbuffer Blit : register(b0) { float scale; uint swapRB; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target
{
    return depthMs.Load(int2(pos.xy), 0);
}
float4 psColor(float4 pos : SV_Position) : SV_Target
{
    float4 c = 0;
    [unroll] for (int i = 0; i < SAMPLES; i++)
        c += colorMs.Load(int2(pos.xy), i);
    c = c / SAMPLES * scale;
    return swapRB ? c.bgra : c;
}
)";
        ComPtr<ID3DBlob> msVs, msPs, msColor;
        Check(D3DCompile(msSource.data(), msSource.size(), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &msVs, &error), "D3DCompile(blit ms vs)");
        Check(D3DCompile(msSource.data(), msSource.size(), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &msPs, &error), "D3DCompile(blit ms depth)");
        Check(D3DCompile(msSource.data(), msSource.size(), nullptr, nullptr, nullptr, "psColor", "ps_5_0", 0, 0, &msColor, &error), "D3DCompile(blit ms colour)");
        pd.VS = { msVs->GetBufferPointer(), msVs->GetBufferSize() };
        pd.PS = { msPs->GetBufferPointer(), msPs->GetBufferSize() };
        pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.depthBlitMs)), "CreateGraphicsPipelineState(depth blit ms)");
        pd.PS = { msColor->GetBufferPointer(), msColor->GetBufferSize() };
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlitMs8)), "CreateGraphicsPipelineState(colour blit ms 8)");
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlitMs16)), "CreateGraphicsPipelineState(colour blit ms 16)");
    }
}

void InitPipelineLibrary(ID3D12Device* device);

void Init()
{
    g_r.device = RenderDevice();
    // NGX at device creation: initialised later, mid-frame, DLSS reports itself unavailable
    // (feature init result FAIL_PlatformError), though the same call works here.
    if (GetSettings().upscaler == Settings::Upscaler::Dlss)
        DlssInit(g_r.device);
    g_r.queue = RenderQueue();
    auto* d = g_r.device;

    // Root signature: b0, b1 root CBVs; one table t0-t32; four static samplers.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2; // t0 upload ring, t1 guest memory
    D3D12_DESCRIPTOR_RANGE bindless[2]{};
    for (UINT i = 0; i < 2; i++)
    {
        // 2D textures in space1, cubes in space2, over the same descriptors.
        bindless[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindless[i].NumDescriptors = kBindless;
        bindless[i].RegisterSpace = 1 + i;
        bindless[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 2;
    params[3].DescriptorTable.pDescriptorRanges = bindless;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    const D3D12_FILTER filters[4] = { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_FILTER_MIN_MAG_MIP_POINT };
    const D3D12_TEXTURE_ADDRESS_MODE modes[4] = { D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP };
    for (UINT i = 0; i < 4; i++)
    {
        samplers[i].Filter = filters[i];
        // Wrapping linear filtering (surface textures: road, ground) is anisotropic: plain
        // trilinear picks blurred levels at grazing angles (blotchy asphalt). Not the clamped
        // sampler: it also reads shadow maps and other render targets, where averaging depth
        // over a long footprint breaks the shadows (black patches, glitchy car shadows). The
        // level is the player's setting (settings.ini anisotropy, 16 by default; 0 is trilinear).
        static LiveSwitch noAniso("FM2_NO_ANISO"); // A/B
        const uint32_t anisotropy = GetSettings().anisotropy;
        if (filters[i] == D3D12_FILTER_MIN_MAG_MIP_LINEAR && modes[i] == D3D12_TEXTURE_ADDRESS_MODE_WRAP && !noAniso && anisotropy >= 2)
        {
            samplers[i].Filter = D3D12_FILTER_ANISOTROPIC;
            samplers[i].MaxAnisotropy = anisotropy;
        }
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = modes[i];
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 4;
    rs.pParameters = params;
    rs.NumStaticSamplers = 4;
    rs.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.rootSignature)), "CreateRootSignature");
    // Motion-vector pipelines: the same plus b2, last frame's vertex constants of the draw.
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].Descriptor.ShaderRegister = 2;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rs.NumParameters = 5;
    blob.Reset();
    error.Reset();
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: motion root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.motionRootSignature)),
        "CreateRootSignature(motion)");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kBindlessBase + kBindless;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.srvHeap)), "CreateDescriptorHeap(srv)");
    g_r.srvStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    hd.NumDescriptors = 2 + 2 * kFrames; // null 2D, null cube, per frame: ring and guest-memory raw views
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.stagingHeap)), "CreateDescriptorHeap(staging)");
    hd.NumDescriptors = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &nullSrv, Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride));
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    nullSrv.TextureCube.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &nullSrv, Cpu(g_r.stagingHeap.Get(), 1, g_r.srvStride));
    d->CopyDescriptorsSimple(2, Cpu(g_r.srvHeap.Get(), kBindlessBase, g_r.srvStride), Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    InitPipelineLibrary(d);
    // Guest physical memory for the GPU, in place: vertex fetches read it
    // without copies.
    ComPtr<ID3D12Device13> d13;
    // FM2_NO_GUEST_HEAP=1 maps nothing: vertex data is copied (A/B: whether the CPU's own reads of guest
    // memory slow down once it is shared with the GPU on a given driver).
    const bool noGuestHeap = getenv("FM2_NO_GUEST_HEAP") != nullptr;
    HRESULT hr = noGuestHeap || getenv("FM2_FORCE_DEVICE3") ? E_NOINTERFACE : d->QueryInterface(IID_PPV_ARGS(&d13)); // (FM2_FORCE_DEVICE3: test the fallback)
    if (SUCCEEDED(hr))
        hr = d13->OpenExistingHeapFromFileMapping(PhysicalSection(), IID_PPV_ARGS(&g_r.physicalHeap));
    if (FAILED(hr) && d13)
    {
        Log("renderer: OpenExistingHeapFromFileMapping failed %08X; trying the view address", unsigned(hr));
        hr = d13->OpenExistingHeapFromAddress1(GuestPtr(0xA0000000), 0x20000000, IID_PPV_ARGS(&g_r.physicalHeap));
    }
    // Older runtimes (Windows 10, older drivers) have no ID3D12Device13, but ID3D12Device3 (Windows 10
    // 1709+) opens a heap over the whole allocation an address is in: the 512 MB view of guest
    // physical memory at 0xA0000000 is exactly that. The self-test below checks the GPU sees it.
    if (FAILED(hr) && !d13 && !noGuestHeap)
    {
        ComPtr<ID3D12Device3> d3;
        if (SUCCEEDED(hr = d->QueryInterface(IID_PPV_ARGS(&d3))))
            hr = d3->OpenExistingHeapFromAddress(GuestPtr(0xA0000000), IID_PPV_ARGS(&g_r.physicalHeap));
        if (SUCCEEDED(hr))
            Log("renderer: guest memory opened through ID3D12Device3 (no ID3D12Device13), %llu MB",
                (unsigned long long)(g_r.physicalHeap->GetDesc().SizeInBytes >> 20));
        else
            Log("renderer: mapping guest memory: no ID3D12Device13, and ID3D12Device3 failed %08X", unsigned(hr));
    }
    else if (FAILED(hr))
        Log("renderer: mapping guest memory: OpenExistingHeapFromAddress1 failed %08X", unsigned(hr));
    if (SUCCEEDED(hr))
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = g_r.physicalHeap->GetDesc().SizeInBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        if (FAILED(hr = d->CreatePlacedResource(g_r.physicalHeap.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_r.physical))))
        {
            Log("renderer: placing the guest memory buffer failed %08X", unsigned(hr));
            g_r.physical.Reset();
        }
    }
    // The calls can succeed on a driver that doesn't alias the memory (vkd3d-proton on RADV, the
    // Steam Deck: no text, no car bodies): test it. FM2_NO_HEAP_SELFTEST=1 trusts the mapping (A/B).
    if (g_r.physical && !getenv("FM2_NO_HEAP_SELFTEST") && !PhysicalHeapAliasesGuest(d, g_r.physical.Get()))
    {
        Log("renderer: the GPU does not see guest memory through the mapped heap; vertex data is copied instead");
        g_r.physical.Reset();
        g_r.physicalHeap.Reset();
    }
    Log("renderer: guest memory %s", g_r.physical ? "mapped for the GPU (zero-copy vertex fetch)" : "not mappable; vertex data is copied");
    // For the launcher's "Reduced performance" warning: what this PC actually got.
    if (FILE* f = _wfopen((UserDir() / "zero_copy.txt").c_str(), L"w"))
    {
        fputs(g_r.physical ? "1" : "0", f);
        fclose(f);
    }

    // Views for the EDRAM surfaces (created on first use).
    hd.NumDescriptors = kMaxSurfaces;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.rtvHeap)), "CreateDescriptorHeap(rtv)");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.dsvHeap)), "CreateDescriptorHeap(dsv)");
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.blitRtvHeap)), "CreateDescriptorHeap(blit rtv)");
    InitDepthBlit();
    g_r.readback = CreateBuffer(UINT64((OutputWidth() * 4 + 255) & ~255u) * OutputHeight(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    kRingSize = (getenv("FM2_RING_MB") ? std::clamp<UINT64>(UINT64(atoi(getenv("FM2_RING_MB"))), 16, 2048) : g_r.physical && !getenv("FM2_NO_DIRECT_FETCH") ? 64 : 384) << 20;
    Log("renderer: upload ring %llu MB per frame", (unsigned long long)(kRingSize >> 20));
    for (UINT i = 0; i < kFrames; i++)
    {
        FrameResources& f = g_r.frames[i];
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)), "CreateCommandAllocator");
        f.ring = CreateBuffer(kRingSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        f.ring->Map(0, nullptr, reinterpret_cast<void**>(&f.ringCpu));
        f.descriptorBase = i * kDescriptorsPerFrame;
    }
    Check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_r.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&g_r.realList)),
        "CreateCommandList");
    g_r.realList->Close();
    g_r.recorder.Init(g_r.realList.Get());
    g_mainContext.list = &g_r.recorder;
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_r.fence)), "CreateFence");
    g_r.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PresenterUseRenderer();
    g_r.ok = true;
    Log("renderer: D3D12 renderer ready");
}

double g_gpuWaitSeconds = 0; // CP thread blocked on the host GPU (frame resources)

FrameResources& Frame() { return g_r.frames[g_r.frameIndex]; }

void TransitionColor(D3D12_RESOURCE_STATES to);

// Gets (creating on first use) the host surface for an EDRAM base.
bool TemporalInit();
void BeginTemporalFrame();

Renderer::Surface& GetSurface(bool depth, uint32_t base)
{
    std::lock_guard surfaceLock(g_surfaceMutex);
    auto& map = depth ? g_r.depthSurfaces : g_r.colorSurfaces;
    if (auto it = map.find(base); it != map.end())
        return it->second;
    if (map.size() >= kMaxSurfaces)
        return map.begin()->second; // out of views: share one (should not happen)
    Renderer::Surface& s = map[base];
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = OutputWidth();
    td.Height = OutputHeight();
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = g_msaa;
    D3D12_CLEAR_VALUE cv{};
    const UINT index = UINT(map.size() - 1);
    if (depth)
    {
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS; // float depth (the title uses D24FS8, reverse-Z); typeless for resolves
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        cv.DepthStencil.Depth = 1.0f;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                  IID_PPV_ARGS(&s.resource)),
            "CreateCommittedResource(depth surface)"); CountCreate("depth surface");
        s.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        s.view = g_r.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        s.view.ptr += SIZE_T(index) * g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsvDesc.ViewDimension = g_msaa > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        C().list->CreateDepthStencilView(g_r.device, s.resource.Get(), dsvDesc, s.view); // in order with the recorded commands
    }
    else
    {
        td.Format = kColorFormat;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                  IID_PPV_ARGS(&s.resource)),
            "CreateCommittedResource(colour surface)"); CountCreate("colour surface");
        s.view = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        s.view.ptr += SIZE_T(index) * g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        C().list->CreateRenderTargetView(g_r.device, s.resource.Get(), s.view); // in order with the recorded commands
    }
    Log("renderer: EDRAM %s surface at base %03X", depth ? "depth" : "colour", base);
    return s;
}

// Binds the surfaces for the current RB_COLOR_INFO / RB_DEPTH_INFO bases
// (the list must be open).
FrameResources& Frame();

void PassMark(const char* fmt, uint32_t a, uint32_t b)
{
    if (!g_gpuPasses || !g_passQueries || !C().list)
        return;
    const UINT frameIndex = UINT(&Frame() - g_r.frames);
    auto& marks = g_passMarks[frameIndex];
    if (marks.size() >= kPassMarks)
        return;
    if (!marks.empty())
        marks.back().draws = g_passDraws;
    g_passDraws = 0;
    PassMarkInfo m{};
    snprintf(m.label, sizeof(m.label), fmt, a, b);
    C().list->EndQuery(g_passQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * kPassMarks + UINT(marks.size()));
    marks.push_back(m);
}

void SelectSurfaces(bool force)
{
    const uint32_t colorBase = gpureg::Get(RB_COLOR_INFO) & 0xFFF, depthBase = gpureg::Get(RB_DEPTH_INFO) & 0xFFF;
    if (!force && colorBase == C().colorBase && depthBase == C().depthBase)
        return;
    if (C().colorBase != ~0u && !C().worker) // (workers leave surfaces as render targets)
        g_r.colorSurfaces[C().colorBase].state = C().colorState;
    Renderer::Surface& c = GetSurface(false, colorBase);
    Renderer::Surface& z = GetSurface(true, depthBase);
    C().color = c.resource;
    C().colorState = c.state;
    C().rtv = c.view;
    C().depth = z.resource;
    C().dsv = z.view;
    C().colorBase = colorBase;
    C().depthBase = depthBase;
    PassMark("pass colour %03X depth %03X", colorBase, depthBase);
    TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    C().list->OMSetRenderTargets(1, &C().rtv, FALSE, &C().dsv);
    g_drawState.motionTargets = false;
}

// Bindless texture descriptors: one per (resource, format, dimension), kept
// while the texture is in use. Entries unused for a few frames (replaced or
// dropped textures) go back to the free list; their resources are released
// once the GPU is done with the frame that last used them.
UINT BindlessIndexLocked(ID3D12Resource* resource, DXGI_FORMAT format, bool cube);
LiveSwitch g_floatsViaStage("FM2_FLOATS_VIA_STAGE");

// The per-draw functions' switches, at namespace scope: as function-local statics each one checked
// its initialisation guard on every draw (dozens a draw).
LiveSwitch g_sw_FM2_ALL_CONSTANTS("FM2_ALL_CONSTANTS");
LiveSwitch g_sw_FM2_ASYNC_SHADERS("FM2_ASYNC_SHADERS");
LiveSwitch g_sw_FM2_CHECK_TEXTURES_ALWAYS("FM2_CHECK_TEXTURES_ALWAYS");
LiveSwitch g_sw_FM2_CLEAR_FETCH_TABLE("FM2_CLEAR_FETCH_TABLE");
LiveSwitch g_sw_FM2_CONSTANTS_BY_COUNT("FM2_CONSTANTS_BY_COUNT");
LiveSwitch g_sw_FM2_CONSTANTS_IN_RING("FM2_CONSTANTS_IN_RING");
LiveSwitch g_sw_FM2_DUMP_TEXTURES("FM2_DUMP_TEXTURES");
LiveSwitch g_sw_FM2_FULL_CONSTANT_BLOCKS("FM2_FULL_CONSTANT_BLOCKS");
LiveSwitch g_sw_FM2_NO_DIRECT_FETCH("FM2_NO_DIRECT_FETCH");
LiveSwitch g_sw_FM2_NO_DIRECT_INDEX("FM2_NO_DIRECT_INDEX");
LiveSwitch g_sw_FM2_NO_GPU_SWIZZLE("FM2_NO_GPU_SWIZZLE");
LiveSwitch g_sw_FM2_NO_HALF_PIXEL("FM2_NO_HALF_PIXEL");
LiveSwitch g_sw_FM2_NO_HUD_SQUEEZE("FM2_NO_HUD_SQUEEZE");
LiveSwitch g_sw_FM2_NO_INDEXED("FM2_NO_INDEXED");
LiveSwitch g_sw_FM2_NO_LIVE_CUBE("FM2_NO_LIVE_CUBE");
LiveSwitch g_sw_FM2_NO_MIPS("FM2_NO_MIPS");
LiveSwitch g_sw_FM2_NO_MSAA_SCALE("FM2_NO_MSAA_SCALE");
LiveSwitch g_sw_FM2_NO_NATIVE_BC("FM2_NO_NATIVE_BC");
LiveSwitch g_sw_FM2_NO_POLY_OFFSET("FM2_NO_POLY_OFFSET");
LiveSwitch g_sw_FM2_NO_RENDER("FM2_NO_RENDER");
LiveSwitch g_sw_FM2_NO_SHADER_SCAN("FM2_NO_SHADER_SCAN");
LiveSwitch g_sw_FM2_NO_STALE_RESOLVES("FM2_NO_STALE_RESOLVES");
LiveSwitch g_sw_FM2_OLD_CLAMP("FM2_OLD_CLAMP");
LiveSwitch g_sw_FM2_TEXTURE_CHECK_EVERY4("FM2_TEXTURE_CHECK_EVERY4");
LiveSwitch g_sw_FM2_TRACE_CAR_DRAWS("FM2_TRACE_CAR_DRAWS");
LiveSwitch g_sw_FM2_TRACE_DIRECT("FM2_TRACE_DIRECT");
LiveSwitch g_sw_FM2_TRACE_MIDSIZE("FM2_TRACE_MIDSIZE");

// Each thread remembers this frame's descriptors (eviction only takes ones unused for frames).
UINT BindlessIndex(ID3D12Resource* resource, DXGI_FORMAT format, bool cube)
{
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(resource)) << 8 ^ uint64_t(format) << 1 ^ uint64_t(cube);
    thread_local FrameMap<UINT> known;
    if (const UINT* index = known.Find(key, g_r.frame))
        return *index;
    std::lock_guard textureLock(g_textureMutex);
    const UINT index = BindlessIndexLocked(resource, format, cube);
    known.Put(key, g_r.frame, index);
    return index;
}

UINT BindlessIndexLocked(ID3D12Resource* resource, DXGI_FORMAT format, bool cube)
{
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(resource)) << 8 ^ uint64_t(format) << 1 ^ uint64_t(cube);
    auto it = g_r.bindless.find(key);
    if (it != g_r.bindless.end())
    {
        it->second.lastFrame = g_r.frame;
        return it->second.index;
    }
    UINT index;
    if (!g_r.bindlessFree.empty())
    {
        index = g_r.bindlessFree.back();
        g_r.bindlessFree.pop_back();
    }
    else if (g_r.bindlessNext < kBindless)
        index = g_r.bindlessNext++;
    else
        return cube ? 1u : 0u; // full: draw with a null texture
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.Shader4ComponentMapping = format == DXGI_FORMAT_BC4_UNORM
        ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0) // DXT5A: its one channel in all four
        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (cube)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = UINT(-1);
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = UINT(-1); // all levels (resolved mip chains)
    }
    g_r.device->CreateShaderResourceView(resource, &srv, Cpu(g_r.srvHeap.Get(), kBindlessBase + index, g_r.srvStride));
    g_r.bindless[key] = { index, resource, g_r.frame };
    return index;
}

void EvictBindless()
{
    if (g_r.frame % 16 != 0)
        return;
    for (auto it = g_r.bindless.begin(); it != g_r.bindless.end();)
    {
        if (it->second.lastFrame + kFrames + 2 < g_r.frame)
        {
            Frame().garbage.push_back(std::move(it->second.resource));
            g_r.bindlessFree.push_back(it->second.index);
            it = g_r.bindless.erase(it);
        }
        else
            ++it;
    }
}

void BeginFrameIfNeeded()
{
    if (g_r.listOpen)
        return;
    FrameResources& f = Frame();
    if (g_r.fence->GetCompletedValue() < f.fence)
    {
        const auto w0 = std::chrono::steady_clock::now();
        g_r.fence->SetEventOnCompletion(f.fence, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, INFINITE);
        g_gpuWaitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    }
    f.garbage.clear();
    void FinishDepthWriteback(UINT frameIndex);
    void FinishPhotoWriteback(UINT frameIndex);
    FinishDepthWriteback(UINT(&f - g_r.frames)); // this slot's previous frame has completed
    FinishPhotoWriteback(UINT(&f - g_r.frames));
    static LiveSwitch perf("FM2_PERF");
    const UINT frameIndex = UINT(&f - g_r.frames);
    if (!g_occlusionPending[frameIndex].empty())
    {
        // This slot's previous frame has completed: keep its occlusion results.
        uint64_t* counts = nullptr;
        const D3D12_RANGE range{ size_t(frameIndex) * kOcclusionPerFrame * 8, size_t(frameIndex + 1) * kOcclusionPerFrame * 8 };
        if (SUCCEEDED(g_occlusionReadback->Map(0, &range, reinterpret_cast<void**>(&counts))))
        {
            for (const auto& [order, index] : g_occlusionPending[frameIndex])
            {
                if (g_occlusionResult.size() <= order)
                    g_occlusionResult.resize(order + 1, -1);
                g_occlusionResult[order] = int64_t(counts[index]);
            }
            const D3D12_RANGE none{ 0, 0 };
            g_occlusionReadback->Unmap(0, &none);
        }
        g_occlusionPending[frameIndex].clear();
    }
    if (perf && !g_timestamps)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2 * kFrames;
        g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_timestamps));
        g_timestampReadback = CreateBuffer(8 * 2 * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (g_gpuPasses && !g_passQueries)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kPassMarks * kFrames;
        g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_passQueries));
        g_passReadback = CreateBuffer(8ull * kPassMarks * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (g_gpuPasses && g_passMarks[frameIndex].size() > 1)
    {
        // This slot's previous frame has completed: add up its passes.
        auto& marks = g_passMarks[frameIndex];
        uint64_t* t = nullptr;
        const D3D12_RANGE range{ size_t(frameIndex) * kPassMarks * 8, size_t(frameIndex + 1) * kPassMarks * 8 };
        if (SUCCEEDED(g_passReadback->Map(0, &range, reinterpret_cast<void**>(&t))))
        {
            UINT64 frequency = 1;
            g_r.queue->GetTimestampFrequency(&frequency);
            const uint64_t* ts = t + size_t(frameIndex) * kPassMarks;
            for (size_t i = 0; i + 1 < marks.size(); i++)
            {
                if (ts[i + 1] < ts[i])
                    continue; // marks from lists that ran out of order
                auto& total = g_passTotals[marks[i].label];
                total.first += double(ts[i + 1] - ts[i]) * 1000.0 / double(frequency);
                total.second += marks[i].draws;
            }
            const D3D12_RANGE none{ 0, 0 };
            g_passReadback->Unmap(0, &none);
            g_passFrames++;
        }
        marks.clear();
        static auto lastLog = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(2) && g_passFrames)
        {
            lastLog = std::chrono::steady_clock::now();
            std::vector<std::pair<double, std::string>> top;
            double sum = 0;
            for (auto& [label, total] : g_passTotals)
            {
                top.push_back({ total.first, label });
                sum += total.first;
            }
            std::sort(top.rbegin(), top.rend());
            Log("gpu passes: %.2f ms/frame over %llu frames", sum / g_passFrames, (unsigned long long)g_passFrames);
            for (size_t i = 0; i < top.size() && i < (g_gpuShaders ? 30u : 14u); i++)
                Log("gpu passes: %6.3f ms  %5.0f draws  %s", top[i].first / g_passFrames, double(g_passTotals[top[i].second].second) / g_passFrames,
                    top[i].second.c_str());
            g_passTotals.clear();
            g_passFrames = 0;
        }
    }
    g_passMarks[frameIndex].clear();
    if (perf && f.timed && g_timestampReadback)
    {
        // This slot's previous frame has completed (waited for above).
        uint64_t* t = nullptr;
        const D3D12_RANGE range{ size_t(frameIndex) * 16, size_t(frameIndex) * 16 + 16 };
        if (SUCCEEDED(g_timestampReadback->Map(0, &range, reinterpret_cast<void**>(&t))))
        {
            UINT64 frequency = 1;
            g_r.queue->GetTimestampFrequency(&frequency);
            const double ms = double(t[frameIndex * 2 + 1] - t[frameIndex * 2]) * 1000.0 / double(frequency);
            const D3D12_RANGE none{ 0, 0 };
            g_timestampReadback->Unmap(0, &none);
            if (ms > 0 && ms < 1000)
            {
                g_gpuFrameMs += ms;
                g_gpuFrameMaxMs = std::max(g_gpuFrameMaxMs, ms);
                g_gpuFrames++;
            }
        }
        f.timed = false;
    }
    f.allocator->Reset();
    f.poolUsed = 0;
    g_r.recorder.Rebind(g_r.realList.Get()); // (a pool list may have continued last frame)
    C().list->Reset(f.allocator.Get(), nullptr);
    // This frame's scene phase and scale (see UpscaleScene).
    {
        const Settings& st = GetSettings();
        const bool temporalUpscaler = st.upscaler == Settings::Upscaler::Dlss || st.upscaler == Settings::Upscaler::Fsr3;
        const float scale = st.SceneScale();
        g_upscale.switchedThisFrame = false;
        g_scenePhase = st.upscaler != Settings::Upscaler::Off && (temporalUpscaler || scale < 1.0f) && g_r.frame >= g_upscale.retryFrame &&
            (!temporalUpscaler || TemporalInit());
        g_temporal = g_scenePhase && temporalUpscaler;
        g_sceneScale = g_scenePhase ? scale : 1.0f;
        if (g_temporal)
            BeginTemporalFrame();
    }
    PassMark("frame start %u%u", 0, 0);
    if (perf && g_timestamps)
        C().list->EndQuery(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2);
    if (const UINT64 used = f.ringUsed.load(); used > g_ringPeak)
    {
        g_ringPeak = used;
        if (used > kRingSize / 2)
            Log("renderer: upload ring peak %llu MB of %llu", (unsigned long long)(used >> 20), (unsigned long long)(kRingSize >> 20));
    }
    f.ringUsed = 0;
    f.uploads.clear();
    // Per-frame table: t0 this frame's ring, t1 guest physical memory.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Buffer.NumElements = UINT(kRingSize / 4);
        raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        g_r.device->CreateShaderResourceView(f.ring.Get(), &raw, Cpu(g_r.srvHeap.Get(), f.descriptorBase, g_r.srvStride));
        if (g_r.physical)
        {
            raw.Buffer.NumElements = UINT(g_r.physical->GetDesc().Width / 4);
            g_r.device->CreateShaderResourceView(g_r.physical.Get(), &raw, Cpu(g_r.srvHeap.Get(), f.descriptorBase + 1, g_r.srvStride));
        }
        else
            g_r.device->CreateShaderResourceView(f.ring.Get(), &raw, Cpu(g_r.srvHeap.Get(), f.descriptorBase + 1, g_r.srvStride));
    }
    f.descriptorsUsed = 2;
    EvictBindless();
    ReclaimTextureBlocks();
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    C().list->SetDescriptorHeaps(1, heaps);
    C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    g_drawState.Invalidate();
    SelectSurfaces(true);
    C().list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_r.listOpen = true;
}

// Allocates from the frame's upload ring; returns the offset (or ~0).
UINT64 RingAlloc(UINT64 bytes, UINT64 alignment = 256)
{
    FrameResources& f = Frame();
    UINT64 used = f.ringUsed.load(std::memory_order_relaxed), offset;
    for (;;)
    {
        offset = (used + alignment - 1) & ~(alignment - 1);
        if (offset + bytes > kRingSize || f.ringUsed.compare_exchange_weak(used, offset + bytes, std::memory_order_relaxed))
            break;
    }
    if (offset + bytes > kRingSize)
    {
        static UINT64 lastFull = 0;
        static int fullLogs = 0;
        if ((RendererTracing() || fullLogs < 20) && lastFull != g_r.frame && ++fullLogs)
        {
            lastFull = g_r.frame;
            Log("ring full: %llu bytes requested, %llu used, %llu uploads cached", (unsigned long long)bytes, (unsigned long long)f.ringUsed.load(), (unsigned long long)f.uploads.size());
        }
        return ~0ull;
    }
    return offset;
}

// Copies a guest range into the ring once per frame; draws sharing a vertex
// buffer reuse the copy. A sampled hash catches ranges rewritten mid-frame.
UINT64 UploadGuest(uint32_t guest, uint32_t bytes)
{
    FrameResources& f = Frame();
    const uint8_t* src = GuestPtr<uint8_t>(guest);
    // Small buffers are often rewritten between draws (UI quads), so hash all
    // of them; large ones (static geometry) are sampled.
    uint64_t sample = bytes;
    static const uint32_t fullHashBytes = getenv("FM2_UPLOAD_FULL_HASH_KB") ? uint32_t(atoi(getenv("FM2_UPLOAD_FULL_HASH_KB"))) * 1024u : 16384u;
    if (bytes <= fullHashBytes)
        sample = ContentHash(src, bytes);
    else
        for (uint32_t i = 0; i < 16 && bytes >= 8; i++)
        {
            uint64_t v;
            memcpy(&v, src + (uint64_t(bytes - 8) * i / 15), 8);
            sample = (sample ^ v) * 0x100000001B3ull;
        }
    const uint64_t key = uint64_t(guest) << 32 | bytes;
    // Each recording thread keeps its own cache (cleared when the frame changes).
    thread_local FrameMap<std::pair<UINT64, uint64_t>> uploads;
    if (const auto* known = uploads.Find(key, g_r.frame); known && known->second == sample)
        return known->first;
    if (RendererTracing() && bytes > (16u << 10))
        Log("upload: %08X %u bytes (ring %llu)", guest, bytes, (unsigned long long)f.ringUsed.load());
    const UINT64 offset = RingAlloc(std::max<uint32_t>(bytes, 16) + 16, 16);
    if (offset == ~0ull)
        return offset;
    memcpy(f.ringCpu + offset, src, bytes);
    uploads.Put(key, g_r.frame, { offset, sample });
    return offset;
}

// Index buffers for indexed draws: the guest's big-endian indices, swapped once into an
// arena on the GPU and checked for changes once per frame (sampled, like textures). Real
// indexed draws let the GPU reuse transformed vertices; the vertex shader then gets the
// index as its vertex ID ("auto" index mode, see hlsl.cpp).
struct IndexArena
{
    static constexpr uint64_t kBytes = 128ull << 20;
    ComPtr<ID3D12Resource> buffer;
    uint64_t used = 0;
    std::unordered_map<uint32_t, std::vector<uint64_t>> free; // offsets by size class (log2)
    struct Entry
    {
        uint64_t offset = 0, hash = 0, checkedFrame = ~0ull;
        uint32_t sizeClass = 0;
        D3D12_GPU_VIRTUAL_ADDRESS staged = 0; // this frame's copy in the upload ring (frame stagedFrame)
        uint64_t stagedFrame = ~0ull;
    };
    std::unordered_map<uint64_t, Entry> entries;
    bool full = false;
    // Copies into the arena, made at the end of the frame in one batch (a barrier
    // on the arena mid-frame would drain the GPU each time new geometry streams in).
    struct Copy
    {
        ID3D12Resource* ring;
        uint64_t from, to, bytes;
    };
    std::vector<Copy> pending;
} g_indexArena;

// This frame's index uploads, copied into the arena once its draws are recorded.
void FlushIndexCopies()
{
    IndexArena& arena = g_indexArena;
    if (arena.pending.empty())
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = arena.buffer.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    C().list->ResourceBarrier(1, &b);
    for (const auto& c : arena.pending)
        C().list->CopyBufferRegion(arena.buffer.Get(), c.to, c.ring, c.from, c.bytes);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    arena.pending.clear();
}

// The view of the guest index buffer at physical address `base` (count indices), swapped
// into the arena; false when it cannot be cached (the draw fetches indices itself).
bool GetIndexBufferLocked(uint32_t base, uint32_t count, bool is32, uint32_t endian, D3D12_INDEX_BUFFER_VIEW& view);
// Each thread remembers this frame's index buffers (checked once per frame).
bool GetIndexBuffer(uint32_t base, uint32_t count, bool is32, uint32_t endian, D3D12_INDEX_BUFFER_VIEW& view)
{
    thread_local FrameMap<D3D12_INDEX_BUFFER_VIEW> known;
    const uint64_t key = uint64_t(base) << 32 ^ uint64_t(count) << 3 ^ uint64_t(is32) << 2 ^ endian;
    if (const auto* v = known.Find(key, g_r.frame))
    {
        view = *v;
        return true;
    }
    std::lock_guard textureLock(g_textureMutex);
    if (!GetIndexBufferLocked(base, count, is32, endian, view))
        return false;
    known.Put(key, g_r.frame, view);
    return true;
}

bool GetIndexBufferLocked(uint32_t base, uint32_t count, bool is32, uint32_t endian, D3D12_INDEX_BUFFER_VIEW& view)
{
    IndexArena& arena = g_indexArena;
    const uint32_t bytes = count * (is32 ? 4 : 2);
    if (!bytes || bytes > (16u << 20))
        return false;
    if (arena.full && arena.entries.size())
    {
        // Start over: copies in this frame run after the frames in flight are done.
        arena.entries.clear();
        arena.free.clear();
        arena.used = 0;
        arena.full = false;
    }
    if (!arena.buffer)
    {
        arena.buffer = CreateBuffer(IndexArena::kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        if (!arena.buffer)
            return false;
    }
    const uint64_t key = uint64_t(base) << 32 ^ uint64_t(bytes) << 3 ^ uint64_t(is32) << 2 ^ endian;
    auto [it, inserted] = arena.entries.try_emplace(key);
    IndexArena::Entry& e = it->second;
    if (inserted)
    {
        uint32_t sizeClass = 8; // 256 bytes
        while ((1ull << sizeClass) < bytes)
            sizeClass++;
        auto& list = arena.free[sizeClass];
        if (!list.empty())
        {
            e.offset = list.back();
            list.pop_back();
        }
        else if (arena.used + (1ull << sizeClass) <= IndexArena::kBytes)
        {
            e.offset = arena.used;
            arena.used += 1ull << sizeClass;
        }
        else
        {
            arena.entries.erase(it);
            arena.full = true;
            return false;
        }
        e.sizeClass = sizeClass;
    }
    view = { arena.buffer->GetGPUVirtualAddress() + e.offset, bytes, is32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
    if (e.checkedFrame == g_r.frame)
    {
        if (e.stagedFrame == g_r.frame)
            view.BufferLocation = e.staged;
        return true;
    }
    e.checkedFrame = g_r.frame;
    const uint8_t* src = GuestPtr<uint8_t>(GpuPhysToGuest(base));
    const uint64_t hash = ContentHash(src, bytes);
    if (!inserted && hash == e.hash)
        return true;
    // Swap into the upload ring (as the vertex shader would read them), then copy.
    const UINT64 offset = RingAlloc((bytes + 15) & ~15u, 16);
    if (offset == ~0ull)
        return false;
    uint8_t* dst = Frame().ringCpu + offset;
    if (is32)
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t v;
            memcpy(&v, src + i * 4, 4);
            if (endian == 2)
                v = __builtin_bswap32(v);
            else if (endian == 1)
                v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
            memcpy(dst + i * 4, &v, 4);
        }
    else
        for (uint32_t i = 0; i < count; i++)
        {
            uint16_t v;
            memcpy(&v, src + i * 2, 2);
            if (endian == 1 || endian == 2)
                v = __builtin_bswap16(v);
            memcpy(dst + i * 2, &v, 2);
        }
    // This frame draws from the upload ring; the arena gets the copy at the end of the frame.
    e.staged = Frame().ring->GetGPUVirtualAddress() + offset;
    e.stagedFrame = g_r.frame;
    view.BufferLocation = e.staged;
    arena.pending.push_back({ Frame().ring.Get(), offset, e.offset, bytes });
    e.hash = hash;
    return true;
}

// Compile activity, for the on-screen overlay and FM2_PERF.
struct CompileStats
{
    std::atomic<int> busy{ 0 };            // jobs (shader compiles, pipeline builds) in flight
    std::atomic<uint32_t> shadersBuilt{ 0 }, shadersCached{ 0 }, pipelinesBuilt{ 0 }, pipelinesLoaded{ 0 };
    std::atomic<uint64_t> lastColdMs{ 0 };  // steady-clock ms of the last real compile
    std::atomic<uint32_t> warmTotal{ 0 }, warmDone{ 0 };
    std::atomic<uint32_t> staticDone{ 0 }, staticTotal{ 0 }; // first boot: static shaders compiled / to compile
    std::mutex mutex;
    std::string current;                    // what the oldest in-flight job is
    std::chrono::steady_clock::time_point currentSince;
    void Begin(const std::string& what)
    {
        std::lock_guard lock(mutex);
        if (busy.load() == 0)
            currentSince = std::chrono::steady_clock::now();
        current = what;
        busy++;
    }
    void End(bool cold)
    {
        busy--;
        if (cold)
            lastColdMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
} g_stats;

// Everything the title has ever bound is recorded (shader microcode as files in
// shadercache/, pipeline states in pipelines.manifest) so later runs rebuild all of
// it in the background at boot instead of stalling the first time a race needs it.
// Cache files written off the command processor thread: opening a file can take
// milliseconds (more with a virus scanner), which showed as frame spikes whenever a race
// met a new pipeline.
struct DiskWriter
{
    std::mutex mutex;
    std::condition_variable cv, idle;
    std::deque<std::function<void()>> queue;
    bool busy = false;

    void Push(std::function<void()> job)
    {
        static std::once_flag started;
        std::call_once(started, [this] {
            std::thread([this] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;)
                {
                    std::function<void()> next;
                    {
                        std::unique_lock lock(mutex);
                        cv.wait(lock, [this] { return !queue.empty(); });
                        next = std::move(queue.front());
                        queue.pop_front();
                        busy = true;
                    }
                    next();
                    std::lock_guard lock(mutex);
                    busy = false;
                    if (queue.empty())
                        idle.notify_all();
                }
            }).detach();
        });
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }
    // Waits until everything queued is written (at exit).
    void Flush()
    {
        std::unique_lock lock(mutex);
        idle.wait_for(lock, std::chrono::seconds(5), [this] { return queue.empty() && !busy; });
    }
} g_diskWriter;

struct Manifest
{
    std::mutex mutex;
    std::unordered_set<PipelineKey, PipelineKeyHash> known;
    std::vector<PipelineKey> pending; // loaded at start, not built yet
    size_t next = 0;
    std::unordered_set<uint64_t> shaderFiles;

    static std::filesystem::path Path() { return UserDir() / "pipelines.manifest"; }
    static std::filesystem::path UcodePath(bool pixel, uint64_t hash)
    {
        char name[48];
        snprintf(name, sizeof(name), "%s_%016llx.ucode", pixel ? "ps" : "vs", (unsigned long long)hash);
        return UserDir() / "shadercache" / name;
    }
    void Load()
    {
        std::ifstream in(Path());
        std::string line;
        while (std::getline(in, line))
        {
            PipelineKey k{};
            unsigned long long vs, ps;
            if (sscanf(line.c_str(), "P %llx %llx %x %x %x %x", &vs, &ps, &k.blend, &k.depth, &k.stencilMaskMode, &k.flags) == 6)
            {
                k.vs = vs;
                k.ps = ps;
                if (known.insert(k).second)
                    pending.push_back(k);
            }
        }
        g_stats.warmTotal = uint32_t(pending.size());
        Log("renderer: manifest lists %zu pipelines to rebuild", pending.size());
    }
    void RecordPipeline(const PipelineKey& k)
    {
        std::lock_guard lock(mutex);
        if (!known.insert(k).second)
            return;
        g_diskWriter.Push([k] {
            std::ofstream(Path(), std::ios::app) << "P " << std::hex << k.vs << ' ' << k.ps << ' ' << k.blend << ' ' << k.depth << ' '
                                                  << k.stencilMaskMode << ' ' << k.flags << '\n';
        });
    }
    void RecordShader(bool pixel, uint64_t hash, const std::vector<uint32_t>& codeBE)
    {
        std::lock_guard lock(mutex);
        if (codeBE.empty() || !shaderFiles.insert(hash).second)
            return;
        g_diskWriter.Push([path = UcodePath(pixel, hash), code = codeBE] {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * 4));
        });
    }
    static bool LoadShader(bool pixel, uint64_t hash, std::vector<uint32_t>& out)
    {
        std::ifstream in(UcodePath(pixel, hash), std::ios::binary | std::ios::ate);
        if (!in)
            return false;
        out.resize(size_t(in.tellg()) / 4);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size() * 4));
        return !out.empty();
    }
} g_manifest;

// Shader compilation runs on worker threads so the command processor never
// stalls on fxc: a draw whose shaders are still compiling is skipped. The
// bytecode is cached on disk by HLSL hash, so later runs start warm.
// Pixel shader cache entries' seed: 3 since they compile without FM2_VERTEX_STAGE (the 1 entries
// sampled without mipmapping).
constexpr uint64_t kPixelCacheSeed = 3;

std::filesystem::path ShaderCachePath(uint64_t key)
{
    char name[32];
    snprintf(name, sizeof(name), "%016llx.dxbc", (unsigned long long)key);
    return UserDir() / "shadercache" / name;
}

struct ShaderCompiler
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<CompiledShader*> queue;
    std::deque<CompiledShader*> background; // static shaders: compiled when nothing urgent waits
    std::vector<std::thread> workers;
    std::atomic<int> jobs{ 0 };   // urgent, queued or compiling (holds wait for these)
    std::atomic<int> waiting{ 0 }; // background, queued or parked
    // Once the game runs, background pushes (memory-scan finds: mostly stale copies the
    // game never draws) are parked instead: translated, compiled only if a draw promotes them.
    std::atomic<bool> park{ false };
    std::unordered_set<const CompiledShader*> parked;

    void Start()
    {
        std::filesystem::create_directories(UserDir() / "shadercache");
        // All but one core: the first boot compiles the whole game behind the overlay.
        const unsigned n = std::max(2u, std::thread::hardware_concurrency() - 1);
        for (unsigned i = 0; i < n; i++)
            workers.emplace_back([this] { Run(); });
        for (auto& w : workers)
            w.detach();
    }

    void Push(CompiledShader* cs, bool later = false)
    {
        {
            std::lock_guard lock(mutex);
            if (later)
            {
                waiting++;
                if (park.load())
                    parked.insert(cs);
                else
                    background.push_back(cs);
            }
            else
            {
                jobs++;
                queue.push_back(cs);
            }
        }
        cv.notify_one();
    }

    // Moves a background shader something now needs to the front of the urgent queue.
    void Promote(const CompiledShader& cs)
    {
        if (cs.state.load(std::memory_order_relaxed) != CompiledShader::Pending || !waiting.load(std::memory_order_relaxed))
            return;
        std::lock_guard lock(mutex);
        bool found = false;
        if (parked.erase(&cs))
            found = true;
        else if (auto it = std::find(background.begin(), background.end(), &cs); it != background.end())
        {
            background.erase(it);
            found = true;
        }
        if (found)
        {
            waiting--;
            jobs++;
            queue.push_front(const_cast<CompiledShader*>(&cs));
        }
    }

    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            CompiledShader* cs;
            bool urgent;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !queue.empty() || !background.empty(); });
                urgent = !queue.empty();
                auto& from = urgent ? queue : background;
                cs = from.front();
                from.pop_front();
                if (!urgent)
                    waiting--;
            }
            if (urgent)
            {
                char label[64];
                snprintf(label, sizeof(label), "%s %08llx", cs->pixel ? "pixel shader" : "vertex shader", (unsigned long long)(cs->hash >> 32));
                g_stats.Begin(label);
                Compile(*cs);
                g_stats.End(true);
                jobs--;
            }
            else
                Compile(*cs);
        }
    }

    static void Compile(CompiledShader& cs)
    {
        ComPtr<ID3DBlob> errors;
        // Runtime compiles skip optimisation: fxc can take minutes on the
        // larger translated shaders.
        const auto start = std::chrono::steady_clock::now();
        // The translated source picks its sampling form per stage (see hlsl.cpp: a vertex
        // shader cannot sample with an implicit LOD).
        // Only for vertex shaders: D3DCompile ends the list at a null *name*, so { "FM2_VERTEX_STAGE",
        // nullptr } still defined it, and every pixel shader sampled at the bias as an absolute LOD
        // (mip 0): no mipmapping anywhere, minified ground textures sparkled (the grainy asphalt,
        // sand and gravel since 0.5.3).
        const D3D_SHADER_MACRO vertexMacros[] = { { "FM2_VERTEX_STAGE", "" }, { nullptr, nullptr } };
        const HRESULT hr = D3DCompile(cs.info.hlsl.data(), cs.info.hlsl.size(), nullptr, cs.pixel ? nullptr : vertexMacros, nullptr, "main",
            cs.pixel ? "ps_5_1" : "vs_5_1", D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &cs.code, &errors);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (FAILED(hr))
        {
            Log("renderer: %s %016llx failed to compile:\n%s", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            cs.state.store(CompiledShader::Failed, std::memory_order_release);
            return;
        }
        g_stats.shadersBuilt++;
        if (ms > 200)
            Log("renderer: %s %016llx compiled in %.0f ms (background)", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash, ms);
        std::ofstream(ShaderCachePath(cs.cacheKey), std::ios::binary)
            .write(static_cast<const char*>(cs.code->GetBufferPointer()), std::streamsize(cs.code->GetBufferSize()));
        cs.state.store(CompiledShader::Ready, std::memory_order_release);
    }
} g_compiler;

// Shaders by canonical key: the microcode with the vertex fetch fields the title patches at
// load masked out. A patched copy of a translated shader shares its HLSL and bytecode and
// only needs its own fetch layout (TranslateVertexFetchLayout), not a full translation
// (which took up to ~18 ms on the command processor for a large shader).
std::unordered_map<uint64_t, const CompiledShader*> g_canonical;
// Bytecode already loaded, by HLSL hash: a shader whose HLSL matches one in memory doesn't
// read the shader cache from disk again.
std::unordered_map<uint64_t, ComPtr<ID3DBlob>> g_bytecode;

uint64_t CanonicalKey(const ucode::Shader& parsed)
{
    std::vector<uint32_t> w = parsed.words;
    for (const ucode::Cf& cf : parsed.cf)
    {
        if (cf.IsExec())
            for (uint32_t k = 0; k < cf.count(); k++)
            {
                const uint32_t index = cf.address() + k;
                if (!((cf.sequence() >> (2 * k)) & 1) || (index + 1) * 3 > w.size() || (w[index * 3] & 0x1F) != 0)
                    continue;
                w[index * 3] &= ~0x1FF00000u;   // fetch constant
                w[index * 3 + 1] &= 0x80000000u; // swizzle, format, sign, rounding, exponent, mini
                w[index * 3 + 2] &= 0x80000000u; // stride, offset
            }
        if (cf.IsEnd())
            break;
    }
    return Fnv(w.data(), w.size() * 4, 0x5A17);
}

// background: queued behind everything a draw needs (the static shader set).
// Live pixel shader edits (the debug server's "ps HASH EXPR"): draws with that shader use an alias hash
// instead, translated afresh with its output replaced, so they get their own pipelines and the original
// shader is untouched (shaders are never removed). A new edit gets a new alias; clearing it goes back.
std::mutex g_psEditMutex;
std::unordered_map<uint64_t, uint64_t> g_psEditAlias; // shader -> alias
std::unordered_map<uint64_t, std::string> g_psEditExpr; // alias -> the expression it returns
std::atomic<bool> g_psEdits{ false };
std::unordered_set<uint64_t> g_psEditEver; // every alias made (kept out of the pipeline manifest)
// The debug server's drawstats: draws per (vs, ps) over one frame.
std::atomic<int> g_drawStats{ 0 }; // 1 armed for the next frame, 2 counting, 0 off
std::mutex g_drawStatsMutex;
std::map<std::pair<uint64_t, uint64_t>, uint32_t> g_drawStatsCounts;
bool IsPsEditAlias(uint64_t hash)
{
    std::lock_guard lock(g_psEditMutex);
    return g_psEditEver.count(hash) != 0;
}
uint64_t PsEditAlias(uint64_t hash)
{
    if (!g_psEdits.load(std::memory_order_relaxed))
        return hash;
    std::lock_guard lock(g_psEditMutex);
    auto it = g_psEditAlias.find(hash);
    return it == g_psEditAlias.end() ? hash : it->second;
}

// The car body's dirt map (a DXT1 1024x128 the game updates as dirt builds up) is wrapped round the car:
// the body vertex shader takes u from an atan2 of the normal, whose branch cut runs along the top centre
// line, so u is ~1 on one side of the bonnet and ~0 on the other. The triangles across that line
// interpolate the long way round and squeeze the whole map into a strip: once there is dirt, dark streaks
// along the bonnet (the AE86 at Tsukuba from lap 2). Seam-safe sampling: those vertex shaders also output
// frac(u + 0.5) (o[9], with a flag in .y), and pixel shaders that sample at that u take whichever of the
// two changes less across the pixel. Elsewhere both give the same texel. FM2_NO_SEAM_FIX=1 turns it off.
// The easter egg's squash, for every mesh of a car (body, tyres, brakes, glass): the clip position is moved
// by K (opos.xyw - o.xyw), the reference car's axes scaled about its origin (dealer.cpp DealerCarDraw), so
// all of a car's parts squash together whatever transform each was drawn with. Off (vsquash zero) normally.
bool AddCarSquash(std::string& hlsl)
{
    static const std::string end = "}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n";
    const size_t e = hlsl.find(end);
    if (e == std::string::npos)
        return false;
    hlsl.insert(e, "            if (vsquash[3].w > 0) { float3 q = opos.xyw - float3(vsquash[0].w, vsquash[1].w, vsquash[2].w);\n"
                   "                opos += float4(dot(vsquash[0].xyz, q), dot(vsquash[1].xyz, q), dot(vsquash[2].xyz, q), dot(vsquash[3].xyz, q)); }\n");
    return true;
}

// Other car meshes (tyres, brake discs): their positions are dequantised like the body's (q * w + vc[191]).
bool MarkCarPart(std::string& hlsl)
{
    static const std::regex dequant(R"(\(r\[\d+\]\.\w+\), \(r\[\d+\]\.wwww\)\) \+ \(vc\[191\])");
    return std::regex_search(hlsl, dequant) && AddCarSquash(hlsl);
}

// Returns true for a car-body vertex shader (it also gets the easter egg's bounce: see dealer.cpp).
bool ApplyDirtSeamFix(std::string& hlsl, bool pixel)
{
    static LiveSwitch off("FM2_NO_SEAM_FIX");
    if (!pixel)
    {
        // o[0].z = u (from the angle), o[0].w = v; o[9] is free in these shaders.
        static const std::string uv = "(r[0].wwww) + (vc[189].xxxx); s = (vc[190].x) + (r[0].y); ps = s; o[0].z = v.z; o[0].w = s.xxxx.w; }";
        const size_t at = hlsl.find(uv);
        if (at == std::string::npos)
            return false;
        if (!off && hlsl.find("o[9]") == std::string::npos)
            hlsl.insert(at + uv.size(), " o[9] = float4(frac(o[0].z + 0.5), 1, 0, 0);");
        AddCarSquash(hlsl);
        return true;
    }
    if (off)
        return false;
    static const std::string coord = "float3(r[0].z, r[0].w, r[0].x)";
    static const std::string init = "for (int j = 0; j < 16; j++) r[j] = input.o[j];";
    const size_t start = hlsl.find(init);
    if (start == std::string::npos || hlsl.find(coord) == std::string::npos)
        return false;
    // Chosen before the shader's control-flow loop (derivatives there would be per-branch).
    hlsl.insert(start + init.size(),
        "\n    float seamU = input.o[9].y > 0.5 && fwidth(input.o[9].x) < fwidth(input.o[0].z) ? input.o[9].x - 0.5 : input.o[0].z;");
    for (size_t at; (at = hlsl.find(coord)) != std::string::npos;)
        hlsl.replace(at, coord.size(), "float3(input.o[9].y > 0.5 ? seamU : r[0].z, r[0].w, r[0].x)");
    return false;
}

const CompiledShader& GetShaderLocked(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel, bool background);
// Shaders are never removed: each thread remembers the ones it looked up (no lock per draw).
const CompiledShader& GetShader(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel, bool background = false)
{
    thread_local std::unordered_map<uint64_t, const CompiledShader*> known;
    if (auto it = known.find(hash); it != known.end())
        return *it->second;
    std::lock_guard shaderLock(g_shaderMutex);
    const CompiledShader& cs = GetShaderLocked(hash, codeBE, pixel, background);
    known.emplace(hash, &cs);
    return cs;
}

const CompiledShader& GetShaderLocked(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel, bool background)
{
    auto it = g_r.shaders.find(hash);
    if (it != g_r.shaders.end())
    {
        if (!background)
            g_compiler.Promote(*it->second);
        return *it->second;
    }
    static std::once_flag started;
    std::call_once(started, [] { g_compiler.Start(); });
    auto& slot = g_r.shaders[hash];
    slot = std::make_unique<CompiledShader>();
    CompiledShader& cs = *slot;
    cs.hash = hash;
    cs.pixel = pixel;
    // Not saved yet: the memory scan also finds stale copies in freed blocks (heap links
    // over the first words), so only shaders a pipeline uses go into the cache.
    cs.ucodeBE = codeBE;
    const auto getStart = std::chrono::steady_clock::now();
    static LiveSwitch perfLog("FM2_PERF");
    const char* path = "full";
    struct LogSlow
    {
        std::chrono::steady_clock::time_point start;
        const CompiledShader& cs;
        const char*& path;
        ~LogSlow()
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (perfLog && ms > 5.0)
                Log("perf: GetShader %s %016llx took %.1f ms (%s, %zu dwords)", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash, ms, path,
                    cs.ucodeBE.size());
        }
    } logSlow{ getStart, cs, path };
    const ucode::Shader parsed = ucode::Parse(codeBE.data(), uint32_t(codeBE.size()), pixel);
    static LiveSwitch noCanonical("FM2_NO_CANONICAL");
    // Baked vertex fetch puts each copy's layout in its code, so copies can't share one compile.
    const uint64_t canonical = pixel || noCanonical || BakedVertexFetch() || IsPsEditAlias(hash) ? 0 : CanonicalKey(parsed);
    if (canonical)
        path = g_canonical.count(canonical) ? (g_canonical[canonical]->IsReady() ? "full, layout fallback" : "full, base not ready") : "full, new canonical";
    if (canonical)
        if (auto c = g_canonical.find(canonical); c != g_canonical.end() && c->second->IsReady())
        {
            const CompiledShader& base = *c->second;
            TranslatedShader layout;
            if (TranslateVertexFetchLayout(parsed, layout))
            {
                cs.info.ok = true;
                cs.info.usedTextureSlots = base.info.usedTextureSlots;
                cs.info.constantsUsed = base.info.constantsUsed; // same ALU code
                cs.info.constantMask = base.info.constantMask;
                cs.info.interpolatorsWritten = base.info.interpolatorsWritten;
                cs.info.vertexFetchLayout = std::move(layout.vertexFetchLayout);
                cs.info.vertexFetchConstants = std::move(layout.vertexFetchConstants);
                cs.cacheKey = base.cacheKey;
                cs.code = base.code;
                cs.hlslSource = &base;
                cs.carBody = base.carBody;
                cs.carPart = base.carPart;
                cs.state = CompiledShader::Ready;
                g_stats.shadersCached++;
                path = "variant";
                return cs;
            }
        }
    cs.info = TranslateShader(parsed);
    // Only a translation whose fetches all read vfl can stand in for its patched copies; one
    // with baked fetch fields would hand them the wrong layout.
    if (canonical && cs.info.ok)
        if (TranslatedShader check; TranslateVertexFetchLayout(parsed, check))
            g_canonical.emplace(canonical, &cs);
    if (!cs.info.ok)
    {
        Log("renderer: %s %016llx not translated: %s", pixel ? "ps" : "vs", (unsigned long long)hash, cs.info.error.c_str());
        // FM2_DUMP_SHADERS: keep the microcode too, so a missing instruction can be looked
        // at with tools/xenos_dis (the on-load dump only covers shaders that get this far).
        static const bool dumpShaders = getenv("FM2_DUMP_SHADERS") != nullptr;
        if (dumpShaders)
        {
            const auto dir = UserDir() / "shaders";
            std::filesystem::create_directories(dir);
            char name[64];
            snprintf(name, sizeof(name), "fail_%s_%016llx.bin", pixel ? "ps" : "vs", (unsigned long long)hash);
            std::ofstream(dir / name, std::ios::binary)
                .write(reinterpret_cast<const char*>(cs.ucodeBE.data()), std::streamsize(cs.ucodeBE.size() * 4));
        }
        cs.state = CompiledShader::Failed;
        return cs;
    }
    cs.carBody = ApplyDirtSeamFix(cs.info.hlsl, pixel);
    cs.carPart = !pixel && !cs.carBody && MarkCarPart(cs.info.hlsl);
    if (cs.carBody)
        Log("renderer: vs %016llx is a car body", (unsigned long long)hash);
    if (g_dumpHlsl)
    {
        std::filesystem::create_directories("out/hlsl");
        char name[64];
        snprintf(name, sizeof(name), "out/hlsl/%s_%016llx.hlsl", pixel ? "ps" : "vs", (unsigned long long)hash);
        if (FILE* f = fopen(name, "wb"))
        {
            fwrite(cs.info.hlsl.data(), 1, cs.info.hlsl.size(), f);
            fclose(f);
        }
    }
    // Debug: FM2_DEBUG_VS=<hash>:<statement> appends a statement to that vertex
    // shader's guest program (it can read r[] and write o[]).
    if (const char* dbg = getenv("FM2_DEBUG_VS"); dbg && !pixel && strtoull(dbg, nullptr, 16) == hash)
        if (const char* stmt = strchr(dbg, ':'))
        {
            // "<hash>:@<block>:<statement>" inserts at the start of control-flow block <block>.
            std::string s = stmt + 1, marker = "}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n";
            if (s[0] == '@')
            {
                const size_t colon = s.find(':');
                marker = "        if (cfpc == " + s.substr(1, colon - 1) + "u)\n        {\n";
                s = s.substr(colon + 1);
            }
            if (size_t at = cs.info.hlsl.find(marker); at != std::string::npos)
                cs.info.hlsl.insert(marker[0] == '}' ? at : at + marker.size(), "            " + s + "\n");
        }
    // Debug: FM2_TINT_PS=h1,h2,... paints those pixel shaders in distinct flat
    // colours (red, green, blue, yellow, magenta, cyan, white, orange, ...).
    if (const char* list = getenv("FM2_TINT_PS"); list && pixel)
    {
        static const char* kTints[] = { "1,0,0", "0,1,0", "0,0,1", "1,1,0", "1,0,1", "0,1,1", "1,1,1", "1,0.5,0", "0.5,0,1", "0,0.5,0.3",
            "0.5,0.5,0.5", "0.3,0.15,0" };
        int n = 0;
        for (const char* s = list; *s && n < 12; n++)
        {
            char* end;
            const uint64_t h = strtoull(s, &end, 16);
            if (h == hash)
                if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                    cs.info.hlsl.replace(at, 13, std::string("return float4(") + kTints[n] + ", 1);");
            s = *end == ',' ? end + 1 : end;
            if (end == s && *end != ',')
                break;
        }
    }
    // Debug: FM2_DEBUG_PS=<hash>:<expr> replaces that pixel shader's output, e.g.
    // "a62c809452fcc71b:float4(input.o[4].xyz, 1)".
    // FM2_DEBUG_PS_ALL=<expr> does the same for every pixel shader.
    if (const char* all = getenv("FM2_DEBUG_PS_ALL"); all && pixel)
        if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
            cs.info.hlsl.replace(at, 13, std::string("return ") + all + ";");
    if (const char* dbg = getenv("FM2_DEBUG_PS"); dbg && pixel && strtoull(dbg, nullptr, 16) == hash)
        if (const char* expr = strchr(dbg, ':'))
            if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                cs.info.hlsl.replace(at, 13, std::string("return ") + (expr + 1) + ";");
    if (!pixel && g_psEdits.load(std::memory_order_relaxed))
    {
        std::string stmt;
        {
            std::lock_guard lock(g_psEditMutex);
            if (auto e = g_psEditExpr.find(hash); e != g_psEditExpr.end())
                stmt = e->second;
        }
        const std::string marker = "}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n";
        // "@TEXT|STATEMENT" runs it after the first line holding TEXT instead of at the end.
        size_t at = std::string::npos;
        if (!stmt.empty() && stmt[0] == '@')
        {
            const size_t bar = stmt.find('|');
            if (bar != std::string::npos)
                if (size_t hit = cs.info.hlsl.find(stmt.substr(1, bar - 1)); hit != std::string::npos)
                    at = cs.info.hlsl.find('\n', hit) + 1;
            stmt = bar == std::string::npos ? "" : stmt.substr(bar + 1);
        }
        else if (!stmt.empty())
            at = cs.info.hlsl.find(marker);
        if (!stmt.empty() && at != std::string::npos && at != 0)
        {
            cs.info.hlsl.insert(at, "            " + stmt + "\n");
            Log("debug: vs alias %016llx runs %s", (unsigned long long)hash, stmt.c_str());
        }
        else if (!stmt.empty())
            Log("debug: vs alias %016llx: the anchor wasn't found", (unsigned long long)hash);
    }
    if (pixel && g_psEdits.load(std::memory_order_relaxed))
    {
        std::string expr;
        {
            std::lock_guard lock(g_psEditMutex);
            if (auto e = g_psEditExpr.find(hash); e != g_psEditExpr.end())
                expr = e->second;
        }
        if (!expr.empty())
        {
            if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                cs.info.hlsl.replace(at, 13, "return " + expr + ";");
            Log("debug: ps alias %016llx returns %s", (unsigned long long)hash, expr.c_str());
        }
    }
    cs.cacheKey = Fnv(cs.info.hlsl.data(), cs.info.hlsl.size(), pixel ? kPixelCacheSeed : 2);
    if (auto b = g_bytecode.find(cs.cacheKey); b != g_bytecode.end())
    {
        cs.code = b->second;
        cs.state = CompiledShader::Ready;
        g_stats.shadersCached++;
        return cs;
    }
    if (std::ifstream in{ ShaderCachePath(cs.cacheKey), std::ios::binary | std::ios::ate })
    {
        const auto size = size_t(in.tellg());
        if (size && SUCCEEDED(D3DCreateBlob(size, &cs.code)))
        {
            in.seekg(0);
            in.read(static_cast<char*>(cs.code->GetBufferPointer()), std::streamsize(size));
            cs.state = CompiledShader::Ready;
            g_bytecode.emplace(cs.cacheKey, cs.code);
            g_stats.shadersCached++;
            return cs;
        }
    }
    g_compiler.Push(&cs, background);
    return cs;
}

// Motion-vector variant of a translated shader (temporal upscalers). Built by rewriting the
// generated HLSL, so default shaders (and their caches) stay as they are:
// - the vertex constants gain last frame's c[] (vcPrev, see DrawImpl) and the frame's
//   jitter (vjitter, NDC units);
// - the vertex shader runs the guest program a second time with vcPrev (GuestVSPrev), puts
//   both clip positions in two more interpolants and jitters only the rasterized position;
// - the pixel shader also writes the motion to SV_Target1 as a UV offset (previous minus
//   current, in the 0..1 range of the target).
// Empty when the shader's text is not in the expected shape.
std::string MotionVariant(const std::string& hlsl, bool pixel)
{
    std::string h = hlsl;
    auto replace = [&](const std::string& from, const std::string& to) {
        const size_t at = h.find(from);
        if (at == std::string::npos)
            return false;
        h.replace(at, from.size(), to);
        return true;
    };
    if (!replace("    float4 vwide;\n};", "    float4 vwide;\n    float4 vjitter;\n};\ncbuffer VSPrevious : register(b2)\n{\n    float4 vcPrev[256];\n};") ||
        !replace("    float2 pcoord : TEXCOORD16; // point sprite coordinate\n",
            "    float2 pcoord : TEXCOORD16; // point sprite coordinate\n    float4 mcur : TEXCOORD17;\n    float4 mprev : TEXCOORD18;\n"))
        return {};
    if (pixel)
    {
        if (!replace("float4 main(VSOut input) : SV_Target0\n{", "struct PSMotionOut { float4 c : SV_Target0; float2 mv : SV_Target1; };\n"
                                                                    "PSMotionOut main(VSOut input)\n{"))
            return {};
        const size_t at = h.rfind("    return oC[0];\n}");
        if (at == std::string::npos)
            return {};
        h.replace(at, std::string("    return oC[0];\n}").size(),
            "    PSMotionOut motionOut;\n"
            "    motionOut.c = oC[0];\n"
            "    float2 cur = input.mcur.xy / input.mcur.w, prev = input.mprev.xy / input.mprev.w;\n"
            "    motionOut.mv = (prev - cur) * float2(0.5, -0.5);\n"
            "    return motionOut;\n}");
        return h;
    }
    // Vertex shader: GuestVSPrev, a copy of the guest program reading last frame's constants.
    const std::string sig = "void GuestVS(uint vindex, out float4 opos, out float4 o[16], out float4 opts)\n{";
    const size_t start = h.find(sig);
    const size_t end = h.find("}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    std::string prev = h.substr(start, end + 2 - start);
    prev.replace(0, std::string("void GuestVS(").size(), "void GuestVSPrev(");
    for (size_t at = 0; (at = prev.find("vc[", at)) != std::string::npos; at += 7)
        prev.replace(at, 3, "vcPrev[");
    h.insert(end + 2, prev);
    // The viewport transform (from the target size to the clip position) as a function.
    const std::string from = "    float2 size = float2(vinfo.zw);\n";
    const std::string last = "    r.pos = float4(ndc * w, p.z * w, w);\n";
    const size_t a = h.find(from), b = h.find(last);
    const size_t mainAt = h.find("VSOut main(uint id : SV_VertexID)\n{");
    if (a == std::string::npos || b == std::string::npos || mainAt == std::string::npos || b < a || a < mainAt)
        return {};
    std::string body = h.substr(a, b - a);
    for (size_t at = 0; (at = body.find("r.pos", at)) != std::string::npos; at += 3)
        body.replace(at, 5, "pos");
    const std::string fn = "float4 ToClip(float4 pos, float2 pointOffset)\n{\n" + body + "    return float4(ndc * w, p.z * w, w);\n}\n";
    h.replace(a, b + last.size() - a,
        "    r.pos = ToClip(r.pos, pointOffset);\n"
        "    r.mcur = r.pos;\n"
        "    r.mprev = r.pos;\n"
        "    if (!isPoint && !rect)\n"
        "    {\n"
        "        float4 q, qo[16], qpts;\n"
        "        GuestVSPrev(e, q, qo, qpts);\n"
        "        r.mprev = ToClip(q, pointOffset);\n"
        "    }\n"
        "    r.pos.xy += vjitter.xy * r.pos.w;\n");
    h.insert(mainAt, fn);
    // VSOut members the original code never sets.
    return h;
}

// The motion-vector variant of a shader (compiled on first use; nullptr if it cannot be made).
std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> g_motionShaders;
const CompiledShader* MotionShaderLookup(const CompiledShader& base);
const CompiledShader* MotionShader(const CompiledShader& base)
{
    if (base.motion && base.motion->state.load(std::memory_order_acquire) != CompiledShader::Pending)
        return base.motion->state.load(std::memory_order_acquire) == CompiledShader::Failed ? nullptr : base.motion;
    const CompiledShader* m = MotionShaderLookup(base);
    base.motion = m;
    return m;
}

const CompiledShader* MotionShaderLookup(const CompiledShader& base)
{
    const CompiledShader& source = base.hlslSource ? *base.hlslSource : base;
    auto [it, inserted] = g_motionShaders.try_emplace(source.cacheKey);
    if (!inserted)
        return it->second->state.load(std::memory_order_acquire) == CompiledShader::Failed ? nullptr : it->second.get();
    it->second = std::make_unique<CompiledShader>();
    CompiledShader& cs = *it->second;
    cs.hash = source.hash;
    cs.pixel = source.pixel;
    cs.info = source.info;
    cs.info.hlsl = MotionVariant(source.info.hlsl, source.pixel);
    if (cs.info.hlsl.empty())
    {
        LOG_ONCE("upscale: no motion-vector variant for %s %016llx", source.pixel ? "ps" : "vs", (unsigned long long)source.hash);
        cs.state = CompiledShader::Failed;
        return nullptr;
    }
    cs.cacheKey = Fnv(cs.info.hlsl.data(), cs.info.hlsl.size(), source.pixel ? kPixelCacheSeed : 2);
    if (auto b = g_bytecode.find(cs.cacheKey); b != g_bytecode.end())
    {
        cs.code = b->second;
        cs.state = CompiledShader::Ready;
        return &cs;
    }
    if (std::ifstream in{ ShaderCachePath(cs.cacheKey), std::ios::binary | std::ios::ate })
    {
        const auto size = size_t(in.tellg());
        if (size && SUCCEEDED(D3DCreateBlob(size, &cs.code)))
        {
            in.seekg(0);
            in.read(static_cast<char*>(cs.code->GetBufferPointer()), std::streamsize(size));
            cs.state = CompiledShader::Ready;
            g_bytecode.emplace(cs.cacheKey, cs.code);
            return &cs;
        }
    }
    g_compiler.Push(&cs, false);
    return &cs;
}

D3D12_BLEND Blend(uint32_t f)
{
    switch (f)
    {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: return D3D12_BLEND_BLEND_FACTOR;
    case 14: return D3D12_BLEND_ALPHA_FACTOR;
    case 13: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 15: return D3D12_BLEND_INV_ALPHA_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND AlphaBlend(uint32_t f)
{
    // Colour factors are invalid for alpha in D3D12: use their alpha forms.
    switch (f)
    {
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_ALPHA;
    case 9: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return Blend(f);
    }
}

D3D12_BLEND_OP BlendOp(uint32_t op)
{
    switch (op)
    {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
    }
}

// Persistent pipeline library: compiled GPU code survives between runs
// (Documents/fm2/pipelines.bin; keyed by the driver, so a driver update just misses).
struct PipelineLibrary
{
    ComPtr<ID3D12PipelineLibrary> library;
    std::atomic<int> stored{ 0 };
    std::mutex saveMutex;
    // Stores and loads share the library; serializing takes it alone. Serialize running
    // during a StorePipeline is not something the runtime promises to handle, and a cold
    // load with many pipelines building at once locked up (Mugello).
    std::shared_mutex access;
    std::chrono::steady_clock::time_point lastSave = std::chrono::steady_clock::now();
    std::vector<char> loaded;

    static std::filesystem::path Path() { return UserDir() / "pipelines.bin"; }

    void Init(ID3D12Device* device)
    {
        ComPtr<ID3D12Device1> d1;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d1))))
            return;
        if (std::ifstream in{ Path(), std::ios::binary | std::ios::ate })
        {
            loaded.resize(size_t(in.tellg()));
            in.seekg(0);
            in.read(loaded.data(), std::streamsize(loaded.size()));
        }
        if (FAILED(d1->CreatePipelineLibrary(loaded.data(), loaded.size(), IID_PPV_ARGS(&library))))
        {
            loaded.clear();
            if (FAILED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library))))
                library.Reset();
        }
        Log("renderer: pipeline library %s (%zu bytes loaded)", library ? "ready" : "unavailable", loaded.size());
    }

    // The shaders' compiled code is part of the name: the game's shader hashes alone stay the same
    // when the translation changes (a translator update, FM2_DYNAMIC_VFETCH), and a stale entry under
    // the name then failed to load and blocked storing the new one, rebuilding it every launch.
    static std::wstring Name(const PipelineKey& k, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd)
    {
        const uint64_t code = Fnv(pd.VS.pShaderBytecode, pd.VS.BytecodeLength, 3) * 0x9E3779B97F4A7C15ull ^
            (pd.PS.pShaderBytecode ? Fnv(pd.PS.pShaderBytecode, pd.PS.BytecodeLength, 4) : 0);
        wchar_t name[128];
        swprintf(name, 128, L"%016llx_%016llx_%08x_%08x_%08x_%08x_%016llx", (unsigned long long)k.vs, (unsigned long long)k.ps, k.blend, k.depth,
            k.stencilMaskMode, k.flags, (unsigned long long)code);
        return name;
    }

    HRESULT CreateOrLoad(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, const PipelineKey& key, ComPtr<ID3D12PipelineState>& pso, bool* hit)
    {
        *hit = false;
        if (library)
        {
            const std::wstring name = Name(key, pd);
            {
                std::shared_lock lock(access);
                if (SUCCEEDED(library->LoadGraphicsPipeline(name.c_str(), &pd, IID_PPV_ARGS(&pso))))
                {
                    *hit = true;
                    return S_OK;
                }
            }
            // The slow part (the driver compiling) runs outside the lock.
            const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
            if (SUCCEEDED(hr))
            {
                std::shared_lock lock(access);
                if (SUCCEEDED(library->StorePipeline(name.c_str(), pso.Get())))
                    stored++;
            }
            return hr;
        }
        return device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    }

    // Writes the library out when new pipelines were stored (at most every 10 s). Never on
    // the calling thread: serializing and writing the whole library (tens to hundreds of MB)
    // there stalled the command processor for one frame every 10 s of a race. force (exit)
    // writes synchronously.
    std::atomic<bool> saving{ false };

    void MaybeSave(bool force = false)
    {
        if (!library || !stored.load())
            return;
        if (force)
        {
            SaveNow();
            return;
        }
        if (std::chrono::steady_clock::now() - lastSave < std::chrono::seconds(10) || saving.exchange(true))
            return;
        lastSave = std::chrono::steady_clock::now();
        std::thread([this] {
            // Not lower: it holds the library alone while serializing, and starving it then
            // would stall every pipeline build.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SaveNow();
            saving = false;
        }).detach();
    }

    void SaveNow()
    {
        std::lock_guard lock(saveMutex);
        const int pending = stored.exchange(0);
        if (!pending)
            return;
        const auto start = std::chrono::steady_clock::now();
        std::vector<char> blob;
        {
            std::unique_lock lock(access);
            blob.resize(library->GetSerializedSize());
            if (blob.empty() || FAILED(library->Serialize(blob.data(), blob.size())))
            {
                stored += pending;
                return;
            }
        }
        const auto serialized = std::chrono::steady_clock::now();
        const auto tmp = Path().string() + ".tmp";
        { std::ofstream(tmp, std::ios::binary).write(blob.data(), std::streamsize(blob.size())); }
        std::error_code ec;
        std::filesystem::rename(tmp, Path(), ec);
        if (ec)
            std::filesystem::remove(tmp, ec);
        if (getenv("FM2_PERF"))
            Log("perf: pipeline library saved in the background (%.0f MB): serialize %.0f ms, write %.0f ms", blob.size() / 1048576.0,
                std::chrono::duration<double, std::milli>(serialized - start).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - serialized).count());
    }
} g_pipelineLibrary;

void InitPipelineLibrary(ID3D12Device* device)
{
    g_pipelineLibrary.Init(device);
    g_manifest.Load();
    ShaderScanStart();
}

struct PipelinePool
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;

    PipelinePool()
    {
        const unsigned n = std::clamp(std::thread::hardware_concurrency() * 3 / 4, 2u, 12u);
        for (unsigned i = 0; i < n; i++)
            std::thread([this] { Run(); }).detach();
    }
    void Push(std::function<void()> job)
    {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }
    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !queue.empty(); });
                job = std::move(queue.front());
                queue.pop_front();
            }
            job();
        }
    }
};

std::shared_ptr<PipelineEntry> EnsurePipeline(const CompiledShader& vs, const CompiledShader& ps, const PipelineKey& key)
{
    if (auto it = g_r.pipelines.find(key); it != g_r.pipelines.end())
        return it->second;
    const uint32_t blend = key.blend, depth = key.depth;
    const uint32_t mask = (key.stencilMaskMode >> 24) & 0xF, mode = (key.stencilMaskMode >> 28) & 7, stencil = key.stencilMaskMode & 0xFFFFFF;
    const bool alphaToMask = key.flags & 1, cullable = (key.flags >> 1) & 1;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_r.rootSignature.Get();
    pd.VS = { vs.code->GetBufferPointer(), vs.code->GetBufferSize() };
    pd.PS = { ps.code->GetBufferPointer(), ps.code->GetBufferSize() };
    auto& rt = pd.BlendState.RenderTarget[0];
    const uint32_t srcC = blend & 31, opC = (blend >> 5) & 7, dstC = (blend >> 8) & 31;
    const uint32_t srcA = (blend >> 16) & 31, opA = (blend >> 21) & 7, dstA = (blend >> 24) & 31;
    rt.BlendEnable = !(srcC == 1 && dstC == 0 && opC == 0 && srcA == 1 && dstA == 0 && opA == 0);
    rt.SrcBlend = Blend(srcC);
    rt.DestBlend = Blend(dstC);
    rt.BlendOp = BlendOp(opC);
    rt.SrcBlendAlpha = AlphaBlend(srcA);
    rt.DestBlendAlpha = AlphaBlend(dstA);
    rt.BlendOpAlpha = BlendOp(opA);
    rt.RenderTargetWriteMask = UINT8(mask);
    pd.BlendState.AlphaToCoverageEnable = alphaToMask; // one sample: coverage where alpha >= 0.5
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    // Polygon offset slope (MakePipelineKey): the scale is in 1/16 subpixel units, as in Xenia.
    pd.RasterizerState.SlopeScaledDepthBias = float(int16_t((key.flags >> 8) & 0xFFFF)) / 16.0f / 16.0f;
    pd.DepthStencilState.DepthEnable = (depth >> 1) & 1;
    pd.DepthStencilState.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(((depth >> 4) & 7) + 1);
    // Stencil: Xenos ops (the D3D9 order) 0 keep, 1 zero, 2 replace, 3 incr clamp, 4 decr clamp,
    // 5 invert, 6 incr wrap, 7 decr wrap. Back faces use their own ops when enabled.
    static const D3D12_STENCIL_OP kOps[8] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR_SAT,
        D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR };
    static LiveSwitch noStencil("FM2_NO_STENCIL");
    if ((depth & 1) && !noStencil)
    {
        auto& ds = pd.DepthStencilState;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = UINT8(stencil >> 8);
        ds.StencilWriteMask = UINT8(stencil >> 16);
        auto faceOps = [&](uint32_t shift) {
            D3D12_DEPTH_STENCILOP_DESC op{};
            op.StencilFunc = D3D12_COMPARISON_FUNC(((depth >> shift) & 7) + 1);
            op.StencilFailOp = kOps[(depth >> (shift + 3)) & 7];
            op.StencilPassOp = kOps[(depth >> (shift + 6)) & 7];
            op.StencilDepthFailOp = kOps[(depth >> (shift + 9)) & 7];
            return op;
        };
        ds.FrontFace = faceOps(8);
        ds.BackFace = (depth >> 7) & 1 ? faceOps(20) : ds.FrontFace;
    }
    // Culling: bit 0 front, bit 1 back; bit 2 set means clockwise faces are front.
    static LiveSwitch flip("FM2_CULL_FLIP");
    pd.RasterizerState.FrontCounterClockwise = ((mode & 4) == 0) != flip;
    // Only real triangle primitives keep the guest winding; generated geometry
    // (rectangle corners, point sprites) is never culled.
    static LiveSwitch noCull("FM2_NO_CULL"); // A/B
    pd.RasterizerState.CullMode = !cullable || noCull ? D3D12_CULL_MODE_NONE
        : (mode & 3) == 1 ? D3D12_CULL_MODE_FRONT : (mode & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kColorFormat;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    pd.SampleDesc.Count = g_msaa;
    const bool motion = key.flags & 4; // motion-vector variants: a second target for the motion
    if (motion)
    {
        pd.pRootSignature = g_r.motionRootSignature.Get();
        pd.NumRenderTargets = 2;
        pd.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        pd.BlendState.IndependentBlendEnable = TRUE;
        auto& mv = pd.BlendState.RenderTarget[1];
        mv = D3D12_RENDER_TARGET_BLEND_DESC{};
        // Blended draws (glass, smoke) keep the motion of what is behind them.
        mv.RenderTargetWriteMask = rt.BlendEnable ? 0 : D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
    }
    auto entry = std::make_shared<PipelineEntry>();
    g_r.pipelines[key] = entry;
    if (!motion && !IsPsEditAlias(key.ps) && !IsPsEditAlias(key.vs)) // the manifest rebuilds pipelines from the base shaders (not live edits)
    {
        g_manifest.RecordShader(false, key.vs, vs.ucodeBE);
        g_manifest.RecordShader(true, key.ps, ps.ucodeBE);
        g_manifest.RecordPipeline(key);
    }
    static PipelinePool pool;
    char label[64];
    snprintf(label, sizeof(label), "pipeline %08llx/%08llx", (unsigned long long)(key.vs >> 32), (unsigned long long)(key.ps >> 32));
    g_stats.Begin(label);
    pool.Push([entry, pd, vsKeep = vs.code, psKeep = ps.code, key]() mutable {
        const auto createStart = std::chrono::steady_clock::now();
        bool hit = false;
        const HRESULT hr = g_pipelineLibrary.CreateOrLoad(g_r.device, pd, key, entry->pso, &hit);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createStart).count();
        if (ms > 30.0 && getenv("FM2_PERF"))
            Log("perf: pipeline %016llx/%016llx took %.1f ms to create (background)", (unsigned long long)key.vs, (unsigned long long)key.ps, ms);
        if (FAILED(hr))
            Log("renderer: pipeline %016llx/%016llx failed", (unsigned long long)key.vs, (unsigned long long)key.ps);
        (hit ? g_stats.pipelinesLoaded : g_stats.pipelinesBuilt)++;
        entry->state.store(FAILED(hr) ? 2 : 1, std::memory_order_release);
        g_stats.End(!hit);
    });
    g_pipelinesCreated++;
    g_frameEvents.pipelines++;
    return entry;
}

void WaitHoldingImpl(const std::function<bool()>& done);

// Waits for the build (draws are never dropped: a missing pipeline used to leave
// shadow passes undrawn). Cold builds can take 100+ ms; the overlay says so.
ID3D12PipelineState* WaitForPipeline(const std::shared_ptr<PipelineEntry>& entry)
{
    static LiveSwitch async("FM2_ASYNC_PIPELINES");
    if (async && entry->state.load(std::memory_order_acquire) == 0)
        return nullptr;
    WaitHoldingImpl([&] { return entry->state.load(std::memory_order_acquire) != 0; });
    return entry->state.load() == 1 ? entry->pso.Get() : nullptr;
}

PipelineKey MakePipelineKey(uint64_t vsHash, uint64_t psHash, bool depthOnly, bool cullable)
{
    const uint32_t blend = gpureg::Get(RB_BLENDCONTROL0);
    uint32_t depth = gpureg::Get(RB_DEPTHCONTROL);
    // Debug: FM2_NO_ZTEST=a-b turns the depth test off for draws a..b-1 of the FM2_DUMP_FRAME frame.
    static uint32_t noZFrom = 0, noZTo = 0;
    static const bool noZParsed = [] { if (const char* s = getenv("FM2_NO_ZTEST")) sscanf(s, "%u-%u", &noZFrom, &noZTo); return true; }();
    static const uint64_t noZFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
    (void)noZParsed;
    if (noZTo && g_r.frame + 1 == noZFrame && g_r.drawsThisFrame >= noZFrom && g_r.drawsThisFrame < noZTo)
        depth &= ~(getenv("FM2_NO_ZWRITE") ? 4u : 2u);
    static LiveSwitch zFuncAlways("FM2_ZFUNC_ALWAYS");
    if (zFuncAlways)
        depth |= 0x70; // debug: depth func ALWAYS for every draw
    const uint32_t mask = depthOnly ? 0 : gpureg::Get(RB_COLOR_MASK) & 0xF;
    // Stencil masks (not the reference, which is dynamic) and culling are pipeline state.
    const uint32_t stencil = gpureg::Get(RB_STENCILREFMASK) & 0xFFFF00, mode = gpureg::Get(PA_SU_SC_MODE_CNTL) & 7;
    // Alpha-to-mask (RB_COLORCONTROL bit 4): cut-out foliage and fences.
    const bool alphaToMask = (gpureg::Get(RB_COLORCONTROL) >> 4) & 1;
    // Polygon offset's slope term (the constant one is in the vertex shader): the scale in 1/16-subpixel
    // units, kept in flags bits 8-23 (pipeline state). FM2_NO_POLY_OFFSET=1 leaves it out.
    uint32_t slope = 0;
    {
        static LiveSwitch noPolyOffset("FM2_NO_POLY_OFFSET");
        const uint32_t sc = gpureg::Get(PA_SU_SC_MODE_CNTL);
        if (!noPolyOffset && cullable && (sc & (3u << 11)))
        {
            const float scale = (sc >> 11) & 1 ? RegFloat(0x2380) : RegFloat(0x2382);
            slope = uint32_t(std::clamp(scale * 16.0f, -32768.0f, 32767.0f)) & 0xFFFF;
        }
    }
    return PipelineKey{ vsHash, psHash, blend, depth, stencil | (mask << 24) | (mode << 28), uint32_t(alphaToMask) | (uint32_t(cullable) << 1) | (slope << 8) };
}

// Alpha-blended colour draws (smoke, sparks, glows) are skippable for the moment it
// takes to build their pipeline; opaque and depth-only draws (shadow maps, geometry)
// are not: dropping those leaves holes or a black ground.
std::chrono::steady_clock::time_point g_lastLongHold{}; // end of the last hold of 2 s or more

// Once a loading hold has finished and the game is running (4 s later), new content that
// shows up first-time (other cars, effects at speed) builds in the background and the
// draw is skipped for the half second that takes, instead of freezing every frame.
bool GameSettled()
{
    static LiveSwitch never("FM2_NO_SETTLED_SKIP");
    return !never && g_lastLongHold.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() - g_lastLongHold > std::chrono::seconds(4);
}

bool SkippableDraw(bool depthOnly, uint32_t blend)
{
    static LiveSwitch noSkip("FM2_NO_SKIP_BLENDED");
    if (noSkip || depthOnly)
        return false;
    const bool opaque = (blend & 0xFF1F1F1F) == 0x00010001 || (blend & 0x1F) == 1 && ((blend >> 8) & 0x1F) == 0;
    return !opaque || GameSettled();
}

ID3D12PipelineState* GetPipeline(const CompiledShader& vs, const CompiledShader& ps, bool depthOnly, bool cullable, bool motion = false)
{
    PipelineKey key = MakePipelineKey(C().vsHash, C().psHash, depthOnly, cullable);
    if (motion)
        key.flags |= 4;
    thread_local std::unordered_map<PipelineKey, ID3D12PipelineState*, PipelineKeyHash> ready; // (pipelines are never removed)
    if (auto r = ready.find(key); r != ready.end())
        return r->second;
    std::shared_ptr<PipelineEntry> entry;
    {
        const auto lockStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::lock_guard shaderLock(g_shaderMutex);
        if (g_perfTiming)
            if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lockStart).count(); ms > 2.0)
                Log("perf: GetPipeline waited %.1f ms for the shader lock", ms);
        auto it = g_r.pipelines.find(key);
        if (it != g_r.pipelines.end() && it->second->state.load(std::memory_order_acquire) == 1)
        {
            ready.emplace(key, it->second->pso.Get());
            return it->second->pso.Get();
        }
        entry = EnsurePipeline(vs, ps, key);
    }
    if (SkippableDraw(depthOnly, key.blend) && entry->state.load(std::memory_order_acquire) == 0)
    {
        g_skippedPipeline++;
        return nullptr; // builds in the background; later frames draw it
    }
    g_waitWhat = depthOnly ? "pipeline (depth only)" : "pipeline";
    g_waitBlend = key.blend;
    const auto waitStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ID3D12PipelineState* pso = WaitForPipeline(entry);
    if (g_perfTiming)
        if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count(); ms > 2.0)
            Log("perf: GetPipeline waited %.1f ms for pipeline %016llx/%016llx (state was %d)", ms, (unsigned long long)key.vs, (unsigned long long)key.ps, 0);
    return pso;
}

// Cube textures: six faces, each a padded 2D slice. Faces the title rendered
// and resolved (dynamic reflections) are copied from their host textures every
// frame; static faces are decoded once and re-decoded when their data changes.
ID3D12Resource* GetCubeTexture(const TextureFetch& fetch, DXGI_FORMAT& format)
{
    TextureScope textureScope;
    const uint32_t w = fetch.Width(), h = fetch.Height();
    if (w != h || w > 2048)
        return nullptr;
    const uint32_t stride = TextureFaceStride(fetch);
    CachedTexture& cached = g_r.cubes[TextureKey(fetch.Address(), fetch.Format(), w, h)];
    if (cached.resource && cached.checkedFrame == g_r.frame)
    {
        format = cached.format;
        return cached.resource.Get();
    }
    cached.checkedFrame = g_r.frame;

    // Faces the title resolved into?
    const ResolvedTexture* faces[6]{};
    bool resolved = false;
    for (uint32_t i = 0; i < 6; i++)
    {
        auto it = g_r.resolved.find((fetch.Address() + i * stride) & 0x1FFFFFFF);
        if (it != g_r.resolved.end() && it->second.format != DXGI_FORMAT_R32_FLOAT && it->second.width >= w && it->second.height >= h)
        {
            faces[i] = &it->second;
            resolved = true;
        }
    }
    DXGI_FORMAT want = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t i = 0; i < 6 && resolved; i++)
        if (faces[i])
        {
            want = faces[i]->format;
            break;
        }
    // Faces the title rendered are at the render scale; the cube matches them (their own host
    // size: the current pass's scale differs when the cube is first fetched in another pass,
    // which rebuilt the cube mid-race, a 4-5 ms draw).
    uint32_t cw = w, ch = h;
    for (uint32_t i = 0; i < 6 && resolved; i++)
        if (faces[i] && faces[i]->format == want && faces[i]->hostWidth && faces[i]->width)
        {
            cw = uint32_t(std::lround(double(w) * faces[i]->hostWidth / faces[i]->width));
            ch = uint32_t(std::lround(double(h) * faces[i]->hostHeight / faces[i]->height));
            break;
        }
    if (!cached.resource || cached.width != cw || cached.height != ch || cached.format != want)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = cw;
        td.Height = ch;
        td.DepthOrArraySize = 6;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = want;
        if (cached.resource)
            Frame().garbage.push_back(std::move(cached.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                  IID_PPV_ARGS(&cached.resource)),
            "CreateCommittedResource(cube)"); CountCreate("cube");
        cached.width = cw;
        cached.height = ch;
        cached.format = want;
        cached.contentHash = 0;
    }
    format = want;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cached.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    if (resolved)
    {
        D3D12_RESOURCE_BARRIER fb[6]{};
        UINT n = 0;
        for (uint32_t i = 0; i < 6; i++)
        {
            if (!faces[i] || faces[i]->format != want)
                continue;
            fb[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fb[n].Transition.pResource = faces[i]->resource.Get();
            fb[n].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            fb[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            n++;
        }
        C().list->ResourceBarrier(1, &b);
        if (n)
            C().list->ResourceBarrier(n, fb);
        for (uint32_t i = 0; i < 6; i++)
        {
            if (!faces[i] || faces[i]->format != want)
                continue;
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = cached.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            src.pResource = faces[i]->resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{ 0, 0, 0, std::min(cw, faces[i]->hostWidth), std::min(ch, faces[i]->hostHeight), 1 };
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        for (UINT k = 0; k < n; k++)
            std::swap(fb[k].Transition.StateBefore, fb[k].Transition.StateAfter);
        if (n)
            C().list->ResourceBarrier(n, fb);
    }
    else
    {
        // Static: hash all six faces, decode on change.
        const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
        const uint64_t hash = ContentHash(source, size_t(stride) * 6) ^ (uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Endian()) << 44);
        if (cached.contentHash == hash)
            return cached.resource.Get();
        const UINT rowPitch = (w * 4 + 255) & ~255u;
        const UINT64 faceBytes = (UINT64(rowPitch) * h + 511) & ~511ull;
        const UINT64 offset = RingAlloc(faceBytes * 6, 512);
        if (offset == ~0ull)
            return nullptr;
        C().list->ResourceBarrier(1, &b);
        for (uint32_t i = 0; i < 6; i++)
        {
            TextureFetch face = fetch;
            face.dwords[1] = (face.dwords[1] & 0xFFF) | ((fetch.Address() + i * stride) & 0xFFFFF000);
            face.dwords[5] = (face.dwords[5] & ~(3u << 9)) | (1u << 9); // decode as 2D
            std::vector<uint8_t> rgba;
            if (!DecodeTexture(face, rgba))
                rgba.assign(size_t(w) * h * 4, 0);
            if (getenv("FM2_DUMP_CUBES"))
            {
                char name[80];
                std::filesystem::create_directories("out/cube");
                snprintf(name, sizeof(name), "out/cube/%08X_face%u_stride%X.png", fetch.Address(), i, stride);
                WritePng(name, w, h, rgba);
            }
            for (uint32_t y = 0; y < h; y++)
                memcpy(Frame().ringCpu + offset + faceBytes * i + UINT64(y) * rowPitch, &rgba[size_t(y) * w * 4], size_t(w) * 4);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = cached.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            src.pResource = Frame().ring.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = offset + faceBytes * i;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, rowPitch };
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        cached.contentHash = hash;
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    return cached.resource.Get();
}

// A render target the title resolved together with its mip chain (it
// downsamples and resolves each level itself): levels 1+ follow each other
// from the fetch constant's mip address, each padded to 32x32 texels and
// 4 KB. Assembled into one mipmapped host texture every frame it is used.
ID3D12Resource* ResolvedMipChain(const TextureFetch& fetch, const ResolvedTexture& level0)
{
    TextureScope textureScope;
    const uint32_t w = std::min(fetch.Width(), level0.width), h = std::min(fetch.Height(), level0.height);
    const UINT bpp = level0.format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
    std::vector<const ResolvedTexture*> levels{ &level0 };
    uint32_t address = fetch.MipAddress();
    for (uint32_t i = 1; i <= fetch.MipMaxLevel() && (w >> i || h >> i); i++)
    {
        const uint32_t lw = std::max(w >> i, 1u), lh = std::max(h >> i, 1u);
        auto it = g_r.resolved.find(address & 0x1FFFFFFF);
        if (it == g_r.resolved.end() || it->second.format != level0.format || it->second.width < lw || it->second.height < lh)
            break;
        levels.push_back(&it->second);
        const uint32_t bytes = ((lw + 31) & ~31u) * ((lh + 31) & ~31u) * bpp;
        address += std::max((bytes + 4095) & ~4095u, 4096u);
    }
    if (levels.size() < 2)
        return nullptr;
    // The levels are host textures (scaled with the render resolution): the chain is too.
    // (At game size it held each level's top-left corner only: Laguna Seca's track-wide
    // shadow map read from the wrong place, black patches on the road.)
    const uint32_t hw = std::max(uint32_t(uint64_t(level0.hostWidth) * w / std::max(level0.width, 1u)), 1u);
    const uint32_t hh = std::max(uint32_t(uint64_t(level0.hostHeight) * h / std::max(level0.height, 1u)), 1u);

    auto& chain = g_r.mipChains[fetch.Address()];
    if (chain.resource && chain.checkedFrame == g_r.frame)
        return chain.resource.Get();
    chain.checkedFrame = g_r.frame;
    const UINT count = UINT(levels.size());
    if (!chain.resource || chain.width != hw || chain.height != hh || chain.format != level0.format ||
        chain.resource->GetDesc().MipLevels != count)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = hw;
        td.Height = hh;
        td.DepthOrArraySize = 1;
        td.MipLevels = UINT16(count);
        td.SampleDesc.Count = 1;
        td.Format = level0.format;
        if (chain.resource)
            Frame().garbage.push_back(std::move(chain.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                  IID_PPV_ARGS(&chain.resource)),
            "CreateCommittedResource(mip chain)"); CountCreate("mip chain");
        chain.width = hw;
        chain.height = hh;
        chain.format = level0.format;
    }
    std::vector<D3D12_RESOURCE_BARRIER> b(count + 1);
    for (UINT i = 0; i <= count; i++)
    {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = i < count ? levels[i]->resource.Get() : chain.resource.Get();
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[i].Transition.StateAfter = i < count ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    }
    // Several levels can share one host texture only if the title resolved
    // them to the same address, which it does not; still, transition each once.
    C().list->ResourceBarrier(UINT(b.size()), b.data());
    for (UINT i = 0; i < count; i++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = chain.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        src.pResource = levels[i]->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{ 0, 0, 0, std::min(std::max(hw >> i, 1u), levels[i]->hostWidth), std::min(std::max(hh >> i, 1u), levels[i]->hostHeight), 1 };
        C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    for (auto& x : b)
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    C().list->ResourceBarrier(UINT(b.size()), b.data());
    return chain.resource.Get();
}

// The resolved texture a fetch reads, if any. Resolves are never block compressed,
// so a compressed fetch at an address we resolved to earlier is a different image the
// title has since loaded there (menu thumbnails reusing a shadow blob's memory).
// A hash of the guest memory a resolved image covers (sampled: it only has to notice new contents).
// (Only the image's own bytes: an 8-bit car shadow hashed as 32-bit took in the memory after it,
// which changes when parts are fitted, and the shadow fell back to that memory as noise.)
uint64_t GuestImageHash(uint32_t base, uint32_t width, uint32_t height, uint32_t bpp)
{
    const size_t bytes = std::min<size_t>(size_t(width) * height * bpp, 4u << 20);
    return ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(base)), std::max<size_t>(bytes, 64), 256);
}

// Records what guest memory holds under a resolved image (after a resolve, or after we write it back).
void NoteResolvedMemory(uint32_t base, ResolvedTexture& r)
{
    r.memHash = GuestImageHash(base, r.width, r.height, r.guestBpp);
    r.memChecked = g_r.frame;
    r.stale = false;
}

const ResolvedTexture* ResolvedFor(const TextureFetch& fetch)
{
    const char* name = TextureFormatName(fetch.Format());
    if (name && (strncmp(name, "DXT", 3) == 0 || strncmp(name, "DXN", 3) == 0 || strncmp(name, "CTX", 3) == 0))
        return nullptr;
    auto it = g_r.resolved.find(fetch.Address() & 0x1FFFFFFF);
    if (it == g_r.resolved.end())
        return nullptr;
    // A resolve not repeated lately whose memory has changed since: the title reused the address for a
    // texture of its own (the event screens' eligibility icons sat on an old menu resolve: white boxes).
    // Checked every 32 frames; a new resolve there makes it current again.
    LiveSwitch &noStaleCheck = g_sw_FM2_NO_STALE_RESOLVES; // A/B
    ResolvedTexture& r = it->second;
    if (!noStaleCheck && r.frame + 8 < g_r.frame && r.frame != ~0ull)
    {
        if (!r.stale && g_r.frame - r.memChecked >= 32)
        {
            r.memChecked = g_r.frame;
            if (GuestImageHash(it->first, r.width, r.height, r.guestBpp) != r.memHash)
            {
                r.stale = true;
                Log("renderer: resolve at %08X (%ux%u) is stale: the memory was reused", it->first, r.width, r.height);
            }
        }
        if (r.stale)
            return nullptr;
    }
    return &r;
}

// The guest bytes a texture check hashes, and how densely.
void TextureHashSpan(const TextureFetch& fetch, uint32_t w, uint32_t h, const uint8_t*& source, size_t& bytes, size_t& stride)
{
    source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
    // The whole base level: the old guess (pitch * height / 2) covered only the first half of a
    // one-byte-per-texel texture (DXT5), so a thumbnail filled in later went unnoticed.
    bytes = std::max<size_t>({ size_t(std::max(fetch.Pitch(), w)) * h / 2, TextureSourceSize(fetch), 64 });
    // Only what changes the decoded image: sampler state (clamp, filter) varies per draw.
    // Small textures (icons, thumbnails) are hashed densely, big ones sampled: a change confined
    // to a few dwords must not slip between the samples.
    stride = bytes <= (64u << 10) ? 4 : bytes <= (512u << 10) ? 64 : 256;
}

// Decodes (if changed) and uploads a texture; returns its resource.
ID3D12Resource* GetTextureLocked(const TextureFetch& fetch, DXGI_FORMAT& format, bool& native);
// Textures are checked and uploaded once per frame; each thread remembers what it got this
// frame, so repeated bindings take no lock.
ID3D12Resource* GetTexture(const TextureFetch& fetch, DXGI_FORMAT& format, bool& native)
{
    struct Known
    {
        ID3D12Resource* resource;
        DXGI_FORMAT format;
        bool native;
    };
    thread_local FrameMap<Known> known;
    // (Resolved textures can change within a frame: not cached.)
    const bool cacheable = !ResolvedFor(fetch) && !RendererTracing();
    const uint64_t key = TextureKey(fetch.Address(), fetch.Format(), fetch.Width(), fetch.Height()) ^ uint64_t(fetch.dwords[0]) << 7 ^
        uint64_t(fetch.dwords[3] & 0xFFF) << 41 ^ uint64_t(fetch.dwords[1] & 0xC0) << 50;
    if (cacheable)
        if (const Known* k = known.Find(key, g_r.frame))
        {
            format = k->format;
            native = k->native;
            return k->resource;
        }
    ID3D12Resource* r = GetTextureLocked(fetch, format, native);
    if (cacheable && r)
        known.Put(key, g_r.frame, { r, format, native });
    return r;
}

ID3D12Resource* GetTextureLocked(const TextureFetch& fetch, DXGI_FORMAT& format, bool& native)
{
    TextureScope textureScope;
    native = false;
    format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (const ResolvedTexture* resolvedTexture = ResolvedFor(fetch))
    {
        // The full-screen scene image rendered this frame: the draw is a screen layer (the
        // post-processing composite, a grid of rectangles). HUD elements sample only atlases
        // and reduced-size buffers (a 320x180 blur of the scene behind their panels).
        // (By the size the draw fetches it at: the HUD samples the scene image too, at 320x180.)
        if (resolvedTexture->frame == g_r.frame && fetch.Width() >= kWidth)
            g_drawSamplesResolve = true;
        format = resolvedTexture->format;
        if (fetch.MipMaxLevel() > 0 && fetch.MipAddress())
            if (ID3D12Resource* chain = ResolvedMipChain(fetch, *resolvedTexture))
                return chain;
        return resolvedTexture->resource.Get();
    }
    if (fetch.Type() != 2 || fetch.Dimension() != 1 || !TextureFormatSupported(fetch.Format()))
    {
        NoteTextureFailure("not a supported 2D texture", fetch);
        return nullptr;
    }
    const uint32_t w = fetch.Width(), h = fetch.Height();
    auto& cached = g_r.textures[TextureKey(fetch.Address(), fetch.Format(), w, h)];
    LiveSwitch &dumpAll = g_sw_FM2_DUMP_TEXTURES;
    const bool forceDecode = dumpAll && RendererTracing();
    if (cached.resource && cached.checkedFrame == g_r.frame && !forceDecode)
    {
        format = cached.format;
        native = cached.native;
        return cached.resource.Get();
    }
    // A texture unchanged for many checks (most of them: loaded once) is checked every 4th
    // frame (staggered), not every frame: hashing all textures in use cost ~0.5 ms a frame.
    // Ones that change (video, render-to-texture on the CPU) never get here.
    LiveSwitch &checkAlways = g_sw_FM2_CHECK_TEXTURES_ALWAYS; // A/B
    const uint64_t textureKey = TextureKey(fetch.Address(), fetch.Format(), w, h);
    // Unchanged for 300 checks (a few seconds): every 16th frame (a CPU-side update shows up at most
    // ~0.1 s late). FM2_TEXTURE_CHECK_EVERY4=1 keeps every 4th (A/B).
    LiveSwitch &every4 = g_sw_FM2_TEXTURE_CHECK_EVERY4;
    const uint64_t checkMask = !every4 && cached.unchanged >= 300 ? 15 : 3;
    if (cached.resource && !checkAlways && !forceDecode && cached.unchanged >= 30 && ((g_r.frame + (textureKey >> 32)) & checkMask) != 0)
    {
        if (((g_r.frame + 1 + (textureKey >> 32)) & checkMask) == 0) // due next frame: hashed ahead
        {
            const uint8_t* source;
            size_t bytes, stride;
            TextureHashSpan(fetch, w, h, source, bytes, stride);
            prehash::Ahead(textureKey, source, bytes, stride);
        }
        cached.checkedFrame = g_r.frame;
        format = cached.format;
        native = cached.native;
        return cached.resource.Get();
    }
    cached.checkedFrame = g_r.frame;
    // DXT formats stay compressed on the GPU (BC1-BC4): no decode, a quarter to an eighth of
    // the upload. D3D12 needs block-aligned sizes; others are decoded to RGBA8.
    LiveSwitch &noNativeBC = g_sw_FM2_NO_NATIVE_BC;
    const uint32_t bc = !noNativeBC && w % 4 == 0 && h % 4 == 0 && !forceDecode ? TextureBCKind(fetch.Format()) : 0;
    const uint8_t* source;
    size_t sourceBytes, hashStride;
    TextureHashSpan(fetch, w, h, source, sourceBytes, hashStride);
    const uint64_t hash = prehash::Hash(textureKey, source, sourceBytes, hashStride) ^ (bc ? 0 : uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Pitch()) << 24) ^
        (uint64_t(fetch.Tiled()) << 40) ^ (uint64_t(fetch.Endian()) << 44);
    if (cached.resource && cached.contentHash == hash && !forceDecode)
    {
        cached.unchanged++;
        format = cached.format;
        native = cached.native;
        return cached.resource.Get();
    }
    const auto uploadStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool wasNew = !cached.resource;
    cached.unchanged = 0;
    struct UploadLog
    {
        std::chrono::steady_clock::time_point start;
        const TextureFetch& fetch;
        bool isNew;
        ~UploadLog()
        {
            if (!g_perfTiming)
                return;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms > 2.0)
                Log("perf: texture %08X %s %ux%u %s took %.1f ms", fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height(),
                    isNew ? "new" : "changed", ms);
        }
    } uploadLog{ uploadStart, fetch, wasNew };
    // The title's own mip levels (distant surfaces sample the smaller, averaged ones: without
    // them the road and grass shimmer and look grainy). Down to the packed tail.
    LiveSwitch &noMips = g_sw_FM2_NO_MIPS; // A/B
    std::vector<TextureFetch> levels{ fetch };
    for (uint32_t l = 1; !noMips && !forceDecode; l++)
    {
        TextureFetch level;
        if (!TextureMipLevel(fetch, l, level))
            break;
        levels.push_back(level);
    }
    const UINT16 mipCount = UINT16(levels.size());
    // Diagnostic: textures the title gives mip levels that get none here (they shimmer and look
    // grainy in the distance), once each.
    if (mipCount == 1 && fetch.MipMaxLevel() > 0 && !noMips && !forceDecode)
    {
        static std::unordered_set<uint64_t> reported;
        if (reported.insert(textureKey).second && reported.size() <= 200)
        {
            const bool pow2 = !(w & (w - 1)) && !(h & (h - 1));
            Log("mips: %08X %s %ux%u has %u levels but none uploaded: %s", fetch.Address(), TextureFormatName(fetch.Format()), w, h,
                fetch.MipMaxLevel(), !fetch.MipAddress() ? "no mip address" : !fetch.Tiled() ? "not tiled" : !pow2 ? "not a power of two" : "level 1 is in the packed tail");
        }
    }
    if (bc)
    {
        static const DXGI_FORMAT bcFormats[] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM,
            DXGI_FORMAT_BC4_UNORM };
        const DXGI_FORMAT bcFormat = bcFormats[bc];
        const uint32_t blockBytes = bc == 1 || bc == 4 ? 8 : 16;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!cached.resource || cached.width != w || cached.height != h || cached.format != bcFormat || cached.resource->GetDesc().MipLevels != mipCount)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w;
            td.Height = h;
            td.DepthOrArraySize = 1;
            td.MipLevels = mipCount;
            td.SampleDesc.Count = 1;
            td.Format = bcFormat;
            RetireTexture(std::move(cached.resource));
            cached.resource = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_DEST, "texture");
            cached.width = w;
            cached.height = h;
            cached.format = bcFormat;
        }
        else
        {
            b.Transition.pResource = cached.resource.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            C().list->ResourceBarrier(1, &b);
        }
        for (UINT16 l = 0; l < mipCount; l++)
        {
            const uint32_t lw = levels[l].Width(), lh = levels[l].Height();
            const UINT rowPitch = (((lw + 3) / 4) * blockBytes + 255) & ~255u;
            const UINT64 offset = RingAlloc(UINT64(rowPitch) * ((lh + 3) / 4), 512);
            if (offset == ~0ull)
            {
                NoteTextureFailure("upload ring full (compressed)", fetch);
                return nullptr;
            }
            CopyTextureBlocks(levels[l], Frame().ringCpu + offset, rowPitch);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = cached.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = l;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = Frame().ring.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = offset;
            src.PlacedFootprint.Footprint = { bcFormat, (lw + 3) & ~3u, (lh + 3) & ~3u, 1, rowPitch };
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        b.Transition.pResource = cached.resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        C().list->ResourceBarrier(1, &b);
        cached.contentHash = hash;
        cached.native = true;
        format = bcFormat;
        native = true;
        return cached.resource.Get();
    }

    std::vector<uint8_t> rgba;
    if (!DecodeTexture(fetch, rgba))
    {
        NoteTextureFailure("decode failed", fetch);
        return nullptr;
    }
    if (RendererTracing())
        Log("texture upload: %08X %s %ux%u (%s)", fetch.Address(), TextureFormatName(fetch.Format()), w, h, cached.resource ? "changed" : "new");
    static const uint32_t dumpAddress = getenv("FM2_DUMP_TEXTURE") ? uint32_t(strtoul(getenv("FM2_DUMP_TEXTURE"), nullptr, 16)) : 0;
    if (RendererTracing() && getenv("FM2_DUMP_TEXTURES"))
    {
        char name[64];
        snprintf(name, sizeof(name), "out/tex/%08X_%s_%ux%u.png", fetch.Address(), TextureFormatName(fetch.Format()), w, h);
        std::filesystem::create_directories("out/tex");
        WritePng(name, w, h, rgba);
    }
    if (dumpAddress && fetch.Address() == dumpAddress)
    {
        WritePng("texture.png", w, h, rgba);
        Log("renderer: wrote texture.png (%s %ux%u, endian %u, tiled %u, pitch %u)", TextureFormatName(fetch.Format()), w, h, fetch.Endian(),
            fetch.Tiled(), fetch.Pitch());
    }
    if (!cached.resource || cached.width != w || cached.height != h || cached.format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        cached.resource->GetDesc().MipLevels != mipCount)
    {
        cached.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cached.native = false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = mipCount;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        RetireTexture(std::move(cached.resource));
        cached.resource = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_DEST, "texture");
        cached.width = w;
        cached.height = h;
    }
    else
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = cached.resource.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        C().list->ResourceBarrier(1, &b);
    }
    for (UINT16 l = 0; l < mipCount; l++)
    {
        if (l > 0 && !DecodeTexture(levels[l], rgba))
        {
            NoteTextureFailure("mip decode failed", fetch);
            return nullptr;
        }
        const uint32_t lw = levels[l].Width(), lh = levels[l].Height();
        const UINT rowPitch = (lw * 4 + 255) & ~255u;
        const UINT64 offset = RingAlloc(UINT64(rowPitch) * lh, 512);
        if (offset == ~0ull)
        {
            NoteTextureFailure("upload ring full", fetch);
            return nullptr;
        }
        for (uint32_t y = 0; y < lh; y++)
            memcpy(Frame().ringCpu + offset + UINT64(y) * rowPitch, &rgba[size_t(y) * lw * 4], size_t(lw) * 4);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = cached.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = l;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = Frame().ring.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, lw, lh, 1, rowPitch };
        C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cached.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    C().list->ResourceBarrier(1, &b);
    cached.contentHash = hash;
    return cached.resource.Get();
}

// F12 capture: every distinct texture a draw of the traced frame binds goes into
// out/capture/textures.txt with its decoded image in out/capture/tex/, and a texture that could not
// be bound (the draw samples the white fallback) is named in out/play.log with the reason.
void CaptureTexture(const TextureFetch& fetch, uint32_t slot, bool cube, bool bound, uint32_t index, bool native, const char* why)
{
    static uint64_t captureFrame = 0;
    static std::set<uint64_t> seen;
    static std::ofstream summary;
    const uint64_t frame = TraceFrame();
    if (frame != captureFrame)
    {
        captureFrame = frame;
        seen.clear();
        std::filesystem::create_directories("out/capture/tex");
        std::filesystem::remove_all("out/capture/mem");
        summary.close();
        summary.open("out/capture/textures.txt");
        summary << "frame " << frame << " (draw numbers are those of the 'draw #N' lines in out/play.log)\n";
        summary << "draw slot address format size pitch tiled swizzle: status [image]\n";
    }
    const bool resolved = ResolvedFor(fetch) != nullptr;
    if (!bound && !cube)
        Log("    texture t%u %08X %s %ux%u is not bound: the draw samples white (%s)", slot, fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(),
            fetch.Height(), why[0] ? why : "unknown");
    const uint64_t key = (uint64_t(fetch.Address()) << 24) ^ (uint64_t(fetch.Format()) << 16) ^ (uint64_t(fetch.Width()) << 4) ^ fetch.Height() ^
        (uint64_t(cube) << 62);
    if (!seen.insert(key).second)
        return;
    char status[160];
    if (!bound)
        snprintf(status, sizeof(status), "WHITE FALLBACK: %s", why[0] ? why : "unknown");
    else if (resolved)
        snprintf(status, sizeof(status), "a resolved render target (out/res)");
    else
        snprintf(status, sizeof(status), "bound%s, bindless %u", native ? " (compressed on the GPU)" : "", index);
    std::string image;
    std::vector<uint8_t> rgba;
    if (!cube && !resolved && fetch.Type() == 2 && fetch.Dimension() == 1 && TextureFormatSupported(fetch.Format()) && DecodeTexture(fetch, rgba))
    {
        char name[160];
        snprintf(name, sizeof(name), "out/capture/tex/%08X_%s_%ux%u.png", fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height());
        if (WritePng(name, fetch.Width(), fetch.Height(), rgba))
            image = name;
    }
    // The raw bytes of the base level as the game left them (out/capture/mem/), to look at a texture the
    // decode shows as noise: wrong layout, or not written yet.
    if (!cube && !resolved && fetch.Type() == 2)
    {
        const size_t bytes = std::min<size_t>(TextureSourceSize(fetch), 16u << 20);
        if (bytes)
        {
            std::filesystem::create_directories("out/capture/mem");
            char name[160];
            snprintf(name, sizeof(name), "out/capture/mem/%08X_%s_%ux%u.bin", fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height());
            std::ofstream raw(name, std::ios::binary);
            raw.write(reinterpret_cast<const char*>(GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()))), std::streamsize(bytes));
        }
    }
    char line[400];
    snprintf(line, sizeof(line), "%u t%u %08X %s %ux%u pitch %u %s sw%03X: %s %s%s\n", g_r.drawsThisFrame.load(), slot, fetch.Address(),
        TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height(), fetch.Pitch(), fetch.Tiled() ? "tiled" : "linear", fetch.Swizzle(), status,
        cube ? "(cube map) " : "", image.c_str());
    summary << line;
    summary.flush();
}

uint32_t ReadIndex(uint32_t base, uint32_t i, bool is32, uint32_t endian)
{
    if (is32)
    {
        uint32_t v = *GuestPtr<uint32_t>(base + i * 4);
        if (endian == 2)
            v = __builtin_bswap32(v);
        else if (endian == 1)
            v = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
        return v;
    }
    uint16_t v = *GuestPtr<uint16_t>(base + i * 2);
    if (endian == 1 || endian == 2)
        v = __builtin_bswap16(v);
    return v;
}

// Expanded host vertex list: guest indices, or rectangle markers
// (bit 31 | role << 29 | list position of the rectangle's first vertex).
bool BuildIndexList(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize, std::vector<uint32_t>& out)
{
    const uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    const bool is32 = (initiator >> 11) & 1;
    const uint32_t offset = gpureg::Get(VGT_INDX_OFFSET);
    // Reused between draws (no allocation per draw).
    thread_local std::vector<uint32_t> g;
    thread_local std::vector<uint8_t> cut;
    g.resize(count);
    cut.clear();
    // Primitive restart: a raw index equal to the reset value ends the strip/fan.
    const bool restart = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    const uint32_t resetIndex = gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX) & (is32 ? 0xFFFFFF : 0xFFFF);
    if (source == 0) // DMA
    {
        const uint32_t base = GpuPhysToGuest(dmaBase & ~(is32 ? 3u : 1u));
        const uint32_t endian = dmaSize >> 30;
        const uint32_t available = (dmaSize & 0xFFFFFF) * (is32 ? 1 : 2);
        for (uint32_t i = 0; i < count && i < available; i++)
        {
            const uint32_t raw = ReadIndex(base, i, is32, endian);
            if (restart && (raw & (is32 ? 0xFFFFFF : 0xFFFF)) == resetIndex)
            {
                cut.resize(count);
                cut[i] = 1;
            }
            g[i] = raw + offset;
        }
    }
    else if (source == 2) // auto
    {
        for (uint32_t i = 0; i < count; i++)
            g[i] = i + offset;
    }
    else
        return false;

    out.clear();
    switch (prim)
    {
    case 1: // point list: a quad per point, corner in bits 29-30
    {
        out.resize(size_t(count) * 6);
        uint32_t* o = out.data();
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t corner : { 0u, 1u, 2u, 2u, 1u, 3u })
                *o++ = (corner << 29) | (g[i] & 0x1FFFFFFF);
        return true;
    }
    case 4: // triangle list
        out.assign(g.begin(), g.begin() + (count / 3) * 3);
        return true;
    case 6: // triangle strip(s), split at restart indices
    case 5: // triangle fan(s)
    {
        out.resize(count >= 3 ? size_t(count - 2) * 3 : 0); // upper bound
        uint32_t* o = out.data();
        for (uint32_t start = 0; start < count;)
        {
            uint32_t end = start;
            if (cut.empty())
                end = count;
            else
                while (end < count && !cut[end])
                    end++;
            for (uint32_t i = start; i + 2 < end; i++)
            {
                const uint32_t k = i - start;
                if (prim == 5)
                    o[0] = g[start], o[1] = g[i + 1], o[2] = g[i + 2];
                else if (k & 1)
                    o[0] = g[i + 1], o[1] = g[i], o[2] = g[i + 2];
                else
                    o[0] = g[i], o[1] = g[i + 1], o[2] = g[i + 2];
                o += 3;
            }
            start = end + 1;
        }
        out.resize(size_t(o - out.data()));
        return true;
    }
    case 13: // quad list
    {
        out.resize(size_t(count / 4) * 6);
        uint32_t* o = out.data();
        for (uint32_t i = 0; i + 3 < count; i += 4, o += 6)
            o[0] = g[i], o[1] = g[i + 1], o[2] = g[i + 2], o[3] = g[i], o[4] = g[i + 2], o[5] = g[i + 3];
        return true;
    }
    case 8: // rectangle list: first triangle as given, second from the
            // hypotenuse ends and the computed fourth corner
        for (uint32_t i = 0; i + 2 < count; i += 3)
        {
            const uint32_t pos = uint32_t(out.size());
            out.insert(out.end(), { g[i], g[i + 1], g[i + 2] });
            for (uint32_t role : { 1u, 2u, 0u })
                out.push_back(0x80000000u | (role << 29) | pos);
        }
        return true;
    default:
        return false;
    }
}
}

namespace
{
// Expanded index lists (strips with restarts, fans, points, rectangles), kept across frames
// and keyed by the draw's index parameters and a sampled hash of the guest indices (like
// index buffers); each is copied into the ring once per frame.
struct IndexListEntry
{
    bool valid = false, ok = false;
    uint64_t sample = 0;
    std::vector<uint32_t> list;
    uint64_t frame = ~0ull; // the frame `offset` belongs to
    UINT64 offset = 0;
};

IndexListEntry* CachedIndexList(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    const uint32_t source = (initiator >> 6) & 3, count = initiator >> 16;
    const bool is32 = (initiator >> 11) & 1;
    uint64_t key = 0xCBF29CE484222325ull;
    for (uint32_t v : { initiator, dmaBase, dmaSize, gpureg::Get(VGT_INDX_OFFSET), gpureg::Get(PA_SU_SC_MODE_CNTL) & (1u << 21),
             gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX) })
        key = (key ^ v) * 0x100000001B3ull;
    uint64_t sample = 0;
    if (source == 0)
    {
        const uint32_t available = (dmaSize & 0xFFFFFF) * (is32 ? 1 : 2);
        const uint32_t bytes = std::min(count, available) * (is32 ? 4 : 2);
        sample = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(dmaBase & ~(is32 ? 3u : 1u))), bytes);
    }
    thread_local std::unordered_map<uint64_t, IndexListEntry> cache;
    if (cache.size() > 8192)
        cache.clear();
    IndexListEntry& e = cache[key];
    if (!e.valid || e.sample != sample)
    {
        e.valid = true;
        e.sample = sample;
        e.frame = ~0ull;
        e.ok = BuildIndexList(initiator, dmaBase, dmaSize, e.list);
    }
    return e.ok ? &e : nullptr;
}

// Origin (screen rows) and screen rectangle of the tile pass being replayed.
struct TileState
{
    int originY = 0;
    D3D12_RECT rect{ 0, 0, 1 << 14, 1 << 14 };
    uint32_t surface = ~0u; // RB_SURFACE_INFO of the tiled pass (~0: any)
};
thread_local TileState g_tile;
static LiveSwitch g_noForeignTile("FM2_NO_FOREIGN_TILE"); // A/B for the paint-editor tile fix

// Tiling: each tile pass is shifted into EDRAM by the window offset and
// clipped to the tile. We render in screen space, so draws (and resolves)
// stay unshifted and are clipped to the tile's screen rectangle.
D3D12_RECT WindowScissor()
{
    const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
    const int32_t wx = int32_t(offsetReg << 17) >> 17, wy = int32_t((offsetReg >> 16) << 17) >> 17;
    const bool vertexOffset = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1;
    const uint32_t tl = gpureg::Get(PA_SC_WINDOW_SCISSOR_TL), br = gpureg::Get(PA_SC_WINDOW_SCISSOR_BR);
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF), x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!(tl >> 31))
    {
        x0 += wx; x1 += wx; y0 += wy; y1 += wy; // to EDRAM space
    }
    if (vertexOffset)
    {
        x0 -= wx; x1 -= wx; y0 -= wy; y1 -= wy; // back to screen space
    }
    // Taller than the screen on a pass narrower than it: a tall render target (see PassHeight).
    const uint32_t surfaceInfo = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    static LiveSwitch noTall("FM2_NO_TALL_PASS");
    if (!noTall && y1 > int32_t(kHeight) && y1 <= 2048 && (surfaceInfo & 0x3FFF) < kWidth)
    {
        if (g_tallInfo != surfaceInfo || g_tallHeight != uint32_t(y1))
            Log("tall pass: scissor y %d..%d x %d..%d window offset %d,%d vertexOffset %d surface %08X", y0, y1, x0, x1, wx, wy, int(vertexOffset), surfaceInfo);
        g_tallInfo = surfaceInfo;
        g_tallHeight = uint32_t(y1);
        g_tallKey = LayoutKey(surfaceInfo);
        // FM2_CAPTURE_TALL=1: trace this very frame from here on (its dump is taken at the swap).
        static LiveSwitch captureTall("FM2_CAPTURE_TALL");
        static bool traced = false;
        if (captureTall && !traced)
        {
            traced = true;
            g_hotkeyFrame = g_r.frame + 1;
            g_tallSeen = true;
        }
    }
    const LONG maxY = LONG(PassHeight());
    const LONG maxX = LONG(PassWidth());
    return { std::clamp<LONG>(x0, 0, maxX), std::clamp<LONG>(y0, 0, maxY), std::clamp<LONG>(x1, 0, maxX), std::clamp<LONG>(y1, 0, maxY) };
}

void TransitionColor(D3D12_RESOURCE_STATES to)
{
    if (C().colorState == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = C().color.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = C().colorState;
    b.Transition.StateAfter = to;
    C().list->ResourceBarrier(1, &b);
    C().colorState = to;
}

// Draws the resolve rectangle of the colour (or depth) buffer into the
// destination's host texture at the same pixels, converting the format and
// scaling colour by the resolve's exponent bias.
// Makes `dest` the variant of that size and format (an earlier one from its spares, or new).
void EnsureResolved(ResolvedTexture& dest, uint32_t width, uint32_t height, uint32_t hostWidth, uint32_t hostHeight, DXGI_FORMAT format)
{
    auto swapVariant = [](ResolvedTexture& a, ResolvedTexture& b) {
        std::swap(a.resource, b.resource);
        std::swap(a.width, b.width);
        std::swap(a.height, b.height);
        std::swap(a.hostWidth, b.hostWidth);
        std::swap(a.hostHeight, b.hostHeight);
        std::swap(a.frame, b.frame);
        std::swap(a.format, b.format);
        std::swap(a.swapped, b.swapped);
    };
    if (!dest.resource || dest.width != width || dest.height != height || dest.hostWidth != hostWidth || dest.hostHeight != hostHeight ||
        dest.format != format)
    {
        bool found = false;
        for (ResolvedTexture& spare : dest.spares)
            if (spare.resource && spare.width == width && spare.height == height && spare.hostWidth == hostWidth &&
                spare.hostHeight == hostHeight && spare.format == format)
            {
                swapVariant(dest, spare);
                found = true;
                break;
            }
        if (!found)
        {
            if (dest.resource)
            {
                // The displaced variant stays available; bound the pool.
                if (dest.spares.size() >= 6)
                {
                    Frame().garbage.push_back(std::move(dest.spares.front().resource));
                    dest.spares.erase(dest.spares.begin());
                }
                dest.spares.emplace_back();
                swapVariant(dest, dest.spares.back());
            }
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = hostWidth;
            td.Height = hostHeight;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = format;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            // Colour images can also be an upscaler's output (written by compute).
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                      IID_PPV_ARGS(&dest.resource)),
                "CreateCommittedResource(resolve)");
            CountCreate("resolve");
            dest.width = width;
            dest.height = height;
            dest.hostWidth = hostWidth;
            dest.hostHeight = hostHeight;
            dest.format = format;
            dest.swapped = false;
        }
    }
}

void BlitResolve(bool depth, uint32_t base, uint32_t width, uint32_t height, DXGI_FORMAT format, const D3D12_RECT& rect, float scale, bool swapRB = false)
{
    ResolvedTexture& dest = g_r.resolved[base];
    const uint32_t hostWidth = uint32_t(HostX(LONG(width))), hostHeight = uint32_t(HostY(LONG(height)));
    EnsureResolved(dest, width, height, hostWidth, hostHeight, format);
    // What memory holds under the image, for the stale check (ResolveCurrent): once per frame, as tiles
    // land in the same image. An image resolved every frame (shadow maps, bloom) only needs it every
    // 8th: the check waits until a resolve has not been repeated for 8 frames, so its baseline is
    // never older than that. Hashing each of PGR4's 31 resolves a frame took 0.9 ms on an FX-8350.
    // (Write-backs record our own writes as they happen.) FM2_RESOLVE_BASELINE_EVERY_FRAME=1: every frame.
    static LiveSwitch baselineEveryFrame("FM2_RESOLVE_BASELINE_EVERY_FRAME");
    // (Resolved within the last 4 frames counts: images refreshed every second or third frame, like
    // reflection faces, were hashed on every resolve.)
    const bool continuing = dest.frame != ~0ull && dest.frame + 4 >= g_r.frame && g_r.frame - dest.memChecked < 8 && !dest.stale;
    if (dest.frame != g_r.frame && (baselineEveryFrame || !continuing))
        NoteResolvedMemory(base, dest);
    dest.frame = g_r.frame;
    const D3D12_RECT sr{ rect.left, rect.top, std::min<LONG>(rect.right, width), std::min<LONG>(rect.bottom, height) };
    FrameResources& f = Frame();
    if (sr.right <= sr.left || sr.bottom <= sr.top || f.descriptorsUsed >= kDescriptorsPerFrame)
        return;

    const UINT slot = f.descriptorBase + f.descriptorsUsed++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : kColorFormat;
    srv.ViewDimension = g_msaa > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(depth ? C().depth.Get() : C().color.Get(), &srv, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    const auto rtv = g_r.blitRtvHeap->GetCPUDescriptorHandleForHeapStart();
    C().list->CreateRenderTargetView(g_r.device, dest.resource.Get(), rtv); // in order with the recorded commands

    D3D12_RESOURCE_BARRIER b[2]{};
    UINT barriers = 1;
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = dest.resource.Get();
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (depth)
    {
        b[1].Transition.pResource = C().depth.Get();
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers = 2;
    }
    else
        TransitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    C().list->ResourceBarrier(barriers, b);

    const bool ms = g_msaa > 1;
    ID3D12PipelineState* pso = depth ? (ms ? g_r.depthBlitMs.Get() : g_r.depthBlit.Get())
        : format == DXGI_FORMAT_R8G8B8A8_UNORM ? (ms ? g_r.colorBlitMs8.Get() : g_r.colorBlit8.Get())
        : (ms ? g_r.colorBlitMs16.Get() : g_r.colorBlit16.Get());
    C().list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    C().list->SetGraphicsRootSignature(g_r.blitRootSignature.Get());
    C().list->SetPipelineState(pso);
    C().list->SetGraphicsRootDescriptorTable(0, Gpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    const uint32_t blitConstants[2] = { std::bit_cast<uint32_t>(scale), uint32_t(swapRB) };
    C().list->SetGraphicsRoot32BitConstants(1, 2, blitConstants, 0);
    // Width and height stay in game pixels (the size the title knows); the texture and the
    // rectangle are scaled.
    const D3D12_VIEWPORT vp{ 0, 0, float(hostWidth), float(hostHeight), 0, 1 };
    const D3D12_RECT hostRect = ToHost(sr);
    C().list->RSSetViewports(1, &vp);
    C().list->RSSetScissorRects(1, &hostRect);
    C().list->DrawInstanced(3, 1, 0, 0);

    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    C().list->ResourceBarrier(barriers, b);
    TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Back to the main targets and root signature.
    C().list->OMSetRenderTargets(1, &C().rtv, FALSE, &C().dsv);
    C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    g_drawState.Invalidate();
}

// EDRAM resolve: copy a rectangle of the render target to the destination
// (as a host texture), then optionally clear colour and depth.
// ---- Upscaling ---------------------------------------------------------------------------
// Temporal upscalers (DLSS, FSR 3): the motion target, jitter, and the evaluation.
bool InitUpscale();
bool TemporalInit()
{
    if (!InitUpscale())
        return false;
    if (!g_upscale.motion)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = OutputWidth();
        td.Height = OutputHeight();
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = td.Format;
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                IID_PPV_ARGS(&g_upscale.motion))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(g_r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_upscale.motionRtvHeap))))
            return false;
        g_upscale.motionRtv = g_upscale.motionRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_r.device->CreateRenderTargetView(g_upscale.motion.Get(), nullptr, g_upscale.motionRtv);
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&g_upscale.depth))))
            return false;
    }
    return true;
}

// A tile of the scene was resolved (its depth may be cleared next): keep its depth.
void CopySceneDepth(const D3D12_RECT& gameRect)
{
    const D3D12_RECT r = ToHost(gameRect);
    FrameResources& f = Frame();
    if (!g_upscale.depth || !C().depth || r.right <= r.left || r.bottom <= r.top || f.descriptorsUsed + 2 > kDescriptorsPerFrame)
        return;
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(C().depth.Get(), &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(g_upscale.depth.Get(), nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = C().depth.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    C().list->ResourceBarrier(1, &b);
    uint32_t c[24] = {};
    c[0] = uint32_t(r.left);
    c[1] = uint32_t(r.top);
    c[20] = uint32_t(r.right - r.left);
    c[21] = uint32_t(r.bottom - r.top);
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.depthCopy.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch((c[20] + 7) / 8, (c[21] + 7) / 8, 1);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
}

float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index; index /= base)
    {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

// Start of a temporal frame: jitter, a cleared motion target, the next constants map.
void BeginTemporalFrame()
{
    g_upscale.renderWidth = uint32_t(std::lround(OutputWidth() * double(g_sceneScale)));
    g_upscale.renderHeight = uint32_t(std::lround(OutputHeight() * double(g_sceneScale)));
    // Phases: 8 per pixel of upscale area (DLSS guidance), at most 64.
    const double ratio = double(OutputWidth()) / std::max(1u, g_upscale.renderWidth);
    const uint32_t phases = std::clamp(uint32_t(std::ceil(8.0 * ratio * ratio)), 8u, 64u);
    const uint32_t index = uint32_t(g_r.frame % phases) + 1;
    static LiveSwitch noJitter("FM2_NO_JITTER");
    g_upscale.jitterPx[0] = noJitter ? 0.0f : Halton(index, 2) - 0.5f;
    g_upscale.jitterPx[1] = noJitter ? 0.0f : Halton(index, 3) - 0.5f;
    g_upscale.jitterNdc[0] = 2.0f * g_upscale.jitterPx[0] / float(std::max(1u, g_upscale.renderWidth));
    g_upscale.jitterNdc[1] = -2.0f * g_upscale.jitterPx[1] / float(std::max(1u, g_upscale.renderHeight));
    g_upscale.constantsFrame++;
    g_upscale.occurrences.clear();
    if (g_upscale.constantsFrame % 600 == 0) // draws not seen for a while
        std::erase_if(g_upscale.constants, [](const auto& e) { return e.second.frame + 2 < g_upscale.constantsFrame; });
    const float zero[4] = {};
    C().list->ClearRenderTargetView(g_upscale.motionRtv, zero, 0, nullptr);
}

void UpscaleResolved(uint32_t base);

// DLSS or FSR 3 on the scene image at `base`: colour (render size), depth and motion (top left of
// output-sized textures); the upscaled image replaces it like UpscaleResolved.
bool EvaluateTemporal(uint32_t base, bool dlss)
{
    if (dlss && !DlssInit(g_r.device))
        return false;
    auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource)
        return false;
    ResolvedTexture& dest = it->second;
    if (dest.format != DXGI_FORMAT_R8G8B8A8_UNORM && dest.format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return false;
    const ComPtr<ID3D12Resource> color = dest.resource;
    const uint32_t inWidth = dest.hostWidth, inHeight = dest.hostHeight;
    const float savedScale = g_sceneScale;
    g_sceneScale = 1.0f;
    const uint32_t outWidth = uint32_t(HostX(LONG(dest.width))), outHeight = uint32_t(HostY(LONG(dest.height)));
    g_sceneScale = savedScale;
    if (outWidth < inWidth || outHeight < inHeight)
        return false;
    const uint64_t frame = dest.frame;
    const DXGI_FORMAT format = dest.format;
    const bool swapped = dest.swapped;
    // DLAA (native scale): the output must be another texture; the pool keys variants on
    // size, so a same-size output gets its own texture, copied back after.
    const bool dlaa = outWidth == inWidth && outHeight == inHeight;
    if (dlaa)
    {
        if (!g_upscale.dlaaOut || g_upscale.dlaaOut->GetDesc().Width != outWidth || g_upscale.dlaaOut->GetDesc().Height != outHeight ||
            g_upscale.dlaaOut->GetDesc().Format != format)
        {
            if (g_upscale.dlaaOut)
                Frame().garbage.push_back(std::move(g_upscale.dlaaOut));
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = color->GetDesc();
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_upscale.dlaaOut))))
                return false;
        }
    }
    else
        EnsureResolved(dest, dest.width, dest.height, outWidth, outHeight, format);
    dest.frame = frame;
    dest.swapped = swapped;

    DlssSetup setup;
    setup.renderWidth = inWidth;
    setup.renderHeight = inHeight;
    setup.outputWidth = outWidth;
    setup.outputHeight = outHeight;
    setup.quality = GetSettings().quality;
    setup.hdr = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    DlssFrame in{};
    in.color = color.Get();
    in.depth = g_upscale.depth.Get();
    in.motion = g_upscale.motion.Get();
    in.output = dlaa ? g_upscale.dlaaOut.Get() : dest.resource.Get();
    static const float jitterSign = getenv("FM2_DLSS_JITTER_FLIP") ? -1.0f : 1.0f;
    in.jitterX = g_upscale.jitterPx[0] * jitterSign;
    in.jitterY = g_upscale.jitterPx[1] * jitterSign;
    in.motionScaleX = float(inWidth);
    in.motionScaleY = float(inHeight);
    in.frameTimeMs = float(std::chrono::duration<double, std::milli>(now - last).count());
    in.reset = g_upscale.reset;
    last = now;

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier(g_upscale.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(g_upscale.motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(in.output, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // NGX records into the real command list (after everything recorded so far).
    const bool ok = dlss ? DlssEvaluate(C().list->Real(), setup, in)
                         : Fsr3Evaluate(g_r.device, C().list->Real(), setup, in, GetSettings().sharpness);
    barrier(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    if (dlaa && ok)
    {
        barrier(in.output, read, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(color.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        C().list->Real()->CopyResource(color.Get(), in.output);
        barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
        barrier(in.output, D3D12_RESOURCE_STATE_COPY_SOURCE, read);
    }
    barrier(g_upscale.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(g_upscale.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // NGX may have changed the list's heaps, root signatures and pipeline: set ours again.
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    C().list->SetDescriptorHeaps(1, heaps);
    C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    C().list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_drawState.Invalidate();
    SelectSurfaces(true);
    if (!ok && !dlaa) // keep the image: show the small one upscaled by FSR 1 instead
    {
        dest.resource = color;
        dest.hostWidth = inWidth;
        dest.hostHeight = inHeight;
        return false;
    }
    return true;
}

// The temporal upscale of the scene image at `base` (colour, depth, motion, jitter); FSR 1
// where the chosen upscaler is not available.
void TemporalEvaluate(uint32_t base)
{
    const bool done = EvaluateTemporal(base, GetSettings().upscaler == Settings::Upscaler::Dlss);
    if (!done)
        UpscaleResolved(base);
    g_upscale.reset = false;
}

// The scene (everything up to the full-screen resolve of the main view) renders at
// g_sceneScale; that resolved image is upscaled to the output size (FSR 1: EASU, then RCAS
// sharpening) and post-processing and the HUD render at full size from it.

bool InitUpscale()
{
    if (g_upscale.tried)
        return g_upscale.ready;
    g_upscale.tried = true;
    D3D12_DESCRIPTOR_RANGE srv{}, uav{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 1;
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 24;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(g_r.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_upscale.root))))
    {
        Log("upscale: root signature failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_upscale.root.Get();
    pd.CS = { kEasuMain, sizeof(kEasuMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.easu))))
        return false;
    pd.CS = { kRcasMain, sizeof(kRcasMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.rcas))))
        return false;
    pd.CS = { kDepthCopyMain, sizeof(kDepthCopyMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.depthCopy))))
        return false;
    pd.CS = { kGammaMain, sizeof(kGammaMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.gamma))))
        return false;
    pd.CS = { kEnhanceMain, sizeof(kEnhanceMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.enhance))))
        return false;
    g_upscale.ready = true;
    Log("upscale: FSR 1 ready (scene at %.0f%% of %ux%u)", GetSettings().SceneScale() * 100.0f, OutputWidth(), OutputHeight());
    return true;
}

// ---- Display gamma ---------------------------------------------------------------------
// The console's display maps the front buffer through a 256-entry table the title uploads
// (DC_LUT_30_COLOR); FM2's darkens slightly (about x^1.15, with a darker toe). Without it the
// picture is ~20% brighter than the console's. FM2_NO_GAMMA=1 shows the front buffer as is.
std::atomic<uint32_t> g_gammaTable[256];
std::atomic<bool> g_gammaSet{ false };
ComPtr<ID3D12Resource> g_gammaOut;
ComPtr<ID3D12Resource> g_enhanceOut; // enhanced lighting output (Settings::enhance)

// Returns the graded copy of `image` (in COPY_SOURCE state), or null to show it as is.
ID3D12Resource* GradeFrontBuffer(ID3D12Resource* image)
{
    static LiveSwitch off("FM2_NO_GAMMA");
    if (off || !g_gammaSet.load(std::memory_order_relaxed) || !InitUpscale())
        return nullptr;
    // 17 points of the table (green; the title writes equal channels), at x = k/16.
    float points[17];
    bool identity = true;
    for (uint32_t k = 0; k <= 16; k++)
    {
        const float at = k * 255.0f / 16.0f;
        const uint32_t i = std::min(uint32_t(at), 254u);
        const float t = at - float(i);
        const float a = float((g_gammaTable[i].load(std::memory_order_relaxed) >> 10) & 0x3FF) / 1023.0f;
        const float b = float((g_gammaTable[i + 1].load(std::memory_order_relaxed) >> 10) & 0x3FF) / 1023.0f;
        points[k] = a + (b - a) * t;
        identity &= std::fabs(points[k] - k / 16.0f) < 0.002f;
    }
    if (identity)
        return nullptr;
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 2 > kDescriptorsPerFrame)
        return nullptr;
    if (!g_gammaOut || g_gammaOut->GetDesc().Width != desc.Width || g_gammaOut->GetDesc().Height != desc.Height)
    {
        D3D12_RESOURCE_DESC td = desc;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        RetireTexture(std::move(g_gammaOut));
        g_gammaOut = CreateTexture(td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "gamma");
    }
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(image, &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(g_gammaOut.Get(), nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    uint32_t c[24] = {};
    memcpy(c, points, sizeof(points));
    c[20] = uint32_t(desc.Width);
    c[21] = desc.Height;
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.gamma.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch(UINT((desc.Width + 7) / 8), (desc.Height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_gammaOut.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    C().list->ResourceBarrier(1, &b);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    return g_gammaOut.Get();
}

// Enhanced lighting (Settings::enhance): the frame through the local-contrast pass. `image` is
// in COPY_SOURCE and so is the result; returns null when the option is off.
ID3D12Resource* EnhanceFrontBuffer(ID3D12Resource* image)
{
    if (!GetSettings().enhance || !image || !InitUpscale())
        return nullptr;
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 2 > kDescriptorsPerFrame)
        return nullptr;
    if (!g_enhanceOut || g_enhanceOut->GetDesc().Width != desc.Width || g_enhanceOut->GetDesc().Height != desc.Height)
    {
        D3D12_RESOURCE_DESC td = desc;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        RetireTexture(std::move(g_enhanceOut));
        g_enhanceOut = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_SOURCE, "enhance");
    }
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(image, &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(g_enhanceOut.Get(), nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));

    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b)
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = image;
    b[0].Transition.Subresource = 0;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource = g_enhanceOut.Get();
    b[1].Transition.Subresource = 0;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    C().list->ResourceBarrier(2, b);

    // The strength (0.35) rides in the rcasCon slot of the constants (index 16), as float bits.
    const float strength = 0.35f;
    uint32_t c[24] = {};
    c[16] = std::bit_cast<uint32_t>(strength);
    c[20] = uint32_t(desc.Width);
    c[21] = desc.Height;
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.enhance.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch(UINT((desc.Width + 7) / 8), (desc.Height + 7) / 8, 1);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    C().list->ResourceBarrier(2, b);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    return g_enhanceOut.Get();
}

// Upscales the resolved image at `base` (rendered at the scene scale) to the output size in
// place: the resolve keeps the small image among its spares and shows the upscaled one.
void UpscaleResolved(uint32_t base)
{
    auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource || !InitUpscale())
        return;
    ResolvedTexture& dest = it->second;
    if (dest.format != DXGI_FORMAT_R8G8B8A8_UNORM && dest.format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return;
    const ComPtr<ID3D12Resource> source = dest.resource;
    const uint32_t inWidth = dest.hostWidth, inHeight = dest.hostHeight;
    const float savedScale = g_sceneScale;
    g_sceneScale = 1.0f;
    const uint32_t outWidth = uint32_t(HostX(LONG(dest.width))), outHeight = uint32_t(HostY(LONG(dest.height)));
    g_sceneScale = savedScale;
    if (outWidth <= inWidth || outHeight <= inHeight)
        return;
    const uint64_t frame = dest.frame;
    const DXGI_FORMAT format = dest.format;
    const bool swapped = dest.swapped;
    EnsureResolved(dest, dest.width, dest.height, outWidth, outHeight, format);
    dest.frame = frame;
    dest.swapped = swapped;
    if (!g_upscale.temp || g_upscale.tempWidth != outWidth || g_upscale.tempHeight != outHeight)
    {
        if (g_upscale.temp)
            Frame().garbage.push_back(std::move(g_upscale.temp));
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = outWidth;
        td.Height = outHeight;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                  IID_PPV_ARGS(&g_upscale.temp)),
            "CreateCommittedResource(upscale)");
        g_upscale.tempWidth = outWidth;
        g_upscale.tempHeight = outHeight;
    }
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 4 > kDescriptorsPerFrame)
        return;
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 4;
    auto srvOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        g_r.device->CreateShaderResourceView(r, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    auto uavOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_r.device->CreateUnorderedAccessView(r, nullptr, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    srvOf(source.Get(), format, slots);
    uavOf(g_upscale.temp.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slots + 1);
    srvOf(g_upscale.temp.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slots + 2);
    uavOf(dest.resource.Get(), format, slots + 3);

    // FSR 1 constants (FsrEasuCon / FsrRcasCon): the whole source texture is the input.
    auto bits = [](float v) { return std::bit_cast<uint32_t>(v); };
    uint32_t c[24] = {};
    const float iw = float(inWidth), ih = float(inHeight), ow = float(outWidth), oh = float(outHeight);
    c[0] = bits(iw / ow); c[1] = bits(ih / oh); c[2] = bits(0.5f * iw / ow - 0.5f); c[3] = bits(0.5f * ih / oh - 0.5f);
    c[4] = bits(1.0f / iw); c[5] = bits(1.0f / ih); c[6] = bits(1.0f / iw); c[7] = bits(-1.0f / ih);
    c[8] = bits(-1.0f / iw); c[9] = bits(2.0f / ih); c[10] = bits(1.0f / iw); c[11] = bits(2.0f / ih);
    c[12] = bits(0.0f); c[13] = bits(4.0f / ih);
    // RCAS: sharpness in stops (0 sharpest); the setting's 1 is sharpest.
    const float sharp = std::exp2(-2.0f * (1.0f - GetSettings().sharpness));
    c[16] = bits(sharp);
    const uint16_t h = DirectX::PackedVector::XMConvertFloatToHalf(sharp);
    c[17] = uint32_t(h) | uint32_t(h) << 16;
    c[20] = outWidth;
    c[21] = outHeight;

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const UINT gx = (outWidth + 15) / 16, gy = (outHeight + 15) / 16;
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.easu.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch(gx, gy, 1);
    barrier(g_upscale.temp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    barrier(dest.resource.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    C().list->SetPipelineState(g_upscale.rcas.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots + 2, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 3, g_r.srvStride));
    C().list->Dispatch(gx, gy, 1);
    barrier(dest.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    barrier(g_upscale.temp.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
}

// FSR 1 on the whole frame (the launcher's render scale and sharpening): the front buffer's image
// upscaled (EASU, when it was rendered smaller) and sharpened (RCAS) into a texture of its own,
// which is shown instead. The title's image is left as it is: it reads it back in later frames,
// and sharpening it in place sharpened it again every frame (banding, posterised colour).
// Returns null to show the image as it is.
ID3D12Resource* Fsr1Present(const ResolvedTexture& image)
{
    static LiveSwitch noFsr1("FM2_NO_FSR1"); // A/B: the image as rendered (stretched when it is smaller)
    if (noFsr1 || !image.resource || image.format != DXGI_FORMAT_R8G8B8A8_UNORM || !InitUpscale())
        return nullptr;
    const uint32_t inWidth = image.hostWidth, inHeight = image.hostHeight;
    const uint32_t outWidth = OutputWidth(), outHeight = OutputHeight();
    const bool upscale = inWidth < outWidth || inHeight < outHeight;
    if (!upscale && GetSettings().sharpness <= 0.0f)
        return nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = outWidth;
    td.Height = outHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!g_upscale.presentOut || g_upscale.presentOut->GetDesc().Width != outWidth || g_upscale.presentOut->GetDesc().Height != outHeight)
    {
        if (g_upscale.presentOut)
            Frame().garbage.push_back(std::move(g_upscale.presentOut));
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, read, nullptr, IID_PPV_ARGS(&g_upscale.presentOut))))
            return nullptr;
    }
    if (upscale && (!g_upscale.temp || g_upscale.tempWidth != outWidth || g_upscale.tempHeight != outHeight))
    {
        if (g_upscale.temp)
            Frame().garbage.push_back(std::move(g_upscale.temp));
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&g_upscale.temp))))
            return nullptr;
        g_upscale.tempWidth = outWidth;
        g_upscale.tempHeight = outHeight;
    }
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 4 > kDescriptorsPerFrame)
        return nullptr;
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 4;
    auto srvOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        g_r.device->CreateShaderResourceView(r, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    auto uavOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_r.device->CreateUnorderedAccessView(r, nullptr, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    // EASU's input and output, then RCAS's input (EASU's output, or the image itself) and output.
    srvOf(image.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slots);
    if (upscale)
    {
        uavOf(g_upscale.temp.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slots + 1);
        srvOf(g_upscale.temp.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slots + 2);
    }
    else
        srvOf(image.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slots + 2);
    uavOf(g_upscale.presentOut.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slots + 3);

    // FSR 1 constants (FsrEasuCon / FsrRcasCon).
    auto bits = [](float v) { return std::bit_cast<uint32_t>(v); };
    uint32_t c[24] = {};
    const float iw = float(inWidth), ih = float(inHeight), ow = float(outWidth), oh = float(outHeight);
    c[0] = bits(iw / ow); c[1] = bits(ih / oh); c[2] = bits(0.5f * iw / ow - 0.5f); c[3] = bits(0.5f * ih / oh - 0.5f);
    c[4] = bits(1.0f / iw); c[5] = bits(1.0f / ih); c[6] = bits(1.0f / iw); c[7] = bits(-1.0f / ih);
    c[8] = bits(-1.0f / iw); c[9] = bits(2.0f / ih); c[10] = bits(1.0f / iw); c[11] = bits(2.0f / ih);
    c[12] = bits(0.0f); c[13] = bits(4.0f / ih);
    // RCAS: sharpness in stops (0 sharpest); the setting's 1 is sharpest, 0 the softest RCAS.
    const float sharp = std::exp2(-2.0f * (1.0f - GetSettings().sharpness));
    c[16] = bits(sharp);
    const uint16_t h = DirectX::PackedVector::XMConvertFloatToHalf(sharp);
    c[17] = uint32_t(h) | uint32_t(h) << 16;
    c[20] = outWidth;
    c[21] = outHeight;

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    const UINT gx = (outWidth + 15) / 16, gy = (outHeight + 15) / 16;
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    if (upscale)
    {
        C().list->SetPipelineState(g_upscale.easu.Get());
        C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
        C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
        C().list->Dispatch(gx, gy, 1);
        barrier(g_upscale.temp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    }
    barrier(g_upscale.presentOut.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    C().list->SetPipelineState(g_upscale.rcas.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots + 2, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 3, g_r.srvStride));
    C().list->Dispatch(gx, gy, 1);
    barrier(g_upscale.presentOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    if (upscale)
        barrier(g_upscale.temp.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    return g_upscale.presentOut.Get();
}

// The scene is complete (its full-screen image resolved): upscale it, and render the rest of
// the frame (post-processing, HUD) at full size.
void UpscaleScene(uint32_t base)
{
    if (g_temporal)
        TemporalEvaluate(base);
    else
        UpscaleResolved(base);
    g_sceneScale = 1.0f;
    g_scenePhase = false;
    g_upscale.switchedThisFrame = true;
}

// The scene's upscale, once all the resolves of its last tile are done (see Upscale::pending).
void FlushPendingUpscale()
{
    if (!g_upscale.pending)
        return;
    g_upscale.pending = false;
    UpscaleScene(g_upscale.pendingBase);
}

// Copies the render target resolved to `base` into guest memory, tiled, for the title to read.
void WriteResolveToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info);
void WriteDepthToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info, const D3D12_RECT& rect);
void WritePhotoToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info);
extern uint64_t g_lastFocusFrame;

void Resolve()
{
    g_resolveEpoch++; // texture bindings may now map to a new resolve
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
    SelectSurfaces(false);
    PassMark("resolve to %08X (colour %03X)", gpureg::Get(RB_COPY_DEST_BASE), C().colorBase);
    const uint32_t control = gpureg::Get(RB_COPY_CONTROL);
    const uint32_t command = (control >> 20) & 3;

    // The rectangle: three float2 vertices in vertex fetch constant 0.
    const uint32_t d0 = gpureg::Get(gpureg::FetchConstants), d1 = gpureg::Get(gpureg::FetchConstants + 1);
    const uint32_t vertices = GpuPhysToGuest(d0 & 0xFFFFFFFC);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < 3; i++)
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
    // Clamped to the tile, as the hardware does: in a tile pass the
    // resolves for the other tiles become empty.
    const D3D12_RECT tile = WindowScissor();
    D3D12_RECT rect{ std::clamp<LONG>(LONG(std::lround(x0)), tile.left, tile.right), std::clamp<LONG>(LONG(std::lround(y0)), tile.top, tile.bottom),
        std::clamp<LONG>(LONG(std::lround(x1)), tile.left, tile.right), std::clamp<LONG>(LONG(std::lround(y1)), tile.top, tile.bottom) };
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;

    const uint32_t sourceSelect = control & 7;
    const uint64_t traceFrame = TraceFrame();
    // FM2_TRACE_RESOLVES=1: every resolve (not just those of the traced frame), with its time.
    static LiveSwitch traceAllResolves("FM2_TRACE_RESOLVES");
    // FM2_TRACE_RESOLVES_MIN=<width>: only resolves at least that wide (less slowdown).
    static const long traceMinWidth = getenv("FM2_TRACE_RESOLVES_MIN") ? atol(getenv("FM2_TRACE_RESOLVES_MIN")) : 0;
    if ((traceAllResolves && rect.right - rect.left >= traceMinWidth) || (traceFrame && g_r.frame + 1 == traceFrame))
        Log("resolve: control %08X dest %08X pitch %08X info %08X rect %ld,%ld-%ld,%ld clear %08X depth clear %08X (color info %08X depth info %08X surface %08X)", control,
            gpureg::Get(RB_COPY_DEST_BASE), gpureg::Get(RB_COPY_DEST_PITCH), gpureg::Get(RB_COPY_DEST_INFO), rect.left, rect.top, rect.right, rect.bottom,
            gpureg::Get(RB_COLOR_CLEAR), gpureg::Get(RB_DEPTH_CLEAR), gpureg::Get(RB_COLOR_INFO), gpureg::Get(RB_DEPTH_INFO), gpureg::Get(RB_SURFACE_INFO));
    // FM2_CAPTURE_ON_RESOLVE=<dest hex>[,<skip>]: trace the frame after the skip-th resolve to that address
    // (passes that run only now and then, like the livery editor's moving-layer preview).
    if (static const char* onResolve = getenv("FM2_CAPTURE_ON_RESOLVE"); onResolve)
    {
        static const uint32_t dest = uint32_t(strtoul(onResolve, nullptr, 16));
        static int skip = strchr(onResolve, ',') ? atoi(strchr(onResolve, ',') + 1) : 0;
        if ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) == dest && skip-- == 0)
        {
            g_hotkeyFrame = g_r.frame + 2;
            Log("capture: resolve to %08X seen, tracing frame %llu", dest, (unsigned long long)(g_r.frame + 2));
        }
    }
    if (command != 3 && sourceSelect <= 4)
    {
        // Each tile resolves to the destination row it starts at; key the
        // host texture by the image's own base so the tiles land together.
        const uint32_t pitchReg = gpureg::Get(RB_COPY_DEST_PITCH);
        const uint32_t pitch = pitchReg & 0x3FFF;
        const uint32_t base = ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) - uint32_t(rect.top) * pitch * 4) & 0x1FFFFFFF;
        const uint32_t width = std::clamp<uint32_t>(pitch, 1, std::max<uint32_t>(kWidth, BigPassSize()));
        // Taller than the screen is possible: the paint editor's 1024x1024 livery target is
        // resolved 720 rows at a time and sampled as 1024 rows. The texture keeps all its rows
        // (clamped to kHeight it was stretched 1.4x vertically when sampled).
        constexpr uint32_t kMaxResolveHeight = 2048;
        uint32_t height = std::clamp<uint32_t>(((pitchReg >> 16) & 0x3FFF) + uint32_t(rect.top), 1, kMaxResolveHeight);
        // A later tile into an image that already holds its rows keeps the image: Photo Mode's
        // depth copies give the full 720 rows for every tile (976, 1232 with the tile's offset),
        // and each "taller" copy recreated the texture, dropping the tiles before it (the lower
        // two thirds of the picture came out black). It grows only when the rows don't fit.
        static LiveSwitch noKeep("FM2_NO_HEIGHT_KEEP");
        if (auto it = g_r.resolved.find(base); !noKeep && it != g_r.resolved.end() && it->second.resource && rect.top > 0 &&
                                                uint32_t(rect.bottom) <= it->second.height && it->second.height < height)
            height = it->second.height;
        const uint32_t info = gpureg::Get(RB_COPY_DEST_INFO);
        const bool depth = sourceSelect == 4;
        // Destination: 8888 stays 8-bit (clamped, as the hardware does); other
        // formats keep the HDR range. The exponent bias scales colour copies.
        const uint32_t destFormat = (info >> 7) & 0x3F;
        const DXGI_FORMAT format = depth ? DXGI_FORMAT_R32_FLOAT
            : destFormat == 6 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        const int bias = int32_t(info << 10) >> 26; // bits 16-21, signed
        // Bit 24 (copy swap): the destination stores red and blue exchanged; the
        // fetch swizzle swaps them back when the title samples it.
        const bool swapRB = !depth && ((info >> 24) & 1);
        // Guest bytes per pixel (Xenos colour formats: k_8 2, 16-bit 3-5/10/15/24, 64-bit 26/37, 128-bit 38).
        g_r.resolved[base].guestBpp = depth ? 4
            : destFormat == 2 ? 1
            : (destFormat >= 3 && destFormat <= 5) || destFormat == 10 || destFormat == 15 || destFormat == 24 ? 2
            : destFormat == 26 || destFormat == 37 ? 8
            : destFormat == 38 ? 16 : 4;
        BlitResolve(depth, base, width, height, format, rect, depth ? 1.0f : std::ldexp(1.0f, bias));
        // Temporal upscaling: each tile of the main view keeps its depth before it is cleared.
        if (g_temporal && g_scenePhase && !depth && width >= kWidth && rect.left == 0 && rect.right >= LONG(kWidth))
            CopySceneDepth(rect);
        // The main view's full image (its last tile reaches the bottom): the scene is done.
        if (g_temporal && g_scenePhase && !depth && width >= kWidth && rect.left == 0 && rect.right >= LONG(kWidth) &&
            rect.bottom >= LONG(kHeight) && !g_upscale.pending)
        {
            g_upscale.pending = true;
            g_upscale.pendingBase = base;
        }
        g_r.resolved[base].swapped = swapRB;
        // The title reads some resolved images back on the CPU (the garage thumbnail it renders, then
        // compresses and saves, and the 1024x1024 livery a race compresses for the car body): our
        // resolves stay on the GPU, so for the sizes listed in FM2_RESOLVE_WRITEBACK ("WxH,WxH",
        // default 256x128,1024x1024) the pixels are also written to memory.
        static const std::vector<std::pair<uint32_t, uint32_t>> writeback = [] {
            std::vector<std::pair<uint32_t, uint32_t>> v;
            const char* e = getenv("FM2_RESOLVE_WRITEBACK");
            for (const char* p = e ? e : "256x128,1024x1024"; p && *p;)
            {
                uint32_t w = 0, h = 0;
                if (sscanf(p, "%ux%u", &w, &h) == 2)
                    v.emplace_back(w, h);
                p = strchr(p, ',');
                if (p)
                    p++;
            }
            return v;
        }();
        // Depth of the 4x scene copied out as 8888 (Photo Mode only, every frame; races copy smaller
        // depth images the same way and must not pay for a readback): the title reads the depth under the
        // reticle on the CPU for its focus point, so these rows also go to memory.
        static LiveSwitch noDepthWriteback("FM2_NO_DEPTH_WRITEBACK");
        if (depth && destFormat == 6 && !noDepthWriteback && width >= kWidth && ((gpureg::Get(RB_SURFACE_INFO) >> 16) & 3) == 2) // 4x scene tiles only
            WriteDepthToGuest(base, width, height, info, rect);
        // Photo Mode's picture (the 4x 8888 image, its last tile): saving locks it and compresses it
        // on the CPU (sub_82293E28), so while the focus readback says Photo Mode is up it goes to memory too.
        if (!depth && destFormat == 6 && !noDepthWriteback && width >= kWidth && rect.left == 0 && rect.bottom >= LONG(kHeight) &&
            ((gpureg::Get(RB_SURFACE_INFO) >> 16) & 3) == 2 && g_r.frame - g_lastFocusFrame <= 2)
            WritePhotoToGuest(base, width, height, info);
        if (!depth && destFormat == 6 && rect.left == 0 && rect.top == 0 &&
            std::find(writeback.begin(), writeback.end(), std::pair<uint32_t, uint32_t>(width, height)) != writeback.end())
            WriteResolveToGuest(base, width, height, info);
    }
    // Clears after a tile's resolve prepare the EDRAM for the next tile, which
    // we draw lower on the same host surface: a full-width clear covers every
    // row from the tile down.
    D3D12_RECT clearRect = rect;
    const uint32_t surfacePitch = gpureg::Get(RB_SURFACE_INFO) & 0x3FFF;
    static LiveSwitch noClearExtend("FM2_NO_CLEAR_EXTEND");
    if (!noClearExtend && surfacePitch && uint32_t(rect.right - rect.left) >= std::min<uint32_t>(surfacePitch, kWidth) && rect.left == 0)
        clearRect.bottom = PassHeight();
    if ((control >> 8) & 1) // colour clear (ARGB)
    {
        const uint32_t c = gpureg::Get(RB_COLOR_CLEAR);
        const float color[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_RECT hostClear = ToHost(clearRect);
        C().list->ClearRenderTargetView(C().rtv, color, 1, &hostClear);
    }
    if ((control >> 9) & 1) // depth/stencil clear: depth 24 bits | stencil 8
    {
        const uint32_t v = gpureg::Get(RB_DEPTH_CLEAR), d = v >> 8;
        float depth = float(d) / 16777215.0f;
        if ((gpureg::Get(RB_DEPTH_INFO) >> 16) & 1)
        {
            // D24FS8: 20e4 float (4-bit exponent, bias 15, 20-bit mantissa).
            const uint32_t e = d >> 20, m = d & 0xFFFFF;
            depth = e ? std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15) : std::ldexp(float(m) / 1048576.0f, -14);
            depth = std::clamp(depth, 0.0f, 1.0f);
        }
        const D3D12_RECT hostClear = ToHost(clearRect);
        C().list->ClearDepthStencilView(C().dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, UINT8(v & 0xFF), 1, &hostClear);
    }
}
}

namespace
{
// ---- Lookahead: pipelines the command stream will need, found by GpuLookahead.
struct LookItem
{
    PipelineKey key;
    std::shared_ptr<std::vector<uint32_t>> vsCode, psCode; // only for shaders not translated yet
    std::shared_ptr<PipelineEntry> entry; // set once the build has been started
    bool dropped = false;
};
std::vector<LookItem> g_look;
std::unordered_set<PipelineKey, PipelineKeyHash> g_lookKeys;
uint64_t g_predVs = 0, g_predPs = 0;
const uint32_t *g_predVsCode = nullptr, *g_predPsCode = nullptr;
// Microcode read from guest memory, copied when the prediction is made: the pointer can
// point into memory the guest rewrites before the predicted draw is looked at, and then the
// bytes would no longer match the hash taken at the same moment.
std::shared_ptr<std::vector<uint32_t>> g_predVsOwn, g_predPsOwn;
uint32_t g_predVsDwords = 0, g_predPsDwords = 0;
size_t g_lookNext = 0;
uint32_t g_lookStarted = 0, g_lookFinished = 0;

// Starts what can be started (translating shaders as needed, queueing pipeline builds);
// returns true while anything is unfinished.
bool AdvanceLookahead(double budgetMs)
{
    const auto start = std::chrono::steady_clock::now();
    bool pending = false;
    for (size_t i = 0; i < g_look.size(); i++)
    {
        LookItem& it = g_look[i];
        if (it.dropped)
            continue;
        if (!it.entry)
        {
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budgetMs)
            {
                pending = true;
                continue;
            }
            const CompiledShader* vs = nullptr;
            const CompiledShader* ps = nullptr;
            if (auto f = g_r.shaders.find(it.key.vs); f != g_r.shaders.end())
                g_compiler.Promote(*(vs = f->second.get()));
            else
                vs = it.vsCode ? &GetShader(it.key.vs, *it.vsCode, false) : nullptr;
            if (auto f = g_r.shaders.find(it.key.ps); f != g_r.shaders.end())
                g_compiler.Promote(*(ps = f->second.get()));
            else
                ps = it.psCode ? &GetShader(it.key.ps, *it.psCode, true) : nullptr;
            if (!vs || !ps)
            {
                it.dropped = true;
                g_lookFinished++;
                continue;
            }
            if (vs->state.load() == CompiledShader::Failed || ps->state.load() == CompiledShader::Failed)
            {
                it.dropped = true;
                g_lookFinished++;
                continue;
            }
            if (vs->state.load() == CompiledShader::Pending || ps->state.load() == CompiledShader::Pending)
            {
                pending = true;
                continue;
            }
            it.entry = EnsurePipeline(*vs, *ps, it.key);
            g_lookStarted++;
        }
        if (it.entry && it.entry->state.load(std::memory_order_acquire) == 0)
            pending = true;
    }
    // Finished items leave the list so it stays short.
    size_t keep = 0;
    for (size_t i = 0; i < g_look.size(); i++)
    {
        const bool finished = g_look[i].dropped || (g_look[i].entry && g_look[i].entry->state.load(std::memory_order_acquire) != 0);
        if (finished && !g_look[i].dropped)
            g_lookFinished++;
        if (!finished)
            g_look[keep++] = std::move(g_look[i]);
    }
    g_look.resize(keep);
    return pending || !g_look.empty();
}

// Shaders the memory scan found (by renderer hash), to see how much of a race it covers.
std::unordered_set<uint64_t> g_scanKnown;
uint32_t g_scanPrefound = 0, g_scanMissed = 0;

// Hands shaders the scanner found to the compile queue (translation runs here, on the
// command processor thread, so the budget keeps it from eating a frame).
void DrainScan(double budgetMs, std::vector<const CompiledShader*>* queued = nullptr)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<ScannedShader> found;
    while (true)
    {
        found.clear();
        if (!ShaderScanTake(found, 8))
            return;
        for (ScannedShader& s : found)
        {
            // The scanner accepts a candidate on control flow alone, and some of what it finds in
            // the mesh files is not a shader (ucode::Plausible: an opcode the ISA doesn't define,
            // or instructions past the end). Warming one costs a background compile and logs a
            // translation failure for a hash no draw will ever use.
            if (!ucode::Plausible(ucode::Parse(s.code.data(), uint32_t(s.code.size()), s.pixel)))
                continue;
            const uint64_t hash = Fnv(s.code.data(), s.code.size() * 4) ^ (s.pixel ? 1 : 0);
            g_scanKnown.insert(hash);
            // Speculative (and partly stale memory): compiled when nothing urgent waits,
            // promoted if a draw turns out to need it.
            const CompiledShader& cs = GetShader(hash, s.code, s.pixel, true);
            if (queued && cs.state.load() == CompiledShader::Pending)
                queued->push_back(&cs);
        }
        if (budgetMs > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budgetMs)
            return;
    }
}

// ---- Hold: while the command processor waits for a compile it keeps presenting the
// last frame with the overlay on top, so the screen doesn't just freeze.
struct Hold
{
    bool active = false;
    ComPtr<ID3D12Resource> image; // last frame shown (kept alive here)
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> upload;
    uint8_t* uploadCpu = nullptr;
    std::chrono::steady_clock::time_point lastPresent{};
} g_hold;

void HoldPresent();
void SubmitLists(UINT n, ID3D12CommandList* const* lists);

// Spins until done() is true. Presents the overlay roughly 8 times a second once the wait
// has lasted 150 ms.


void WaitHoldingImpl(const std::function<bool()>& done)
{
    if (done())
        return;
    const auto waitStart = std::chrono::steady_clock::now();
    struct CountWait
    {
        std::chrono::steady_clock::time_point start;
        ~CountWait()
        {
            g_frameEvents.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            g_frameEvents.waits++;
        }
    } countWait{ waitStart };
    // A stall: look at everything queued behind this draw and build it all at once
    // (in parallel), then wait for the whole set, not just this draw's pipeline.
    static auto lastLook = std::chrono::steady_clock::time_point{};
    const auto start = std::chrono::steady_clock::now();
    bool announced = false;
    // Not once the game runs: the scan alone takes ~18 ms and the wait is usually shorter.
    if (!g_compiler.park.load())
    {
        ShaderScanNow(std::max(2u, std::thread::hardware_concurrency() / 2));
        DrainScan(0);
    }
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLook > std::chrono::milliseconds(400))
        {
            lastLook = now;
            GpuLookahead();
        }
        const bool lookPending = AdvanceLookahead(4.0);
        if (done() && !lookPending && g_compiler.jobs.load() == 0)
            break;
        if (now - start > std::chrono::seconds(180))
            break;
        // Nothing legitimate takes this long: say what is still pending and where every
        // thread is, once per wait.
        static std::chrono::steady_clock::time_point reported{};
        if (now - start > std::chrono::seconds(20) && reported != start)
        {
            reported = start;
            int queued = 0, background = 0, parked = 0;
            {
                std::lock_guard lock(g_compiler.mutex);
                queued = int(g_compiler.queue.size());
                background = int(g_compiler.background.size());
                parked = int(g_compiler.parked.size());
            }
            Log("hang: waiting %.0f s on %s: done %d, lookahead pending %d (%zu items), compile jobs %d (queued %d, background %d, parked %d), "
                "pipeline builds in flight %d",
                std::chrono::duration<double>(now - start).count(), g_waitWhat, int(done()), int(lookPending), g_look.size(),
                g_compiler.jobs.load(), queued, background, parked, g_stats.busy.load());
            LogAllHostThreadStacks("renderer wait over 20 s");
        }
        if (now - start > std::chrono::milliseconds(150))
        {
            g_hold.active = true;
            announced = true;
            DrainScan(0);
            if (now - g_hold.lastPresent > std::chrono::milliseconds(33))
                HoldPresent();
        }
        Sleep(1);
    }
    if (announced)
        g_hold.active = false;
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
        g_lastLongHold = std::chrono::steady_clock::now();
    if (getenv("FM2_PERF"))
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms > 20)
            Log("perf: stall of %.0f ms on %s (blend %08X, lookahead started %u, finished %u)", ms, g_waitWhat, g_waitBlend, g_lookStarted, g_lookFinished);
    }
}

// Lookahead hooks, called from GpuLookahead on shadow register state.
void PredictBeginImpl()
{
    g_predVs = C().vsHash;
    g_predPs = C().psHash;
    g_predVsCode = C().vsCode.data();
    g_predPsCode = C().psCode.data();
    g_predVsOwn.reset(); // the command list's own copy stays valid
    g_predPsOwn.reset();
    g_predVsDwords = uint32_t(C().vsCode.size());
    g_predPsDwords = uint32_t(C().psCode.size());
}

// ---- Warm-up: rebuilds every shader/pipeline recorded in the manifest in the
// background (on this thread a few ms per frame) while the game boots and sits in menus.
void WarmupStep()
{
    DrainScan(3.0);
    static LiveSwitch off("FM2_NO_WARMUP");
    if (off || g_manifest.next >= g_manifest.pending.size())
        return;
    const auto start = std::chrono::steady_clock::now();
    int skipped = 0;
    while (g_manifest.next < g_manifest.pending.size() && g_stats.busy.load() < 12)
    {
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > 4.0)
            break;
        const PipelineKey key = g_manifest.pending[g_manifest.next];
        if (g_r.pipelines.count(key))
        {
            g_manifest.next++;
            g_stats.warmDone++;
            continue;
        }
        std::vector<uint32_t> vsCode, psCode;
        if (!Manifest::LoadShader(false, key.vs, vsCode) || !Manifest::LoadShader(true, key.ps, psCode))
        {
            g_manifest.next++;
            g_stats.warmDone++;
            continue;
        }
        const CompiledShader& vs = GetShader(key.vs, vsCode, false);
        const CompiledShader& ps = GetShader(key.ps, psCode, true);
        if (vs.state.load() == CompiledShader::Pending || ps.state.load() == CompiledShader::Pending)
        {
            // Still compiling: look further ahead instead of waiting on it.
            std::rotate(g_manifest.pending.begin() + g_manifest.next, g_manifest.pending.begin() + g_manifest.next + 1, g_manifest.pending.end());
            if (++skipped > 64)
                break;
            continue;
        }
        g_manifest.next++;
        g_stats.warmDone++;
        if (vs.IsReady() && ps.IsReady())
            EnsurePipeline(vs, ps, key);
    }
}

// ---- Overlay: a small panel in the corner while shaders are compiling.
constexpr UINT kOverlayW = 520, kOverlayH = 140;
struct Overlay
{
    ComPtr<ID3D12Resource> texture;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    std::chrono::steady_clock::time_point lastRender{};
    std::string lastText;
    bool visible = false;
} g_overlay;

bool OverlayWanted(std::vector<std::string>& lines)
{
    static LiveSwitch disabled("FM2_NO_OVERLAY");
    if (disabled)
        return false;
    const uint64_t nowMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool recentCold = g_stats.lastColdMs.load() && nowMs - g_stats.lastColdMs.load() < 4000;
    const bool warming = g_stats.warmTotal.load() && g_stats.warmDone.load() < g_stats.warmTotal.load();
    if (g_stats.busy.load() == 0 && !recentCold && !warming && !g_hold.active)
        return false;
    char l[160];
    lines.push_back(g_hold.active ? "FM2  preparing shaders - please wait" : "FM2  shader compiler");
    if (g_stats.busy.load() > 0)
    {
        std::lock_guard lock(g_stats.mutex);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_stats.currentSince).count();
        snprintf(l, sizeof(l), "Compiling %s  (%.1f s, %d in progress)", g_stats.current.c_str(), sec, g_stats.busy.load());
        lines.push_back(l);
    }
    else
        lines.push_back("Idle");
    snprintf(l, sizeof(l), "Shaders    %u compiled, %u from cache", g_stats.shadersBuilt.load(), g_stats.shadersCached.load());
    lines.push_back(l);
    snprintf(l, sizeof(l), "Pipelines  %u built, %u from cache", g_stats.pipelinesBuilt.load(), g_stats.pipelinesLoaded.load());
    lines.push_back(l);
    if (g_hold.active)
    {
        snprintf(l, sizeof(l), "This race  %u pipelines found, %u done, %zu waiting", g_lookStarted + uint32_t(g_look.size()), g_lookFinished, g_look.size());
        lines.push_back(l);
    }
    if (g_stats.warmTotal.load())
    {
        snprintf(l, sizeof(l), "Warm-up    %u / %u pipelines", std::min(g_stats.warmDone.load(), g_stats.warmTotal.load()), g_stats.warmTotal.load());
        lines.push_back(l);
    }
    if (g_stats.staticTotal.load())
    {
        snprintf(l, sizeof(l), "First boot %u / %u game shaders compiled", std::min(g_stats.staticDone.load(), g_stats.staticTotal.load()), g_stats.staticTotal.load());
        lines.push_back(l);
    }
    return true;
}

// Renders the lines with GDI into RGBA pixels.
void RenderOverlayPixels(const std::vector<std::string>& lines, std::vector<uint32_t>& pixels)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kOverlayW;
    bi.bmiHeader.biHeight = -int(kOverlayH);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT all{ 0, 0, LONG(kOverlayW), LONG(kOverlayH) };
    HBRUSH bg = CreateSolidBrush(RGB(14, 16, 22)), border = CreateSolidBrush(RGB(255, 140, 0));
    FillRect(dc, &all, border);
    RECT inner{ 2, 2, LONG(kOverlayW) - 2, LONG(kOverlayH) - 2 };
    FillRect(dc, &inner, bg);
    HFONT font = CreateFontW(19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < lines.size(); i++)
    {
        SetTextColor(dc, i == 0 ? RGB(255, 170, 40) : RGB(235, 235, 235));
        TextOutA(dc, 12, 8 + int(i) * 22, lines[i].c_str(), int(lines[i].size()));
    }
    GdiFlush();
    pixels.resize(size_t(kOverlayW) * kOverlayH);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < pixels.size(); i++)
    {
        const uint32_t p = src[i]; // 0x00RRGGBB
        pixels[i] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bg);
    DeleteObject(border);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// Records the overlay upload (when its text changed) and returns the texture to
// copy onto the back buffer, or null when nothing is showing.
struct OverlayUpload
{
    ID3D12Resource* resource = nullptr;
    uint8_t* cpu = nullptr;
    UINT64 offset = 0;
};

// upload(bytes) returns staging space (resource + CPU pointer + offset) or a null resource.
ID3D12Resource* PrepareOverlayOn(ID3D12GraphicsCommandList* list, const std::function<OverlayUpload(UINT64)>& upload, bool force)
{
    std::vector<std::string> lines;
    g_overlay.visible = OverlayWanted(lines);
    if (!g_overlay.visible)
        return nullptr;
    std::string text;
    for (auto& l : lines)
        text += l + "\n";
    const auto now = std::chrono::steady_clock::now();
    const bool stale = !g_overlay.texture || (text != g_overlay.lastText && (force || now - g_overlay.lastRender > std::chrono::milliseconds(200)));
    if (stale)
    {
        if (!g_overlay.texture)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = kOverlayW;
            td.Height = kOverlayH;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_overlay.texture))))
                return nullptr;
            g_overlay.state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        std::vector<uint32_t> pixels;
        RenderOverlayPixels(lines, pixels);
        static LiveSwitch dumpOverlay("FM2_OVERLAY_DUMP");
        if (dumpOverlay)
        {
            std::vector<uint8_t> rgba(pixels.size() * 4);
            memcpy(rgba.data(), pixels.data(), rgba.size());
            WritePng("overlay.png", kOverlayW, kOverlayH, rgba);
        }
        const UINT pitch = kOverlayW * 4;
        const UINT alignedPitch = (pitch + 255) & ~255u;
        const OverlayUpload staging = upload(UINT64(alignedPitch) * kOverlayH);
        if (!staging.resource)
            return nullptr;
        for (UINT y = 0; y < kOverlayH; y++)
            memcpy(staging.cpu + staging.offset + UINT64(y) * alignedPitch, pixels.data() + size_t(y) * kOverlayW, pitch);
        if (g_overlay.state != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_overlay.texture.Get();
            b.Transition.Subresource = 0;
            b.Transition.StateBefore = g_overlay.state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = g_overlay.texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = staging.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = staging.offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kOverlayW, kOverlayH, 1, alignedPitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_overlay.texture.Get();
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        g_overlay.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_overlay.lastText = text;
        g_overlay.lastRender = now;
    }
    return g_overlay.state == D3D12_RESOURCE_STATE_COPY_SOURCE ? g_overlay.texture.Get() : nullptr;
}

// The frame path stages through the frame's upload ring.
ID3D12Resource* PrepareOverlay()
{
    return PrepareOverlayOn(C().list->Real(), [](UINT64 bytes) {
        OverlayUpload u;
        const UINT64 offset = RingAlloc(bytes, 512);
        if (offset == ~0ull)
            return u;
        u.resource = Frame().ring.Get();
        u.cpu = Frame().ringCpu;
        u.offset = offset;
        return u;
    }, false);
}

// Presents the last frame again with the overlay on top, on a separate command list
// (the frame's own list is mid-recording while the command processor waits).
void HoldPresent()
{
    g_hold.lastPresent = std::chrono::steady_clock::now();
    if (!g_hold.image || !g_r.device)
        return;
    if (!g_hold.list)
    {
        if (FAILED(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_hold.allocator))) ||
            FAILED(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_hold.allocator.Get(), nullptr, IID_PPV_ARGS(&g_hold.list))))
            return;
        g_hold.list->Close();
        const UINT64 bytes = UINT64((kOverlayW * 4 + 255) & ~255u) * kOverlayH + 1024;
        g_hold.upload = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        g_hold.upload->Map(0, nullptr, &mapped);
        g_hold.uploadCpu = static_cast<uint8_t*>(mapped);
    }
    // Everything the main queue still runs must be done before this list reuses its allocator.
    g_hold.allocator->Reset();
    g_hold.list->Reset(g_hold.allocator.Get(), nullptr);
    ID3D12Resource* overlay = PrepareOverlayOn(g_hold.list.Get(), [](UINT64) {
        OverlayUpload u;
        u.resource = g_hold.upload.Get();
        u.cpu = g_hold.uploadCpu;
        return u;
    }, true);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_hold.image.Get();
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_hold.list->ResourceBarrier(1, &b);
    const bool ok = PresenterRecordCopy(g_hold.list.Get(), g_hold.image.Get(), overlay, kOverlayW, kOverlayH);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_hold.list->ResourceBarrier(1, &b);
    g_hold.list->Close();
    if (!ok)
        return;
    ID3D12CommandList* lists[] = { g_hold.list.Get() };
    SubmitLists(1, lists);
    PresenterPresent();
    // Wait for it: the allocator and upload buffer are reused at the next hold.
    const UINT64 value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    if (g_r.fence->GetCompletedValue() < value)
    {
        g_r.fence->SetEventOnCompletion(value, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, 500);
    }
}
}

// Puts a full 1280x720 RGBA8 frame (the swap chain size) on screen: the first-boot screen,
// shown before the game has drawn anything that HoldPresent could repeat.
void BootPresent(const std::vector<uint8_t>& rgba)
{
    constexpr uint32_t gw = 1280, gh = 720;
    const uint32_t w = OutputWidth(), h = OutputHeight(); // the swap chain size
    static ComPtr<ID3D12Resource> texture, upload;
    static uint8_t* uploadCpu = nullptr;
    if (!g_r.device || rgba.size() != size_t(gw) * gh * 4)
        return;
    if (!g_hold.list)
    {
        if (FAILED(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_hold.allocator))) ||
            FAILED(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_hold.allocator.Get(), nullptr, IID_PPV_ARGS(&g_hold.list))))
            return;
        g_hold.list->Close();
        const UINT64 bytes = UINT64((kOverlayW * 4 + 255) & ~255u) * kOverlayH + 1024;
        g_hold.upload = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        g_hold.upload->Map(0, nullptr, &mapped);
        g_hold.uploadCpu = static_cast<uint8_t*>(mapped);
    }
    if (!texture)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_r.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
            return;
        upload = CreateBuffer(UINT64((w * 4 + 255) & ~255u) * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        upload->Map(0, nullptr, &mapped);
        uploadCpu = static_cast<uint8_t*>(mapped);
    }
    // Nearest-neighbour to the output size, centred at 16:9 on wider outputs with the
    // background's edge columns carried out to the sides.
    const uint32_t pitch = (w * 4 + 255) & ~255u;
    const uint32_t contentW = std::min(w, h * gw / gh), left = (w - contentW) / 2;
    for (uint32_t y = 0; y < h; y++)
    {
        const uint32_t* in = reinterpret_cast<const uint32_t*>(rgba.data()) + size_t(y * gh / h) * gw;
        uint32_t* out = reinterpret_cast<uint32_t*>(uploadCpu + size_t(y) * pitch);
        for (uint32_t x = 0; x < w; x++)
        {
            const uint32_t cx = x < left ? 0 : std::min(contentW - 1, x - left);
            out[x] = in[size_t(cx) * gw / contentW];
        }
    }
    g_hold.allocator->Reset();
    g_hold.list->Reset(g_hold.allocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, (w * 4 + 255) & ~255u };
    g_hold.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texture.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_hold.list->ResourceBarrier(1, &b);
    const bool ok = PresenterRecordCopy(g_hold.list.Get(), texture.Get());
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_hold.list->ResourceBarrier(1, &b);
    g_hold.list->Close();
    if (!ok)
        return;
    ID3D12CommandList* lists[] = { g_hold.list.Get() };
    SubmitLists(1, lists);
    PresenterPresent();
    const UINT64 value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    if (g_r.fence->GetCompletedValue() < value)
    {
        g_r.fence->SetEventOnCompletion(value, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, 500);
    }
}

void RendererPredictBegin()
{
    PredictBeginImpl();
}

void RendererPredictShader(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords)
{
    dwords = ucode::UsedDwords(microcodeBE, dwords);
    auto& hash = type == 0 ? g_predVs : g_predPs;
    auto& code = type == 0 ? g_predVsCode : g_predPsCode;
    auto& own = type == 0 ? g_predVsOwn : g_predPsOwn;
    auto& count = type == 0 ? g_predVsDwords : g_predPsDwords;
    hash = 0;
    count = 0;
    own.reset();
    // The signal can name a shader the command processor has not loaded yet, so the
    // microcode may still be uninitialised or already reused: only predict one that decodes.
    if (!dwords || !ucode::Plausible(ucode::Parse(microcodeBE, dwords, type != 0)))
        return;
    hash = Fnv(microcodeBE, dwords * 4) ^ type;
    own = std::make_shared<std::vector<uint32_t>>(microcodeBE, microcodeBE + dwords);
    code = own->data();
    count = dwords;
}

void RendererPredictDraw(uint32_t initiator)
{
    const uint32_t edramMode = gpureg::Get(RB_MODECONTROL) & 7;
    if ((edramMode != 4 && edramMode != 5) || !g_predVs || !g_predPs || !g_predVsDwords || !g_predPsDwords)
        return;
    const uint32_t prim = initiator & 0x3F;
    const PipelineKey key = MakePipelineKey(g_predVs, g_predPs, edramMode == 5, prim != 8 && prim != 1);
    if (g_r.pipelines.count(key) || !g_lookKeys.insert(key).second)
        return;
    LookItem item{ key, nullptr, nullptr, nullptr, false };
    if (!g_r.shaders.count(g_predVs))
        item.vsCode = g_predVsOwn ? g_predVsOwn : std::make_shared<std::vector<uint32_t>>(g_predVsCode, g_predVsCode + g_predVsDwords);
    if (!g_r.shaders.count(g_predPs))
        item.psCode = g_predPsOwn ? g_predPsOwn : std::make_shared<std::vector<uint32_t>>(g_predPsCode, g_predPsCode + g_predPsDwords);
    g_look.push_back(std::move(item));
}

bool RendererCompileIdle()
{
    const uint64_t nowMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool warming = (g_stats.warmTotal.load() && g_stats.warmDone.load() < g_stats.warmTotal.load()) || g_stats.staticTotal.load();
    return g_stats.busy.load() == 0 && !warming && nowMs - g_stats.lastColdMs.load() > 2000;
}

void RendererSaveCaches()
{
    g_diskWriter.Flush();
    g_pipelineLibrary.MaybeSave(true);
}

void RendererSetShader(uint32_t type, const uint32_t* microcodeBE, uint32_t usedDwords, uint64_t hash)
{
    std::vector<uint32_t>& code = type == 0 ? C().vsCode : C().psCode;
    code.assign(microcodeBE, microcodeBE + usedDwords);
    (type == 0 ? C().vsHash : C().psHash) = hash;
}

void RendererOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords)
{
    std::vector<uint32_t>& code = type == 0 ? C().vsCode : C().psCode;
    code.assign(microcodeBE, microcodeBE + ucode::UsedDwords(microcodeBE, dwords));
    (type == 0 ? C().vsHash : C().psHash) = Fnv(code.data(), code.size() * 4) ^ type;
}

namespace
{
void TraceSkip(const char* why, uint32_t initiator)
{
    const uint64_t traceFrame = TraceFrame();
    if (traceFrame && g_r.frame + 1 == traceFrame)
        Log("skip draw (%s): prim %u count %u mode %u vs %016llx ps %016llx", why, initiator & 0x3F, initiator >> 16,
            gpureg::Get(RB_MODECONTROL) & 7, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
}
}

namespace
{
// FM2_TRACE_DIRECT: in-place vertex buffers of depth-only draws, by address (see DrawImpl).
struct DirectTrace
{
    struct Entry
    {
        uint64_t frame = ~0ull, hash = 0, vs = 0;
        uint32_t bytes = 0;
    };
    std::mutex mutex;
    std::unordered_map<uint32_t, Entry> seen;
    uint32_t changed = 0, checked = 0;
} g_directTrace;

// Performance counters (FM2_PERF=1 logs them once a second).
struct Perf
{
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    double drawSeconds = 0, swapSeconds = 0;
    double section[8] = {};
    double sub[8] = {}; // the constants section split up (Sub)
    std::chrono::steady_clock::time_point subMark;
    uint64_t tablesBuilt = 0, tablesReused = 0;
    std::chrono::steady_clock::time_point mark;
    uint64_t frames = 0, draws = 0, slowFrames = 0;
    double maxFrame = 0;
} g_perf;
void DrawImpl(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
}


// ---- Parallel draw pieces --------------------------------------------------------------
namespace
{
std::mutex g_poolMutex;

ID3D12GraphicsCommandList* AcquireList()
{
    FrameResources& f = Frame();
    std::lock_guard lock(g_poolMutex);
    if (f.poolUsed == f.pool.size())
    {
        auto& e = f.pool.emplace_back();
        Check(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e.first)), "CreateCommandAllocator(piece)");
        Check(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, e.first.Get(), nullptr, IID_PPV_ARGS(&e.second)),
            "CreateCommandList(piece)");
    }
    else
    {
        auto& e = f.pool[f.poolUsed];
        e.first->Reset();
        e.second->Reset(e.first.Get(), nullptr);
    }
    return f.pool[f.poolUsed++].second.Get();
}

// The state every draw list starts with.
void ListPrologue(RecordedList& list)
{
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    list.SetDescriptorHeaps(1, heaps);
    list.SetGraphicsRootSignature(g_r.rootSignature.Get());
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// Command lists closed but not yet submitted, in order. Parallel segments end the main list and add
// their pieces several dozen times a frame; submitted one segment at a time, each submission left the
// GPU idle a moment (GPU time per frame rose by a third). They are queued here and go with the next
// real submission (the frame's, or one that must reach the GPU now). FM2_NO_DEFER_LISTS=1: at once.
std::mutex g_pendingMutex;
std::vector<ID3D12CommandList*> g_pendingLists;

// Submits the queued lists, then these.
void SubmitLists(UINT n, ID3D12CommandList* const* lists)
{
    std::lock_guard lock(g_pendingMutex);
    g_pendingLists.insert(g_pendingLists.end(), lists, lists + n);
    if (!g_pendingLists.empty())
        g_r.queue->ExecuteCommandLists(UINT(g_pendingLists.size()), g_pendingLists.data());
    g_pendingLists.clear();
}

// Queues lists for the next submission (or submits them, with FM2_NO_DEFER_LISTS).
void QueueLists(UINT n, ID3D12CommandList* const* lists)
{
    static const bool now = getenv("FM2_NO_DEFER_LISTS") != nullptr;
    if (now)
        return SubmitLists(n, lists);
    std::lock_guard lock(g_pendingMutex);
    g_pendingLists.insert(g_pendingLists.end(), lists, lists + n);
}

// Ends the main list so far (queued for submission) and continues the frame in a fresh one.
void EndOcclusionQuery();

void SplitMainList()
{
    // A query must end in the list it began in.
    if (g_occlusionOpen)
        EndOcclusionQuery();
    ID3D12GraphicsCommandList* done = g_r.recorder.Real();
    done->Close();
    ID3D12CommandList* lists[] = { done };
    QueueLists(1, lists);
    g_r.recorder.Rebind(AcquireList());
    ListPrologue(g_r.recorder);
    g_drawState.Invalidate();
    SelectSurfaces(true);
}

// Waits until everything submitted to the queue so far has finished.
void WaitForQueueIdle()
{
    SubmitLists(0, nullptr); // (queued lists first: the signal follows them)
    const UINT64 value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    if (g_r.fence->GetCompletedValue() < value)
    {
        g_r.fence->SetEventOnCompletion(value, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, 2000);
    }
}

void EndOcclusionQuery()
{
    g_occlusionOpen = false;
    C().list->EndQuery(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex);
    C().list->ResolveQueryData(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex, 1, g_occlusionReadback.Get(), UINT64(g_occlusionIndex) * 8);
    g_occlusionPending[g_r.frameIndex].push_back({ g_occlusionOrder, g_occlusionIndex });
}

// The depth around the screen centre (Photo Mode's focus reticle), written to guest memory as the
// console stores a D24FS8 copy: tiled 32-bit words, the 20e4 float depth in the top 24 bits. The
// box is copied to a readback buffer with the frame and written when that frame slot comes round
// again (a frame or two late, no stall: the full image with a wait per tile ran Photo Mode at 20 fps).
constexpr uint32_t kFocusX0 = 512, kFocusX1 = 768, kFocusY0 = 296, kFocusY1 = 424; // game pixels
struct DepthWriteback
{
    ComPtr<ID3D12Resource> buffer;
    UINT pitch = 0, hostX0 = 0, hostY0 = 0, hostW = 0, hostH = 0;
    uint32_t base = 0, width = 0, height = 0, info = 0, y0 = 0, y1 = 0;
    bool pending = false;
};
DepthWriteback g_depthWriteback[kFrames];

void FinishDepthWriteback(UINT frameIndex)
{
    DepthWriteback& w = g_depthWriteback[frameIndex];
    if (!w.pending)
        return;
    w.pending = false;
    const uint8_t* pixels = nullptr;
    if (FAILED(w.buffer->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))) || !pixels)
        return;
    auto encode = [](float v) -> uint32_t {
        v = std::clamp(v, 0.0f, 1.0f);
        if (v < std::ldexp(1.0f, -14))
            return std::min<uint32_t>(uint32_t(std::lround(std::ldexp(double(v), 34))), 0xFFFFF); // denormal: m / 2^20 * 2^-14
        int e = 0;
        (void)std::frexp(double(v), &e); // v = f * 2^e, f in [0.5, 1)
        const int exponent = std::clamp(e - 1 + 15, 1, 15);
        const double mantissa = std::ldexp(double(v), 15 - exponent) - 1.0;
        return uint32_t(exponent) << 20 | std::min<uint32_t>(uint32_t(std::lround(mantissa * 1048576.0)), 0xFFFFF);
    };
    const uint32_t endian = w.info & 7, pitchTexels = (w.width + 31) & ~31u;
    uint8_t* guest = GuestPtr<uint8_t>(GpuPhysToGuest(w.base));
    const double sx = double(w.hostW) / double(kFocusX1 - kFocusX0), sy = double(w.hostH) / double(w.y1 - w.y0);
    for (uint32_t y = w.y0; y < w.y1; y++)
    {
        const float* line = reinterpret_cast<const float*>(pixels + size_t(std::min<double>((y - w.y0) * sy, w.hostH - 1)) * w.pitch);
        for (uint32_t x = kFocusX0; x < kFocusX1; x++)
        {
            const uint32_t word = encode(line[std::min<size_t>(size_t((x - kFocusX0) * sx), w.hostW - 1)]) << 8;
            const uint32_t stored = endian == 0 ? word : __builtin_bswap32(word);
            memcpy(guest + TextureTiledOffset(x, y, pitchTexels, 2), &stored, 4);
        }
    }
    const D3D12_RANGE none{ 0, 0 };
    w.buffer->Unmap(0, &none);
    if (auto it = g_r.resolved.find(w.base); it != g_r.resolved.end())
        NoteResolvedMemory(w.base, it->second); // our own write: not a reuse of the memory
}

void WriteDepthToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info, const D3D12_RECT& rect)
{
    const auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource || it->second.format != DXGI_FORMAT_R32_FLOAT || width < kFocusX1 || height < kFocusY1)
        return;
    // Only the tile that holds the box (rows of this resolve).
    const uint32_t y0 = std::max<uint32_t>(kFocusY0, uint32_t(std::max<LONG>(rect.top, 0)));
    const uint32_t y1 = std::min<uint32_t>(kFocusY1, uint32_t(std::max<LONG>(rect.bottom, 0)));
    if (y1 <= y0)
        return;
    ResolvedTexture& r = it->second;
    DepthWriteback& w = g_depthWriteback[g_r.frameIndex];
    w.hostX0 = UINT(uint64_t(kFocusX0) * r.hostWidth / width);
    w.hostW = std::max<UINT>(1, UINT(uint64_t(kFocusX1) * r.hostWidth / width) - w.hostX0);
    w.hostY0 = UINT(uint64_t(y0) * r.hostHeight / height);
    w.hostH = std::max<UINT>(1, std::min<UINT>(UINT(uint64_t(y1) * r.hostHeight / height), r.hostHeight) - w.hostY0);
    w.pitch = (w.hostW * 4 + 255) & ~255u;
    const UINT64 bytes = UINT64(w.pitch) * w.hostH;
    if (!w.buffer || w.buffer->GetDesc().Width < bytes)
        w.buffer = CreateBuffer(std::max<UINT64>(bytes, 1u << 20), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r.resource.Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    C().list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = w.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, w.hostW, w.hostH, 1, w.pitch };
    src.pResource = r.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{ w.hostX0, w.hostY0, 0, w.hostX0 + w.hostW, w.hostY0 + w.hostH, 1 };
    C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    C().list->ResourceBarrier(1, &bar);
    w.base = base;
    w.width = width;
    w.height = height;
    w.info = info;
    w.y0 = y0;
    w.y1 = y1;
    w.pending = true;
    g_lastFocusFrame = g_r.frame;
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("resolve: Photo Mode focus: depth around the centre written back to guest memory at %08X", base);
    }
}

uint64_t g_lastFocusFrame = ~0ull >> 1;

// Photo Mode's picture, copied with its frame and written to memory (tiled A8R8G8B8) when the
// frame slot comes round again: no stall (a wait every frame ran Photo Mode at 20 fps).
struct PhotoWriteback
{
    ComPtr<ID3D12Resource> buffer;
    UINT pitch = 0, hostW = 0, hostH = 0;
    uint32_t base = 0, width = 0, height = 0, info = 0;
    bool pending = false;
};
PhotoWriteback g_photoWriteback[kFrames];

void FinishPhotoWriteback(UINT frameIndex)
{
    PhotoWriteback& w = g_photoWriteback[frameIndex];
    if (!w.pending)
        return;
    w.pending = false;
    uint8_t* pixels = nullptr;
    if (FAILED(w.buffer->Map(0, nullptr, reinterpret_cast<void**>(&pixels))) || !pixels)
        return;
    const uint32_t endian = w.info & 7, pitchTexels = (w.width + 31) & ~31u;
    // The saver byte-swaps the words itself and reads them with no fetch swizzle: with the copy
    // swap bit set, red stays in bits 16-23 (the other way round, photos had red and blue exchanged).
    const bool swapRB = ((w.info >> 24) & 1) == 0;
    uint8_t* guest = GuestPtr<uint8_t>(GpuPhysToGuest(w.base));
    for (uint32_t y = 0; y < w.height; y++)
    {
        const uint8_t* line = pixels + size_t(y * w.hostH / w.height) * w.pitch;
        for (uint32_t x = 0; x < w.width; x++)
        {
            const uint8_t* px = line + size_t(x * w.hostW / w.width) * 4;
            const uint32_t red = swapRB ? px[2] : px[0], green = px[1], blue = swapRB ? px[0] : px[2];
            const uint32_t word = uint32_t(px[3]) << 24 | red << 16 | green << 8 | blue;
            const uint32_t stored = endian == 0 ? word : __builtin_bswap32(word);
            memcpy(guest + TextureTiledOffset(x, y, pitchTexels, 2), &stored, 4);
        }
    }
    const D3D12_RANGE none{ 0, 0 };
    w.buffer->Unmap(0, &none);
    if (auto it = g_r.resolved.find(w.base); it != g_r.resolved.end())
        NoteResolvedMemory(w.base, it->second); // our own write: not a reuse of the memory
}

void WritePhotoToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info)
{
    const auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource || it->second.format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return;
    ResolvedTexture& r = it->second;
    PhotoWriteback& w = g_photoWriteback[g_r.frameIndex];
    w.hostW = r.hostWidth;
    w.hostH = r.hostHeight;
    w.pitch = (w.hostW * 4 + 255) & ~255u;
    const UINT64 bytes = UINT64(w.pitch) * w.hostH;
    if (!w.buffer || w.buffer->GetDesc().Width < bytes)
        w.buffer = CreateBuffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r.resource.Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    C().list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = w.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w.hostW, w.hostH, 1, w.pitch };
    src.pResource = r.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    C().list->ResourceBarrier(1, &bar);
    w.base = base;
    w.width = width;
    w.height = std::min(height, kHeight);
    w.info = info;
    w.pending = true;
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("resolve: Photo Mode picture written back to guest memory at %08X", base);
    }
}

void WriteResolveToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info)
{
    const auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource || it->second.format != DXGI_FORMAT_R8G8B8A8_UNORM || !width || !height)
        return;
    ResolvedTexture& r = it->second;
    // Run what has been recorded (the resolve's copy included) and let the GPU finish it.
    BeginFrameIfNeeded();
    SplitMainList();
    WaitForQueueIdle();
    const UINT pitch = (r.hostWidth * 4 + 255) & ~255u;
    ComPtr<ID3D12Resource> buffer = CreateBuffer(UINT64(pitch) * r.hostHeight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList* list = AcquireList();
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r.resource.Get();
    bar.Transition.Subresource = 0;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, r.hostWidth, r.hostHeight, 1, pitch };
    src.pResource = r.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    list->ResourceBarrier(1, &bar);
    list->Close();
    ID3D12CommandList* lists[] = { list };
    SubmitLists(1, lists);
    WaitForQueueIdle();
    uint8_t* pixels = nullptr;
    buffer->Map(0, nullptr, reinterpret_cast<void**>(&pixels));
    if (!pixels)
        return;
    // The surface in memory: tiled, 32-texel aligned, one A8R8G8B8 word per pixel (red and blue
    // exchanged when the copy has the swap bit; the title's fetch swizzle expects that), in the
    // byte order the copy's endian field names (2: big-endian words, 0: little-endian).
    const uint32_t endian = info & 7;
    // Everything written back here is read on the CPU (the garage thumbnail, the race livery), which
    // takes the words as they are, like the photo saver: red stays in bits 16-23 when the copy swaps
    // (written swapped, the livery's blue came out orange and red thumbnails blue).
    const bool swapRB = ((info >> 24) & 1) == 0;
    const uint32_t pitchTexels = (width + 31) & ~31u, rows = (height + 31) & ~31u;
    uint8_t* guest = GuestPtr<uint8_t>(GpuPhysToGuest(base));
    for (uint32_t y = 0; y < height; y++)
    {
        const uint8_t* line = pixels + size_t(y * r.hostHeight / height) * pitch;
        for (uint32_t x = 0; x < width; x++)
        {
            const uint8_t* px = line + size_t(x * r.hostWidth / width) * 4;
            const uint32_t red = swapRB ? px[2] : px[0], green = px[1], blue = swapRB ? px[0] : px[2], alpha = px[3];
            const uint32_t word = alpha << 24 | red << 16 | green << 8 | blue;
            uint8_t* out = guest + TextureTiledOffset(x, y, pitchTexels, 2);
            if (endian == 0)
                memcpy(out, &word, 4); // little-endian
            else
            {
                const uint32_t be = __builtin_bswap32(word);
                memcpy(out, &be, 4);
            }
        }
    }
    (void)rows;
    buffer->Unmap(0, nullptr);
    NoteResolvedMemory(base, r); // our own write: not a reuse of the memory
    Log("resolve: wrote %ux%u back to guest memory at %08X (endian %u, swap %d)", width, height, base, endian, int(swapRB));
}

struct Workers
{
    bool started = false;
    std::mutex mutex;
    std::condition_variable start, done;
    uint64_t generation = 0;
    const std::function<void(uint32_t)>* job = nullptr;
    uint32_t count = 0;
    std::atomic<uint32_t> next{ 0 }, finished{ 0 };
    std::vector<ID3D12GraphicsCommandList*> lists;
} g_workers;

// One piece on this thread, with a worker context of its own.
std::atomic<uint64_t> g_pieceNs{ 0 }, g_segmentNs{ 0 }, g_pieceCount{ 0 }, g_segmentCount{ 0 };
void TranslatePiece(DrawContext& ctx, uint32_t i)
{
    const auto t0 = std::chrono::steady_clock::now();
    ID3D12GraphicsCommandList* real = AcquireList();
    RecordedList recorder;
    recorder.Init(real, true);
    ctx = DrawContext{};
    ctx.list = &recorder;
    ctx.worker = true;
    DrawContext* saved = t_context;
    t_context = &ctx;
    ListPrologue(recorder);
    g_drawState.Invalidate();
    (*g_workers.job)(i);
    t_context = saved;
    real->Close();
    g_workers.lists[i] = real;
    g_pieceNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_pieceCount++;
}

}
HANDLE g_profiledWorker = nullptr; // FM2_PROFILE=w
namespace
{
void WorkerMain()
{
    SetThreadDescription(GetCurrentThread(), L"gpu draw piece worker");
    if (!g_profiledWorker)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_profiledWorker, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DrawContext ctx;
    uint64_t seen = 0;
    for (;;)
    {
        {
            std::unique_lock lock(g_workers.mutex);
            g_workers.start.wait(lock, [&] { return g_workers.generation != seen; });
            seen = g_workers.generation;
        }
        for (uint32_t i; (i = g_workers.next.fetch_add(1)) < g_workers.count;)
        {
            TranslatePiece(ctx, i);
            if (g_workers.finished.fetch_add(1) + 1 == g_workers.count)
            {
                std::lock_guard lock(g_workers.mutex);
                g_workers.done.notify_all();
            }
        }
    }
}
}

std::string CurrentHostStack(int maxFrames);
// Exceptions escaping a thread end the process silently: say where.
void InstallTerminateLog()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::set_terminate([] {
            std::string what = "?";
            if (auto e = std::current_exception())
                try { std::rethrow_exception(e); } catch (const std::exception& x) { what = x.what(); } catch (...) {}
            Log("CRASH: std::terminate (%s): %s", what.c_str(), CurrentHostStack(24).c_str());
            fflush(nullptr);
            std::abort();
        });
    });
}

uint32_t RendererDrawWorkers()
{
    InstallTerminateLog();
    static const uint32_t n = [] {
        const char* v = getenv("FM2_DRAW_WORKERS");
        const uint32_t hw = std::max(2u, std::thread::hardware_concurrency());
        (void)hw;
        // Off by default: measured slower than the serial path so far (per-draw cost roughly
        // doubles across threads and the command processor waits for each segment).
        return v ? std::min<uint32_t>(uint32_t(atoi(v)), hw) : 0u;
    }();
    return n;
}

bool RendererParallelReady()
{
    return RendererDrawWorkers() > 0 && g_r.ok && GameSettled() && GetSettings().upscaler == Settings::Upscaler::Off && !RendererTracing();
}

bool RendererIsResolveDraw()
{
    return (gpureg::Get(RB_MODECONTROL) & 7) == 6;
}

void RendererOcclusionBegin()
{
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
    if (g_occlusionFrame != g_r.frame)
    {
        g_occlusionFrame = g_r.frame;
        g_occlusionCount = 0;
    }
    if (g_occlusionOpen || g_occlusionCount >= kOcclusionPerFrame)
        return;
    if (!g_occlusionHeap)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        qd.Count = kOcclusionPerFrame * kFrames;
        g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_occlusionHeap));
        g_occlusionReadback = CreateBuffer(8ull * kOcclusionPerFrame * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_occlusionHeap || !g_occlusionReadback)
            return;
    }
    g_occlusionOrder = g_occlusionCount++;
    g_occlusionIndex = g_r.frameIndex * kOcclusionPerFrame + g_occlusionOrder;
    C().list->BeginQuery(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex);
    g_occlusionOpen = true;
}

bool RendererOcclusionEnd(uint32_t& samples)
{
    if (!g_occlusionOpen)
        return false;
    const UINT order = g_occlusionOrder;
    EndOcclusionQuery();
    if (order >= g_occlusionResult.size() || g_occlusionResult[order] < 0)
        return false;
    // Host samples to the console's: the output is ScaleX x ScaleY host pixels per game pixel
    // with g_msaa samples each; the guest surface has 1, 2 or 4 samples per pixel.
    const uint32_t guestMsaa = (gpureg::Get(RB_SURFACE_INFO) >> 16) & 3;
    const double perHost = double(1u << guestMsaa) / (double(ScaleX()) * double(ScaleY()) * double(g_msaa));
    samples = uint32_t(std::min(double(g_occlusionResult[order]) * perHost + 0.5, 4294967295.0));
    return true;
}

void RendererTrackTile(RendererTile& tile)
{
    const uint32_t mode = gpureg::Get(RB_MODECONTROL) & 7;
    if (mode != 4 && mode != 5)
        return;
    const D3D12_RECT scissor = WindowScissor();
    const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
    const int32_t wy = ((gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1) ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
    const bool full = scissor.left <= 0 && scissor.top <= 0 && scissor.right >= LONG(PassWidth()) && scissor.bottom >= LONG(PassHeight());
    if (!full)
    {
        tile.originY = -wy;
        tile.rect[0] = scissor.left;
        tile.rect[1] = scissor.top;
        tile.rect[2] = scissor.right;
        tile.rect[3] = scissor.bottom;
    }
}

void RendererSetTile(const RendererTile& tile)
{
    g_tile.originY = tile.originY;
    g_tile.rect = { tile.rect[0], tile.rect[1], tile.rect[2], tile.rect[3] };
}

void RendererRunPieces(uint32_t n, const std::function<void(uint32_t)>& translate)
{
    if (!n)
        return;
    const auto segmentStart = std::chrono::steady_clock::now();
    struct Done
    {
        std::chrono::steady_clock::time_point start;
        ~Done()
        {
            g_segmentNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            g_segmentCount++;
            static auto last = std::chrono::steady_clock::now();
            if (getenv("FM2_PERF") && std::chrono::steady_clock::now() - last > std::chrono::seconds(2))
            {
                last = std::chrono::steady_clock::now();
                Log("perf: parallel segments %llu: %.1f ms wall, %.1f ms in %llu pieces (%.2fx)", (unsigned long long)g_segmentCount.load(),
                    g_segmentNs / 1e6, g_pieceNs / 1e6, (unsigned long long)g_pieceCount.load(), double(g_pieceNs) / std::max<uint64_t>(1, g_segmentNs));
                g_segmentNs = g_pieceNs = g_pieceCount = g_segmentCount = 0;
            }
        }
    } done{ segmentStart };
    BeginFrameIfNeeded();
    SplitMainList();
    // Texture uploads of the pieces, executed before them.
    ID3D12GraphicsCommandList* prepReal = AcquireList();
    RecordedList prep;
    prep.Init(prepReal, true);
    g_prepList = &prep;
    {
        std::lock_guard lock(g_workers.mutex);
        if (!g_workers.started)
        {
            g_workers.started = true;
            // 16 MB stacks like the command processor's: shader translation and the HLSL
            // compiler can run on these threads, and they need far more than 1 MB.
            for (uint32_t i = 0; i < RendererDrawWorkers(); i++)
                if (HANDLE h = CreateThread(nullptr, 16u << 20, [](void*) -> DWORD { WorkerMain(); return 0; }, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr))
                    CloseHandle(h);
        }
        g_workers.job = &translate;
        g_workers.count = n;
        g_workers.next = 0;
        g_workers.finished = 0;
        g_workers.lists.assign(n, nullptr);
        g_workers.generation++;
    }
    g_workers.start.notify_all();
    // This thread takes pieces too.
    static DrawContext ownContext;
    for (uint32_t i; (i = g_workers.next.fetch_add(1)) < n;)
    {
        TranslatePiece(ownContext, i);
        g_workers.finished.fetch_add(1);
    }
    {
        std::unique_lock lock(g_workers.mutex);
        g_workers.done.wait(lock, [] { return g_workers.finished.load() >= g_workers.count; });
    }
    g_prepList = nullptr;
    prepReal->Close();
    std::vector<ID3D12CommandList*> lists;
    lists.push_back(prepReal);
    for (ID3D12GraphicsCommandList* l : g_workers.lists)
        lists.push_back(l);
    QueueLists(UINT(lists.size()), lists.data());
}

void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    if (g_gpuShaders)
        PassMark("vs %08X ps %08X", uint32_t(C().vsHash), uint32_t(C().psHash));
    if (!g_perfTiming)
    {
        DrawImpl(initiator, dmaBase, dmaSize);
        g_perf.draws++;
        g_passDraws++;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    DrawImpl(initiator, dmaBase, dmaSize);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_perf.drawSeconds += seconds;
    g_frameEvents.worstDrawMs = std::max(g_frameEvents.worstDrawMs, seconds * 1000.0);
    g_perf.draws++;
    g_passDraws++;
}

namespace
{
void Section(int i)
{
    if (!g_perfTiming)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (i > 0)
    {
        const double seconds = std::chrono::duration<double>(now - g_perf.mark).count();
        g_perf.section[i - 1] += seconds;
        g_frameEvents.section[i - 1] += seconds * 1000.0;
    }
    g_perf.mark = now;
}

// The constants section split up: Sub(-1) starts it, Sub(i) adds the time since the last call to part i.
void Sub(int i)
{
    if (!g_perfTiming)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (i >= 0)
        g_perf.sub[i] += std::chrono::duration<double>(now - g_perf.subMark).count();
    g_perf.subMark = now;
}

void DrawImpl(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    Section(0);
    g_drawSamplesResolve = false;
    LiveSwitch &noRender = g_sw_FM2_NO_RENDER;
    if (noRender)
        return;
    if (!g_r.ok)
        Init();
    const uint32_t edramMode = gpureg::Get(RB_MODECONTROL) & 7;
    if (edramMode == 6)
    {
        Resolve();
        return;
    }
    // Colour+depth (4) and depth-only (5) passes draw.
    if ((edramMode != 4 && edramMode != 5) || C().vsCode.empty() || C().psCode.empty())
    {
        TraceSkip("edram mode", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    {
        // A draw reached a shader we have not seen: a new race or car has loaded. Before the
        // static shader set existed this scanned guest memory for all of its shader objects
        // right away; now that set covers the game, and the 512 MB scan (~18 ms on this
        // thread) only showed as a frame spike, so once the game runs the low-priority
        // background scan is enough.
        const bool unseen = g_r.shaders.find(C().vsHash) == g_r.shaders.end() || g_r.shaders.find(C().psHash) == g_r.shaders.end();
        static auto lastScan = std::chrono::steady_clock::time_point{};
        LiveSwitch &noScan = g_sw_FM2_NO_SHADER_SCAN;
        if (unseen && !noScan)
        {
            if (g_scanKnown.count(C().vsHash) || g_scanKnown.count(C().psHash))
                g_scanPrefound++;
            else
                g_scanMissed++;
            if (!g_compiler.park.load() && std::chrono::steady_clock::now() - lastScan > std::chrono::milliseconds(300))
            {
                const auto t0 = std::chrono::steady_clock::now();
                ShaderScanNow(std::max(2u, std::thread::hardware_concurrency() / 2));
                lastScan = std::chrono::steady_clock::now();
                DrainScan(0);
                if (getenv("FM2_PERF"))
                    Log("perf: shader scan took %.0f ms, %llu shaders found in total", std::chrono::duration<double, std::milli>(lastScan - t0).count(),
                        (unsigned long long)ShaderScanTotal());
            }
        }
    }
    // A live edit of this pixel shader (PsEditAlias): this draw uses the alias, the context keeps the real hash.
    struct PsAlias
    {
        uint64_t real, realVs;
        PsAlias() : real(C().psHash), realVs(C().vsHash) { C().psHash = PsEditAlias(real); C().vsHash = PsEditAlias(realVs); }
        ~PsAlias() { C().psHash = real; C().vsHash = realVs; }
    } psAlias;
    const CompiledShader& vs = GetShader(C().vsHash, C().vsCode, false);
    if (g_drawStats.load(std::memory_order_relaxed) == 2) // the debug server's drawstats: every draw of a frame
    {
        std::lock_guard lock(g_drawStatsMutex);
        g_drawStatsCounts[{ psAlias.realVs, psAlias.real }]++;
    }
    const CompiledShader& ps = GetShader(C().psHash, C().psCode, true);
    Section(1);
    LiveSwitch &asyncShaders = g_sw_FM2_ASYNC_SHADERS;
    const bool shadersPending = vs.state.load(std::memory_order_acquire) == CompiledShader::Pending || ps.state.load(std::memory_order_acquire) == CompiledShader::Pending;
    if (shadersPending && SkippableDraw(edramMode == 5, gpureg::Get(RB_BLENDCONTROL0)))
    {
        g_skippedPipeline++;
        return; // compiling in the background
    }
    g_waitWhat = "shader";
    g_waitBlend = gpureg::Get(RB_BLENDCONTROL0);
    if (!asyncShaders)
        WaitHoldingImpl([&] {
            return vs.state.load(std::memory_order_acquire) != CompiledShader::Pending && ps.state.load(std::memory_order_acquire) != CompiledShader::Pending;
        });
    if (!vs.IsReady() || !ps.IsReady())
    {
        TraceSkip(vs.state == CompiledShader::Failed || ps.state == CompiledShader::Failed ? "shader" : "compiling", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    // A fetch from vertex fetch constant 96 or above (index field past the 96 slots) has no
    // vertex data. With the layout baked in, such shaders failed to compile and their draws
    // were skipped; read from vfl they compile and draw garbage (black shapes on Mugello).
    for (uint32_t k : vs.info.vertexFetchConstants)
        if (k >= 96)
        {
            TraceSkip("invalid vertex fetch constant", initiator);
            g_r.skippedThisFrame++;
            return;
        }
    // Triangle lists and restart-free strips read their indices on the GPU;
    // everything else is expanded here.
    const uint32_t prim = initiator & 0x3F, indexSource = (initiator >> 6) & 3, indexCount = initiator >> 16;
    const bool restartOn = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    LiveSwitch &noDirectIndex = g_sw_FM2_NO_DIRECT_INDEX;
    const bool directIndex = g_r.physical && !noDirectIndex && (indexSource == 0 || indexSource == 2) &&
        (prim == 4 || prim == 13 || (prim == 6 && !restartOn));
    const uint32_t directVertices = prim == 4 ? indexCount / 3 * 3 : prim == 13 ? indexCount / 4 * 6 : (indexCount >= 3 ? (indexCount - 2) * 3 : 0);
    IndexListEntry* indexEntry = directIndex ? nullptr : CachedIndexList(initiator, dmaBase, dmaSize);
    static const std::vector<uint32_t> noList;
    const std::vector<uint32_t>& list = indexEntry ? indexEntry->list : noList;
    if (directIndex ? directVertices == 0 : (!indexEntry || list.empty()))
    {
        TraceSkip("primitive", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    Section(2);
    {
        // Debug bisection: FM2_SKIP_DRAWS=a-b drops draws a..b-1 of the FM2_DUMP_FRAME frame.
        static uint32_t skipFrom = 0, skipTo = 0;
        static const bool parsed = [] { if (const char* s = getenv("FM2_SKIP_DRAWS")) sscanf(s, "%u-%u", &skipFrom, &skipTo); return true; }();
        static const uint64_t envDumpFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
        const uint64_t dumpFrame = g_hotkeyFrame.load(std::memory_order_relaxed) ? g_hotkeyFrame.load(std::memory_order_relaxed) : envDumpFrame; // (F12)
        (void)parsed;
        // FM2_SKIP_VS=h1,h2,...: drop those vertex shaders in the frame after the dump
        // (written to frame2.png), for an A/B pair under the same camera.
        static const std::vector<uint64_t> skipVs = [] {
            std::vector<uint64_t> v;
            for (const char* s = getenv("FM2_SKIP_VS"); s && *s;)
            {
                char* end;
                v.push_back(strtoull(s, &end, 16));
                s = *end == ',' ? end + 1 : end;
                if (end == s && *end != ',') break;
            }
            return v;
        }();
        const bool skipThis = g_r.frame == dumpFrame && std::find(skipVs.begin(), skipVs.end(), C().vsHash) != skipVs.end();
        // FM2_SWEEP="start,step,count" (with FM2_DUMP_FRAME or F12 as the first frame): frame
        // dumpFrame+i drops draws [start+i*step, start+(i+1)*step) and is written to
        // out/sweep/<i>.png, so one run compares many skips on near-identical frames.
        static uint32_t sweepStart = 0, sweepStep = 0, sweepCount = 0;
        static const bool sweepParsed = [] { if (const char* e = getenv("FM2_SWEEP")) sscanf(e, "%u,%u,%u", &sweepStart, &sweepStep, &sweepCount); return true; }();
        (void)sweepParsed;
        bool sweepSkip = false;
        if (sweepCount && dumpFrame && g_r.frame + 1 >= dumpFrame && g_r.frame + 1 < dumpFrame + sweepCount)
        {
            const uint32_t i = uint32_t(g_r.frame + 1 - dumpFrame);
            sweepSkip = g_r.drawsThisFrame >= sweepStart + i * sweepStep && g_r.drawsThisFrame < sweepStart + (i + 1) * sweepStep;
            // FM2_SWEEP_ONLY=1: the opposite: drop every draw in the whole swept range except this frame's chunk.
            if (getenv("FM2_SWEEP_ONLY"))
                sweepSkip = g_r.drawsThisFrame >= sweepStart && g_r.drawsThisFrame < sweepStart + sweepCount * sweepStep && !sweepSkip;
        }
        if (skipThis || sweepSkip || (skipTo && g_r.frame + 1 == dumpFrame && g_r.drawsThisFrame >= skipFrom && g_r.drawsThisFrame < skipTo))
        {
            g_r.drawsThisFrame++;
            return;
        }
    }
    BeginFrameIfNeeded();
    FlushPendingUpscale();
    SelectSurfaces(false);
    // Temporal upscaling: the main view's draws in the scene also write motion vectors (their
    // shaders' motion variants, once compiled; until then the draw has no motion).
    const bool cullable = (initiator & 0x3F) != 8 && (initiator & 0x3F) != 1;
    static LiveSwitch noMotion("FM2_NO_MOTION"); // A/B: no motion vectors (the upscaler sees a still scene)
    const bool wantMotion = g_temporal && g_scenePhase && edramMode == 4 && WidePass() && !noMotion;
    const CompiledShader* mvs = wantMotion ? MotionShader(vs) : nullptr;
    const CompiledShader* mps = wantMotion ? MotionShader(ps) : nullptr;
    bool motion = mvs && mps && mvs->IsReady() && mps->IsReady();
    D3D12_GPU_VIRTUAL_ADDRESS motionPrevious = 0; // last frame's vertex constants of this draw (b2)
    ID3D12PipelineState* pso = motion ? GetPipeline(*mvs, *mps, false, cullable, true) : nullptr;
    if (!pso)
    {
        motion = false;
        pso = GetPipeline(vs, ps, edramMode == 5, cullable);
    }
    if (!pso)
    {
        // A shader that failed to translate or compile: the draw is lost (logged once per pair).
        if (vs.state.load() == CompiledShader::Failed || ps.state.load() == CompiledShader::Failed)
        {
            static std::mutex failedMutex;
            static std::set<std::pair<uint64_t, uint64_t>> failedSeen;
            std::lock_guard failedLock(failedMutex);
            if (failedSeen.insert({ vs.hash, ps.hash }).second)
                Log("renderer: draw dropped, vs %016llx%s ps %016llx%s", (unsigned long long)vs.hash, vs.state.load() == CompiledShader::Failed ? " (failed)" : "",
                    (unsigned long long)ps.hash, ps.state.load() == CompiledShader::Failed ? " (failed)" : "");
        }
        TraceSkip("pipeline", initiator);
        g_skippedPipeline++;
        return;
    }
    FrameResources& f = Frame();

    Section(3);
    Sub(-1);
    // Index list.
    UINT64 listOffset = 0;
    if (!directIndex)
    {
        if (indexEntry->frame != g_r.frame)
        {
            indexEntry->offset = RingAlloc(list.size() * 4, 16);
            if (indexEntry->offset != ~0ull)
            {
                memcpy(f.ringCpu + indexEntry->offset, list.data(), list.size() * 4);
                indexEntry->frame = g_r.frame;
            }
        }
        listOffset = indexEntry->offset;
    }
    if (listOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }

    // Vertex constants: c[256], bools, fetch table, info.
    struct VSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t l[32]; // loop constants
        uint32_t vf[96][4];
        uint32_t info[4];
        float point[4];
        float vscale[4], voffset[4];
        uint32_t vte[4];
        uint32_t vfl[32][4]; // vertex fetch layout of the bound shader
        float vsquash[4][4]; // the easter egg's squash of car meshes (hlsl.cpp); zero = off
        float vpoly[4];      // x: polygon offset (window-space depth)
        float vwide[4];      // x: Hor+ factor (see hlsl.cpp)
        float vjitter[4];     // motion-vector variants: this frame's jitter (NDC)
    };
    struct PSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t l[32]; // loop constants
        uint32_t info[4];
        float info2[4];
        uint32_t tinfo[32][4];
        float tbias[32][4];
        float forceLod[4]; // x: forced absolute LOD (FM2_FORCE_LOD), -1 = implicit
    };
    Sub(0);
    const UINT64 vsOffset = RingAlloc(sizeof(VSConstants)), psOffset = RingAlloc(sizeof(PSConstants));
    if (vsOffset == ~0ull || psOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }
    // The draw's constants are built in ordinary memory and copied to the ring in a few sequential
    // blocks just before the draw: the ring is write-combined upload memory, where scattered small
    // writes and any reads back are slow (0.86 -> 0.64 ms a frame on a Ryzen 7 5800X). Not in low
    // performance mode: on an FX-8350 the bigger block copies cost more than they saved (submit
    // 3 -> 13-19 ms). FM2_CONSTANTS_IN_RING=1 writes straight into the ring (A/B).
    LiveSwitch &inRingSwitch = g_sw_FM2_CONSTANTS_IN_RING;
    const bool inRing = inRingSwitch || GetSettings().lowPerformance;
    thread_local std::unique_ptr<VSConstants> vcStage(new VSConstants);
    thread_local std::unique_ptr<PSConstants> pcStage(new PSConstants);
    const bool staged = !inRing;
    auto* vc = staged ? vcStage.get() : reinterpret_cast<VSConstants*>(f.ringCpu + vsOffset);
    auto* pc = staged ? pcStage.get() : reinterpret_cast<PSConstants*>(f.ringCpu + psOffset);
    // The float constants go straight into the ring when nothing reads them back from the stage (from
    // ReGPU 2fd47c3): staged, they were copied a second time at the flush, up to 4 KB a draw. Car draws
    // (hooks/dealer.cpp reads and changes vc->c) and traced frames (logged from it) keep the stage.
    // FM2_FLOATS_VIA_STAGE=1: always through the stage (A/B).
    const bool floatsDirect = staged && !g_floatsViaStage && !vs.carBody && !vs.carPart && !RendererTracing();
    {
        // Byte-swapped block copy of both constant banks (1024 dwords each).
        const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        // Only the constants each shader reads (the rest of the block is never read).
        const auto* src = reinterpret_cast<const __m128i*>(gpureg::File() + gpureg::AluConstants * 4);
        auto* dv = reinterpret_cast<__m128i*>(floatsDirect ? reinterpret_cast<VSConstants*>(f.ringCpu + vsOffset)->c : vc->c);
        auto* dp = reinterpret_cast<__m128i*>(floatsDirect ? reinterpret_cast<PSConstants*>(f.ringCpu + psOffset)->c : pc->c);
        LiveSwitch &allConstants = g_sw_FM2_ALL_CONSTANTS; // A/B
        const uint32_t nv = allConstants ? 256 : std::min(vs.info.constantsUsed, 256u), np = allConstants ? 256 : std::min(ps.info.constantsUsed, 256u);
        // Only the constants each shader reads, by its mask (from ReGPU 8a8bfa3). Car draws copy them
        // all: hooks/dealer.cpp reads the car's constants itself. FM2_CONSTANTS_BY_COUNT copies all
        // up to the highest one again (A/B).
        LiveSwitch &byCount = g_sw_FM2_CONSTANTS_BY_COUNT;
        auto copy = [&](__m128i* dst, const __m128i* from, const std::array<uint64_t, 4>& mask, uint32_t n, bool all) {
            if (all || byCount)
            {
                for (uint32_t i = 0; i < n; i++)
                    _mm_storeu_si128(dst + i, _mm_shuffle_epi8(_mm_loadu_si128(from + i), swap));
                return;
            }
            for (uint32_t w = 0; w < 4; w++)
                for (uint64_t bits = mask[w]; bits; bits &= bits - 1)
                {
                    const uint32_t i = w * 64 + uint32_t(_tzcnt_u64(bits));
                    _mm_storeu_si128(dst + i, _mm_shuffle_epi8(_mm_loadu_si128(from + i), swap));
                }
        };
        const bool carDraw = vs.carBody || vs.carPart;
        copy(dv, src, vs.info.constantMask, carDraw ? 256 : nv, allConstants || carDraw);
        copy(dp, src + 256, ps.info.constantMask, np, allConstants);
        if (motion)
        {
            // This draw's constants for the next frame, and last frame's for its motion. The
            // draw is known by its shaders, vertex and index data and how often it came up.
            uint64_t key = C().vsHash * 0x9E3779B97F4A7C15ull ^ C().psHash ^ (uint64_t(dmaBase) << 20) ^ initiator;
            for (uint32_t k : vs.info.vertexFetchConstants)
            {
                key = (key ^ gpureg::Get(gpureg::FetchConstants + k * 2)) * 0xFF51AFD7ED558CCDull;
                break; // the first vertex buffer is enough to tell draws apart
            }
            key ^= uint64_t(g_upscale.occurrences[key]++) * 0xC2B2AE3D27D4EB4Full;
            // Last frame's constants are still in last frame's ring (it is reused only after
            // kFrames): the draw reads them there (b2).
            auto& entry = g_upscale.constants[key];
            const uint32_t parity = g_upscale.constantsFrame & 1;
            const D3D12_GPU_VIRTUAL_ADDRESS now = f.ring->GetGPUVirtualAddress() + vsOffset;
            const bool known = entry.frame + 1 == g_upscale.constantsFrame && !g_upscale.reset;
            motionPrevious = known ? entry.address[parity ^ 1] : now;
            entry.address[parity] = now;
            entry.frame = g_upscale.constantsFrame;
            vc->vjitter[0] = g_upscale.jitterNdc[0];
            vc->vjitter[1] = g_upscale.jitterNdc[1];
            vc->vjitter[2] = vc->vjitter[3] = 0;
        }
    }
    Sub(1);
    for (uint32_t i = 0; i < 8; i++)
        vc->b[i] = pc->b[i] = gpureg::Get(gpureg::BoolConstants + i);
    for (uint32_t i = 0; i < 32; i++)
        vc->l[i] = pc->l[i] = gpureg::Get(gpureg::LoopConstants + i);
    // Debug: FM2_FORCE_BOOL=<index>:<0|1> forces one boolean constant for every draw.
    if (static const char* force = getenv("FM2_FORCE_BOOL"); force)
    {
        static uint32_t index = 0, value = 0;
        static const bool parsed = sscanf(force, "%u:%u", &index, &value) == 2 && index < 256;
        if (parsed)
        {
            const uint32_t bit = 1u << (index & 31);
            vc->b[index >> 5] = pc->b[index >> 5] = value ? (pc->b[index >> 5] | bit) : (pc->b[index >> 5] & ~bit);
        }
    }
    Sub(4);
    // Only the entries this shader reads are written (all four fields): clearing all 96 (1.5 KB of
    // write-combined memory a draw) was most of the vertex fetch time on an FX-8350.
    LiveSwitch &clearFetch = g_sw_FM2_CLEAR_FETCH_TABLE; // A/B
    if (clearFetch)
        memset(vc->vf, 0, sizeof(vc->vf));
    for (uint32_t k : vs.info.vertexFetchConstants)
    {
        const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + k * 2), d1 = gpureg::Get(gpureg::FetchConstants + k * 2 + 1);
        const uint32_t address = d0 & 0xFFFFFFFC, bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
        // Large buffers (static geometry) are read in place; small ones may be
        // rewritten before the GPU runs this frame, so they are copied.
        LiveSwitch &noDirect = g_sw_FM2_NO_DIRECT_FETCH;
        static const uint32_t directBytes = getenv("FM2_DIRECT_FETCH_KB") ? uint32_t(atoi(getenv("FM2_DIRECT_FETCH_KB"))) * 1024u : kDirectFetchBytes;
        if (g_r.physical && !noDirect && bytes >= directBytes)
        {
            // FM2_TRACE_DIRECT=1: remember the in-place buffers of depth-only draws and check at the next
            // frame's end whether the game rewrote them (the GPU reads them later than the draw is recorded).
            LiveSwitch &traceDirect = g_sw_FM2_TRACE_DIRECT;
            if (traceDirect && edramMode == 5)
            {
                std::lock_guard lock(g_directTrace.mutex);
                auto& e = g_directTrace.seen[address];
                if (e.frame != g_r.frame)
                {
                    e.frame = g_r.frame;
                    e.bytes = bytes;
                    e.vs = C().vsHash;
                    e.hash = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(address)), bytes);
                }
            }
            vc->vf[k][0] = address & 0x1FFFFFFF;
            vc->vf[k][1] = d1 & 3;
            vc->vf[k][2] = 1;
            vc->vf[k][3] = 0;
            continue;
        }
        // FM2_TRACE_MIDSIZE=1: log copied buffers of 16-64 KB that change within a frame (they must stay copied)
        LiveSwitch &traceMid = g_sw_FM2_TRACE_MIDSIZE;
        static const uint32_t traceMinBytes = traceMid ? uint32_t(atoi(getenv("FM2_TRACE_MIDSIZE"))) * 1024u : 0;
        if (traceMid && bytes >= traceMinBytes)
        {
            static std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> seen; // address -> frame, hash
            static uint32_t changed = 0, total = 0;
            const uint64_t h = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(address)), bytes);
            auto& e = seen[address];
            total++;
            if (e.first == g_r.frame && e.second != h && changed++ < 40)
                Log("midsize: %08X %u bytes changed within frame %llu (vs %016llx)", address, bytes, (unsigned long long)g_r.frame, (unsigned long long)C().vsHash);
            static uint32_t across = 0;
            if (e.first != g_r.frame && e.first + 4 > g_r.frame && e.second != h && across++ < 60)
                Log("midsize: %08X %u bytes changed since frame %llu (vs %016llx ps %016llx)", address, bytes, (unsigned long long)e.first, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
            e = { g_r.frame, h };
        }
        Sub(2);
        const UINT64 dataOffset = UploadGuest(GpuPhysToGuest(address), bytes);
        Sub(5);
        if (dataOffset == ~0ull)
        {
            TraceSkip("ring full", initiator);
            return;
        }
        vc->vf[k][0] = uint32_t(dataOffset);
        vc->vf[k][1] = d1 & 3;
        vc->vf[k][2] = vc->vf[k][3] = 0;
    }
    Sub(2);
    // Perspective geometry in a wide pass (see ViewExtent): the game's camera already projects
    // at the wider aspect once hooks/camera_aspect.cpp widened it, so only the vertical zoom
    // for FM2_WIDE_FOV < 100 remains; otherwise the view is narrowed here (Hor+ without the
    // game's culling knowing).
    {
        const float wide = WidePass() ? OutputWide() : 1.0f;
        const float extent = WidePass() ? ViewExtent() : 1.0f;
        const bool gameWidened = g_cameraAspectWidened.load(std::memory_order_relaxed);
        vc->vwide[0] = gameWidened ? 1.0f : 1.0f / extent;
        vc->vwide[1] = wide / extent;
        LiveSwitch &noSqueeze = g_sw_FM2_NO_HUD_SQUEEZE; // A/B: 2D stretched with the view
        vc->vwide[2] = noSqueeze ? 1.0f : 1.0f / wide;
        vc->vwide[3] = 1; // set after the textures are bound (below)
    }
    if (!vs.info.vertexFetchLayout.empty())
        memcpy(vc->vfl, vs.info.vertexFetchLayout.data(), vs.info.vertexFetchLayout.size() * sizeof(vc->vfl[0]));
    vc->info[0] = uint32_t(listOffset);
    vc->info[1] = (initiator & 0x3F) == 1 ? 2 : 0;
    // Viewport transform in the vertex shader (see hlsl.cpp).
    vc->vscale[0] = RegFloat(PA_CL_VPORT_XSCALE);
    vc->vscale[1] = RegFloat(PA_CL_VPORT_YSCALE);
    vc->vscale[2] = RegFloat(PA_CL_VPORT_ZSCALE);
    vc->vscale[3] = 0;
    vc->voffset[3] = 0;
    // The title clears its (non-MSAA) shadow depth maps through views of the same EDRAM
    // with MSAA on: a pixel of a 4x surface covers 2x2 samples of the linear layout we
    // render in, a pixel of a 2x surface 1x2. Scale those clear rectangles to match.
    LiveSwitch &noMsaaScale = g_sw_FM2_NO_MSAA_SCALE;
    Sub(3);
    if (!noMsaaScale && (initiator & 0x3F) == 8 && (gpureg::Get(RB_COLOR_MASK) & 0xF) == 0 && (gpureg::Get(RB_SURFACE_INFO) & 0x3FFF) < 1000)
    {
        const uint32_t msaa = (gpureg::Get(RB_SURFACE_INFO) >> 16) & 3;
        if (msaa == 2)
            vc->vscale[3] = vc->voffset[3] = 2.0f;
        else if (msaa == 1)
        {
            vc->vscale[3] = 1.0f;
            vc->voffset[3] = 2.0f;
        }
    }
    // The same trick between two MSAA views (the paint editor clears its 2x livery composite
    // through a 4x view, 1000 pixels of it plus a 48-pixel 2x strip): texture-less rectangles
    // are scaled from their view's layout to the surface's (see g_nativeMsaa).
    else if (!noMsaaScale)
    {
        const uint32_t surface = gpureg::Get(RB_SURFACE_INFO), msaa = (surface >> 16) & 3;
        if ((initiator & 0x3F) != 8 || ps.info.usedTextureSlots || vs.info.usedTextureSlots)
        {
            SetNativeMsaa(LayoutKey(surface), msaa);
        }
        else if (TallView())
        {
            const uint32_t native = (g_tallInfo >> 16) & 3;
            vc->vscale[3] = float(MsaaAcross(msaa)) / float(MsaaAcross(native));
            vc->voffset[3] = float(MsaaDown(msaa)) / float(MsaaDown(native));
        }
        else if (const int native = ForeignNativeMsaa(); native >= 0)
        {
            vc->vscale[3] = float(MsaaAcross(msaa)) / float(MsaaAcross(uint32_t(native)));
            vc->voffset[3] = float(MsaaDown(msaa)) / float(MsaaDown(uint32_t(native)));
        }
    }
    Sub(6);
    vc->voffset[0] = RegFloat(PA_CL_VPORT_XOFFSET);
    vc->voffset[1] = RegFloat(PA_CL_VPORT_YOFFSET);
    vc->voffset[2] = RegFloat(PA_CL_VPORT_ZOFFSET);
    vc->vte[0] = gpureg::Get(PA_CL_VTE_CNTL);
    // Polygon offset (PA_SU_SC_MODE_CNTL bits 11 front, 12 back, 13 other primitives): a constant depth
    // offset, as Xenia applies it (halved for 24-bit float depth); the slope term is not applied. The
    // showroom's floor shadow is a decal drawn on the floor with an offset and a >= depth test: without
    // it the decal z-fought the floor below, black wedges popping in and out as the car turned.
    // FM2_NO_POLY_OFFSET=1 leaves it out (A/B).
    {
        LiveSwitch &noPolyOffset = g_sw_FM2_NO_POLY_OFFSET;
        const uint32_t sc = gpureg::Get(PA_SU_SC_MODE_CNTL);
        const bool tri = prim == 4 || prim == 5 || prim == 6;
        const bool enabled = tri ? (sc & (3u << 11)) != 0 : ((sc >> 13) & 1) != 0;
        float offset = 0;
        if (enabled && !noPolyOffset)
        {
            offset = (sc >> 11) & 1 ? RegFloat(0x2381) : RegFloat(0x2383);
            if ((gpureg::Get(0x2002) >> 16) & 1)
                offset *= 0.5f;
        }
        vc->vpoly[0] = offset;
        vc->vpoly[1] = vc->vpoly[2] = vc->vpoly[3] = 0;
    }
    // D3D9 pixel centres (PA_SU_VTX_CNTL bit 0 clear): every draw lands half a pixel off D3D12's, as
    // Xenia's half_pixel_offset applies (bit 16 tells the vertex shader). FM2_NO_HALF_PIXEL=1: off (A/B).
    LiveSwitch &noHalfPixel = g_sw_FM2_NO_HALF_PIXEL;
    if (!noHalfPixel && (gpureg::Get(0x2302) & 1) == 0)
        vc->vte[0] |= 1u << 16;
    vc->vte[1] = vc->vte[2] = vc->vte[3] = 0;
    // Triangle lists and restart-free strips from an index buffer: a real indexed draw.
    LiveSwitch &noIndexed = g_sw_FM2_NO_INDEXED;
    D3D12_INDEX_BUFFER_VIEW indexView{};
    uint32_t indexedCount = 0;
    if (directIndex && !noIndexed && indexSource == 0 && (prim == 4 || prim == 6))
    {
        const bool is32 = (initiator >> 11) & 1;
        const uint32_t available = dmaSize & 0xFFFFFF;
        const uint32_t count = std::min(indexCount, available);
        indexedCount = prim == 4 ? count / 3 * 3 : count;
        Sub(3);
        if (indexedCount < 3 || !GetIndexBuffer(dmaBase & 0x1FFFFFFF & ~(is32 ? 3u : 1u), count, is32, dmaSize >> 30, indexView))
            indexedCount = 0;
        Sub(7);
    }
    if (indexedCount)
    {
        vc->vte[1] = 1u | 8u; // the vertex ID is the index
        vc->vte[3] = gpureg::Get(VGT_INDX_OFFSET);
    }
    else if (directIndex)
    {
        const bool is32 = (initiator >> 11) & 1;
        vc->vte[1] = 1u | (prim == 6 ? 2u : 0u) | (prim == 13 ? 64u : 0u) | (is32 ? 4u : 0u) | (indexSource == 2 ? 8u : 0u) | ((dmaSize >> 30) << 4);
        vc->vte[2] = dmaBase & 0x1FFFFFFF & ~(is32 ? 3u : 1u);
        vc->vte[3] = gpureg::Get(VGT_INDX_OFFSET);
    }
    // The window scissor marks a tall pass (PassHeight); this draw may be its first, so check before the
    // target size is taken (else the vertices are placed for 720 rows in a taller viewport: the career
    // menu's one-off livery downsample came out stretched 1.42x).
    (void)WindowScissor();
    vc->info[2] = PassWidth();
    vc->info[3] = PassHeight();
    // Point sizes are 12.4 fixed-point radii.
    const uint32_t pointSize = gpureg::Get(PA_SU_POINT_SIZE), pointMinMax = gpureg::Get(PA_SU_POINT_MINMAX);
    vc->point[0] = float(pointSize >> 16) / 8.0f;
    vc->point[1] = float(pointSize & 0xFFFF) / 8.0f;
    vc->point[2] = float(pointMinMax & 0xFFFF) / 8.0f;
    vc->point[3] = float(pointMinMax >> 16) / 8.0f;
    // Car meshes read vsquash as their squash (the "whysthisdealer" easter egg, dealer.cpp).
    memset(vc->vsquash, 0, sizeof(vc->vsquash));
    if (vs.carBody || vs.carPart)
    {
        DealerCarDraw(C().vsHash, vc->c, vc->vsquash, vs.carBody);
        LiveSwitch &traceCar = g_sw_FM2_TRACE_CAR_DRAWS;
        if (traceCar)
            Log("car draw: vs %016llx ps %016llx origin %.3f %.3f %.3f %.3f x %.3f %.3f %.3f", (unsigned long long)C().vsHash, (unsigned long long)C().psHash,
                vc->c[3][0], vc->c[3][1], vc->c[3][2], vc->c[3][3], vc->c[0][0], vc->c[0][1], vc->c[0][2]);
    }
    pc->info[1] = (gpureg::Get(SQ_PROGRAM_CNTL) >> 18) & 1 ? (gpureg::Get(SQ_CONTEXT_MISC) >> 8) & 0xFF : 0xFF;

    const uint32_t colorControl = gpureg::Get(RB_COLORCONTROL);
    // Depth-only passes (EDRAM mode 5) keep the alpha test only when the alpha comes from a texture:
    // the showroom's car shadow map has cut-out geometry that otherwise lands in it as solid blocks.
    // A pass-through alpha (an interpolator, often unwritten) is tested here but not in effect on
    // the console: the race's sun shadow map lost every caster (no car shadows, #4), and the clear
    // rectangles before it, so its depth piled up over frames (a smeared blob that moved).
    // FM2_NO_DEPTH_ALPHATEST=1 skips it in every depth-only pass.
    static const bool depthAlphaTest = getenv("FM2_NO_DEPTH_ALPHATEST") == nullptr;
    const bool alphaTestHere = edramMode != 5 || (depthAlphaTest && ps.info.usedTextureSlots != 0);
    pc->info[0] = (colorControl >> 3) & 1 && alphaTestHere ? (colorControl & 7) : 7;
    pc->info2[0] = RegFloat(RB_ALPHA_REF);
    pc->info2[1] = 1.0f / ScaleX();
    pc->info2[2] = 1.0f / ScaleY();
    // Our colour target is half float: a 7e3 surface (formats 3 and 12, the title's HDR scene)
    // saturates at 31.875, so its output is clamped to that, as the hardware does (the bloom
    // reads those values).
    const uint32_t colorFormat = (gpureg::Get(RB_COLOR_INFO) >> 16) & 0xF;
    pc->info2[3] = colorFormat == 3 || colorFormat == 12 ? 31.875f : 0.0f;
    // FM2_FORCE_LOD=<float>: force the pixel shader to sample an absolute mip level, for the
    // RADV distance-noise diagnostic. -1 (default) keeps the implicit LOD.
    static const float forceLod = getenv("FM2_FORCE_LOD") ? float(atof(getenv("FM2_FORCE_LOD"))) : -1.0f;
    pc->forceLod[0] = forceLod;
    pc->forceLod[1] = pc->forceLod[2] = pc->forceLod[3] = 0.0f;
    Sub(3);

    Section(4);
    // Textures are bindless: each one has a permanent descriptor in the heap's
    // bindless region, and the draw passes its index in the constants.
    memset(pc->tinfo, 0, sizeof(pc->tinfo));
    memset(pc->tbias, 0, sizeof(pc->tbias));
    const uint32_t usedSlots = ps.info.usedTextureSlots | vs.info.usedTextureSlots;
    if (g_bindingsFrame != g_r.frame || g_bindingsEpoch != g_resolveEpoch)
    {
        if (!g_bindings)
            g_bindings = std::make_unique<SlotBinding[]>(kBindingSlots); // (entries carry their frame and epoch)
        g_bindingsFrame = g_r.frame;
        g_bindingsEpoch = g_resolveEpoch;
    }
    for (uint32_t m = usedSlots; m; m &= m - 1)
    {
        const uint32_t slot = uint32_t(__builtin_ctz(m));
        const TextureFetch fetch = ReadTextureFetch(slot);
        uint64_t bindingKey = 0xCBF29CE484222325ull;
        for (uint32_t d : fetch.dwords)
            bindingKey = (bindingKey ^ d) * 0x100000001B3ull;
        SlotBinding& memo = g_bindings[(bindingKey ^ bindingKey >> 29) & (kBindingSlots - 1)];
        static const bool noSlotMemo = getenv("FM2_NO_SLOT_MEMO") != nullptr || getenv("FM2_DUMP_PS_TEXTURES") != nullptr;
        if (!noSlotMemo && memo.frame == g_r.frame && memo.epoch == g_resolveEpoch && memcmp(memo.fetch, fetch.dwords, sizeof(memo.fetch)) == 0)
        {
            memcpy(pc->tinfo[slot], memo.tinfo, sizeof(memo.tinfo));
            memcpy(pc->tbias[slot], memo.tbias, sizeof(memo.tbias));
            g_drawSamplesResolve |= memo.samplesResolve;
            continue;
        }
        const bool samplesBefore = g_drawSamplesResolve;
        g_drawSamplesResolve = false;
        const bool cube = fetch.Dimension() == 3;
        static const uint64_t dumpPs = getenv("FM2_DUMP_PS_TEXTURES") ? strtoull(getenv("FM2_DUMP_PS_TEXTURES"), nullptr, 16) : 0;
        if (dumpPs && C().psHash == dumpPs && !cube)
        {
            static std::set<uint32_t> dumped;
            std::vector<uint8_t> rgba;
            if (dumped.insert(fetch.Address()).second && DecodeTexture(fetch, rgba))
            {
                char name[80];
                std::filesystem::create_directories("out/tex");
                snprintf(name, sizeof(name), "out/tex/t%u_%08X_%s_%ux%u_sw%03X.png", slot, fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(),
                    fetch.Height(), fetch.Swizzle());
                WritePng(name, fetch.Width(), fetch.Height(), rgba);
            }
        }
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool native = false;
        g_textureFailure[0] = 0;
        ID3D12Resource* texture = cube ? GetCubeTexture(fetch, format) : GetTexture(fetch, format, native);
        // Debug: FM2_NO_LIVE_CUBE=1 binds the fallback for cubes the title rendered (live reflections).
        if (LiveSwitch &noLiveCube = g_sw_FM2_NO_LIVE_CUBE; noLiveCube && cube && ResolvedFor(fetch))
            texture = nullptr;
        const uint32_t index = texture ? BindlessIndex(texture, format, cube) : (cube ? 1u : 0u);
        if (RendererTracing())
            CaptureTexture(fetch, slot, cube, texture != nullptr, index, native, g_textureFailure);
        // w: dimension (1 2D, 3 cube); bit 31 + bits 8-19: the fetch swizzle, applied in the
        // shader for textures that live on the GPU (resolves). Decoded ones are swizzled already.
        LiveSwitch &noGpuSwizzle = g_sw_FM2_NO_GPU_SWIZZLE;
        const ResolvedTexture* resolvedPtr = ResolvedFor(fetch);
        const bool onGpu = (!noGpuSwizzle && fetch.Type() == 2 && resolvedPtr != nullptr) || native;
        // Our resolved textures keep red and blue in place; when the title
        // resolved with the swap bit, its fetch swizzle expects them exchanged.
        uint32_t swizzle = fetch.Swizzle();
        if (onGpu && resolvedPtr && resolvedPtr->swapped)
            for (uint32_t c = 0; c < 4; c++)
            {
                const uint32_t sel = (swizzle >> (3 * c)) & 7;
                if (sel == 0 || sel == 2)
                    swizzle ^= (sel ^ (2 - sel)) << (3 * c);
            }
        // Built here and written once: the constants live in upload memory, which is slow to read
        // back (from ReGPU e9e5e2c).
        uint32_t ti[4]{};
        float tb[4]{};
        ti[3] = (cube ? 3 : 1) | (onGpu ? (1u << 31) | (swizzle << 8) : 0);
        // Clamp X and Y (dword 0 bits 10-12, 13-15): 0 wrap, else clamp. Filter: dword 3 bits
        // 19-20 (mag): 0 point. With only one axis clamped (kerbs: clamped across, repeating
        // along) the wrapping sampler is used and the shader clamps that axis (bits 18, 19).
        const uint32_t clampX = (fetch.dwords[0] >> 10) & 7, clampY = (fetch.dwords[0] >> 13) & 7;
        LiveSwitch &oldClamp = g_sw_FM2_OLD_CLAMP; // A/B: X's mode for both axes (before the kerb fix)
        const bool clamp = oldClamp ? clampX != 0 : clampX != 0 && clampY != 0;
        const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
        ti[0] = (point ? (clamp ? 2 : 3) : (clamp ? 1 : 0)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
        if (!clamp && !oldClamp)
            ti[0] |= (clampX ? 1u << 18 : 0u) | (clampY ? 1u << 19 : 0u);
        // Clamp to border on both axes (modes 6, 7): the shader returns the border colour
        // outside the image (dword 5 bits 0-1: 1 white, else black).
        if (clampX >= 6 && clampY >= 6)
            ti[0] |= (1u << 16) | ((fetch.dwords[5] & 3) == 1 ? 1u << 17 : 0u);
        tb[0] = fetch.LodBias();
        memcpy(&tb[1], &index, 4);
        // A resolved texture is as wide as the pitch it was resolved with; the fetch
        // constant may declare a narrower image inside it (e.g. 163 of 192 columns):
        // the shader scales UVs so 0..1 spans the declared image, not the padding.
        tb[2] = tb[3] = 1.0f;
        if (onGpu && resolvedPtr && !cube && resolvedPtr->width && resolvedPtr->height && resolvedPtr->resource)
        {
            tb[2] = std::min(1.0f, float(fetch.Width()) / float(resolvedPtr->width));
            tb[3] = std::min(1.0f, float(fetch.Height()) / float(resolvedPtr->height));
        }
        ti[1] = fetch.Width();
        ti[2] = fetch.Height();
        memcpy(memo.fetch, fetch.dwords, sizeof(memo.fetch));
        memo.frame = g_r.frame;
        memo.epoch = g_resolveEpoch;
        memcpy(memo.tinfo, ti, sizeof(memo.tinfo));
        memcpy(memo.tbias, tb, sizeof(memo.tbias));
        memcpy(pc->tinfo[slot], ti, sizeof(ti));
        memcpy(pc->tbias[slot], tb, sizeof(tb));
        memo.samplesResolve = g_drawSamplesResolve;
        g_drawSamplesResolve |= samplesBefore;
    }
    // HUD and menus sample ordinary textures; full-screen layers sample resolved targets
    // and must keep spanning the width (squeezing them warped the image outside 16:9).
    vc->vwide[3] = g_drawSamplesResolve ? 0.0f : 1.0f;

    Section(5);
    // Viewport. The viewport transform happens in the vertex shader; D3D maps 1:1.
    // Tiled passes shift each draw into EDRAM by the window offset. We render in screen
    // space, so tiled draws stay unshifted. The title's rectangle draws (depth resets)
    // reset the window offset to zero and use tile-relative coordinates instead:
    // those land in the current tile, so shift them by the tile's origin.
    D3D12_RECT scissor = WindowScissor();
    float shiftY = 0;
    {
        const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
        const int32_t wy = ((gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1) ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
        const bool full = scissor.left <= 0 && scissor.top <= 0 && scissor.right >= LONG(PassWidth()) && scissor.bottom >= LONG(PassHeight());
        if (!full)
        {
            g_tile.originY = -wy;
            g_tile.rect = scissor;
            g_tile.surface = gpureg::Get(RB_SURFACE_INFO);
        }
        // Only draws on the tiled pass's own surface are tile-relative: Photo Mode draws its
        // depth-of-field composite once, full-screen, into another view of the same EDRAM after
        // the 4x scene's three tiles, and clipped to the last tile it covered only the bottom
        // third (the rest of the picture was black).
        // Another MSAA view of the tiled surface itself (the paint editor's stencil clears) is still
        // tile-relative.
        else if (g_tile.originY != 0 && (g_tile.surface == ~0u || g_tile.surface == gpureg::Get(RB_SURFACE_INFO) || (!g_noForeignTile && ForeignNativeMsaa() >= 0)))
        {
            shiftY = float(g_tile.originY + wy);
            scissor.left = std::max(scissor.left, g_tile.rect.left);
            scissor.top = std::max(scissor.top, g_tile.rect.top);
            scissor.right = std::min(scissor.right, g_tile.rect.right);
            scissor.bottom = std::min(scissor.bottom, g_tile.rect.bottom);
        }
    }
    const D3D12_VIEWPORT vp{ 0, shiftY * ScaleY(), float(PassWidth()) * ScaleX(), float(PassHeight()) * ScaleY(), 0, 1 };
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
    {
        TraceSkip("empty scissor", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    D3D12_RECT hostScissor = ToHost(scissor);
    // A scissor narrower than the screen in a wide pass clips 2D content (the track map),
    // which the vertex shader keeps in the centred 16:9 area: map it the same way. Not for
    // screen layers (post-processing done in halves would leave a seam).
    if (WidePass() && ScaleWide() != ScaleY() && !g_drawSamplesResolve && (scissor.left > 0 || scissor.right < LONG(kWidth)))
    {
        const double centre = kWidth * ScaleWide() / 2.0, s = ScaleY();
        hostScissor.left = LONG(std::lround(centre + (scissor.left - double(kWidth) / 2) * s));
        hostScissor.right = LONG(std::lround(centre + (scissor.right - double(kWidth) / 2) * s));
    }
    DrawState& ds = g_drawState;
    if (memcmp(&ds.viewport, &vp, sizeof(vp)) != 0)
    {
        ds.viewport = vp;
        C().list->RSSetViewports(1, &vp);
    }
    if (memcmp(&ds.scissor, &hostScissor, sizeof(hostScissor)) != 0)
    {
        ds.scissor = hostScissor;
        C().list->RSSetScissorRects(1, &hostScissor);
    }
    if (const uint32_t ref = gpureg::Get(RB_STENCILREFMASK) & 0xFF; ref != ds.stencilRef)
    {
        ds.stencilRef = ref;
        C().list->OMSetStencilRef(ref);
    }
    {
        // RB_BLEND_RED..ALPHA: the constant colour for blend factors 12-15.
        const float factor[4] = { RegFloat(0x2105), RegFloat(0x2106), RegFloat(0x2107), RegFloat(0x2108) };
        if (memcmp(ds.blendFactor, factor, sizeof(factor)) != 0)
        {
            memcpy(ds.blendFactor, factor, sizeof(factor));
            C().list->OMSetBlendFactor(factor);
        }
    }
    if (motion != ds.motionRoot)
    {
        // Motion pipelines have their own root signature; switching clears the root arguments.
        ds.motionRoot = motion;
        C().list->SetGraphicsRootSignature(motion ? g_r.motionRootSignature.Get() : g_r.rootSignature.Get());
        ds.table2 = ds.table3 = D3D12_GPU_DESCRIPTOR_HANDLE{};
    }
    if (motion != ds.motionTargets)
    {
        ds.motionTargets = motion;
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = { C().rtv, g_upscale.motionRtv };
        C().list->OMSetRenderTargets(motion ? 2 : 1, targets, FALSE, &C().dsv);
    }
    if (pso != ds.pso)
    {
        ds.pso = pso;
        const auto t0 = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        C().list->SetPipelineState(pso);
        if (g_perfTiming)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 1.0)
                Log("perf: SetPipelineState took %.1f ms (vs %016llx ps %016llx)", ms, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
        }
    }
    if (staged)
    {
        // Float constants up to the highest one the shader reads (all for car draws, which the
        // dealer hook reads), then everything after them, each in one go.
        LiveSwitch &allConstantsFlush = g_sw_FM2_ALL_CONSTANTS;
        const bool carDraw = vs.carBody || vs.carPart;
        const size_t nv = allConstantsFlush || carDraw ? 256 : std::min(vs.info.constantsUsed, 256u);
        const size_t np = allConstantsFlush ? 256 : std::min(ps.info.constantsUsed, 256u);
        uint8_t* rv = f.ringCpu + vsOffset;
        uint8_t* rp = f.ringCpu + psOffset;
        if (!floatsDirect)
        {
            memcpy(rv, vc->c, nv * 16);
            memcpy(rp, pc->c, np * 16);
        }
        // Then only the vertex fetch and texture entries this draw's shaders read: the tables are sized for
        // 96 fetch constants and 32 slots, a draw uses a few, and the rest was ~3 KB of padding copied per
        // draw (the largest single cost on the command processor in a race; from ReGPU 4884e92). The ring
        // bytes skipped are never read. FM2_FULL_CONSTANT_BLOCKS=1 copies the whole blocks (A/B).
        LiveSwitch &fullBlocks = g_sw_FM2_FULL_CONSTANT_BLOCKS;
        auto put = [](uint8_t* ring, const void* stage, size_t from, size_t to) {
            if (to > from)
                memcpy(ring + from, static_cast<const uint8_t*>(stage) + from, to - from);
        };
        if (fullBlocks)
        {
            put(rv, vc, sizeof(vc->c), sizeof(VSConstants));
            put(rp, pc, sizeof(pc->c), sizeof(PSConstants));
        }
        else
        {
            uint32_t fetches = 0;
            for (uint32_t k : vs.info.vertexFetchConstants)
                fetches = std::max(fetches, k + 1);
            const size_t layouts = std::min<size_t>(vs.info.vertexFetchLayout.size(), 32);
            const uint32_t slotMask = ps.info.usedTextureSlots | vs.info.usedTextureSlots;
            const uint32_t slots = slotMask ? 32 - uint32_t(__builtin_clz(slotMask)) : 0;
            put(rv, vc, offsetof(VSConstants, b), offsetof(VSConstants, vf));
            put(rv, vc, offsetof(VSConstants, vf), offsetof(VSConstants, vf) + fetches * sizeof(vc->vf[0]));
            put(rv, vc, offsetof(VSConstants, info), offsetof(VSConstants, vfl));
            put(rv, vc, offsetof(VSConstants, vfl), offsetof(VSConstants, vfl) + layouts * sizeof(vc->vfl[0]));
            put(rv, vc, offsetof(VSConstants, vsquash), sizeof(VSConstants));
            put(rp, pc, offsetof(PSConstants, b), offsetof(PSConstants, tinfo));
            put(rp, pc, offsetof(PSConstants, tinfo), offsetof(PSConstants, tinfo) + slots * sizeof(pc->tinfo[0]));
            put(rp, pc, offsetof(PSConstants, tbias), offsetof(PSConstants, tbias) + slots * sizeof(pc->tbias[0]));
            put(rp, pc, offsetof(PSConstants, forceLod), sizeof(PSConstants));
        }
    }
    C().list->SetGraphicsRootConstantBufferView(0, f.ring->GetGPUVirtualAddress() + vsOffset);
    C().list->SetGraphicsRootConstantBufferView(1, f.ring->GetGPUVirtualAddress() + psOffset);
    if (motion)
        C().list->SetGraphicsRootConstantBufferView(4, motionPrevious);
    if (const D3D12_GPU_DESCRIPTOR_HANDLE t2 = Gpu(g_r.srvHeap.Get(), f.descriptorBase, g_r.srvStride); t2.ptr != ds.table2.ptr)
    {
        ds.table2 = t2;
        C().list->SetGraphicsRootDescriptorTable(2, t2);
    }
    if (const D3D12_GPU_DESCRIPTOR_HANDLE t3 = Gpu(g_r.srvHeap.Get(), kBindlessBase, g_r.srvStride); t3.ptr != ds.table3.ptr)
    {
        ds.table3 = t3;
        C().list->SetGraphicsRootDescriptorTable(3, t3);
    }
    // FM2_CAPTURE_COMPOSITE=<n>: trace (as F12) the n-th frame that draws into the 2048-wide
    // surface (pitch field 0x820: the paint editor's livery composite), from its first such draw on.
    if (static const int compositeN = getenv("FM2_CAPTURE_COMPOSITE") ? atoi(getenv("FM2_CAPTURE_COMPOSITE")) : 0;
        compositeN && (gpureg::Get(RB_SURFACE_INFO) & 0x3FFF) == 0x820)
    {
        static uint64_t lastFrame = ~0ull;
        static int seen = 0;
        if (g_r.frame != lastFrame && ++seen == compositeN)
        {
            g_hotkeyFrame = g_r.frame + 1;
            Log("capture: composite pass %d, tracing frame %llu", seen, (unsigned long long)(g_r.frame + 1));
        }
        lastFrame = g_r.frame;
    }
    const uint64_t traceFrame = TraceFrame();
    if (traceFrame && g_r.frame + 1 == traceFrame)
    {
        std::string tex;
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            if (((ps.info.usedTextureSlots >> slot) & 1) == 0)
                continue;
            const TextureFetch t = ReadTextureFetch(slot);
            char e[160];
            snprintf(e, sizeof(e), " t%u=%s %ux%u@%08X sw%03X sg%02X d%u [%08X %08X %08X %08X %08X %08X]", slot, TextureFormatName(t.Format()), t.Width(), t.Height(), t.Address(), t.Swizzle(),
                (t.dwords[0] >> 2) & 0xFF, t.Dimension(), t.dwords[0], t.dwords[1], t.dwords[2], t.dwords[3], t.dwords[4], t.dwords[5]);
            tex += e;
        }
        Log("    indices (initiator %08X dma %08X offset %u): %u %u %u %u %u %u ... %u", initiator, dmaBase, gpureg::Get(VGT_INDX_OFFSET),
            list.size() > 0 ? list[0] : 0, list.size() > 1 ? list[1] : 0, list.size() > 2 ? list[2] : 0, list.size() > 3 ? list[3] : 0,
            list.size() > 4 ? list[4] : 0, list.size() > 5 ? list[5] : 0, list.empty() ? 0 : list.back());
        Log("draw #%u vs %016llx ps %016llx prim %u n %zu blend %08X mask %X scissor %ld,%ld-%ld,%ld vte %03X wide %d layer %d pitch %u%s", g_r.drawsThisFrame.load(),
            (unsigned long long)C().vsHash, (unsigned long long)C().psHash, initiator & 0x3F, list.size(), gpureg::Get(RB_BLENDCONTROL0),
            gpureg::Get(RB_COLOR_MASK) & 15, scissor.left, scissor.top, scissor.right, scissor.bottom, gpureg::Get(PA_CL_VTE_CNTL) & 0x3FF,
            int(WidePass()), int(g_drawSamplesResolve), gpureg::Get(RB_SURFACE_INFO) & 0x3FFF, tex.c_str());
        uint32_t bools[8];
        for (int i = 0; i < 8; i++)
            bools[i] = gpureg::Get(gpureg::BoolConstants + i);
        Log("    bools %08X %08X %08X %08X %08X %08X %08X %08X", bools[0], bools[1], bools[2], bools[3], bools[4], bools[5], bools[6], bools[7]);
        if (getenv("FM2_TRACE_VC"))
            for (uint32_t i = 0; i < 256; i++)
            {
                const float* c = vc->c[i];
                if (c[0] != 0 || c[1] != 0 || c[2] != 0 || c[3] != 0)
                {
                    uint32_t h[4];
                    memcpy(h, c, 16);
                    Log("    vc%u = %g %g %g %g [%08X %08X %08X %08X]", i, c[0], c[1], c[2], c[3], h[0], h[1], h[2], h[3]);
                }
            }
        for (uint32_t k : vs.info.vertexFetchConstants)
        {
            const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + k * 2), d1 = gpureg::Get(gpureg::FetchConstants + k * 2 + 1);
            const uint32_t* v = GuestPtr<uint32_t>(GpuPhysToGuest(d0 & 0xFFFFFFFC));
            Log("    vf%u: %08X size %u endian %u: %08X %08X %08X %08X %08X %08X %08X %08X", k, d0 & 0xFFFFFFFC, ((d1 >> 2) & 0xFFFFFF) * 4, d1 & 3,
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            if ((initiator & 0x3F) == 8)
            {
                // Rectangle lists: print the corners as floats (big-endian vertex data).
                std::string corners;
                for (uint32_t i = 0; i < 3 && (i * 7 + 1) * 4 < ((d1 >> 2) & 0xFFFFFF) * 4; i++)
                {
                    uint32_t xb = __builtin_bswap32(v[i * 7]), yb = __builtin_bswap32(v[i * 7 + 1]);
                    float x, y;
                    memcpy(&x, &xb, 4);
                    memcpy(&y, &yb, 4);
                    char t[48];
                    snprintf(t, sizeof(t), " (%g,%g)", x, y);
                    corners += t;
                }
                Log("    rect corners:%s", corners.c_str());
            }
            std::string nz;
            for (uint32_t i = 0; i * 8 + 5 < ((d1 >> 2) & 0xFFFFFF) && i < 24; i++)
            {
                char t[16];
                snprintf(t, sizeof(t), " %08X", v[i * 8 + 5]);
                nz += t;
            }
            Log("    vf%u normal z per vertex:%s", k, nz.c_str());
            // FM2_TRACE_VSTAT=<dword>,<stride>: min/max/mean of that big-endian float over the
            // vertices this draw indexes (e.g. 3,15: the per-vertex dirt of the car body).
            if (const char* e = getenv("FM2_TRACE_VSTAT"))
            {
                uint32_t off = 0, stride = 0;
                const uint32_t dwordsInBuffer = (d1 >> 2) & 0xFFFFFF;
                if (sscanf(e, "%u,%u", &off, &stride) == 2 && stride && off < stride && !list.empty())
                {
                    double lo = 1e30, hi = -1e30, sum = 0;
                    uint32_t n = 0, above = 0;
                    for (uint32_t index : list)
                    {
                        const uint64_t at = uint64_t(index) * stride + off;
                        if (at >= dwordsInBuffer)
                            continue;
                        const uint32_t b = __builtin_bswap32(v[at]);
                        float f;
                        memcpy(&f, &b, 4);
                        lo = std::min<double>(lo, f);
                        hi = std::max<double>(hi, f);
                        sum += f;
                        n++;
                        above += f > 0.5f;
                    }
                    if (n)
                        Log("    vf%u dword %u of %u: min %g max %g mean %g, %u of %u above 0.5", k, off, stride, lo, hi, sum / n, above, n);
                }
            }
        }
        Log("    scissors: generic %08X-%08X screen %08X-%08X window %08X-%08X offset %08X vport %g,%g +%g,%g", gpureg::Get(0x2090), gpureg::Get(0x2091),
            gpureg::Get(0x200E), gpureg::Get(0x200F), gpureg::Get(PA_SC_WINDOW_SCISSOR_TL), gpureg::Get(PA_SC_WINDOW_SCISSOR_BR), gpureg::Get(PA_SC_WINDOW_OFFSET),
            RegFloat(PA_CL_VPORT_XOFFSET), RegFloat(PA_CL_VPORT_YOFFSET), RegFloat(PA_CL_VPORT_XSCALE), RegFloat(PA_CL_VPORT_YSCALE));
        Log("    vport z scale %g offset %g, vte %08X, clip %08X, depth clamp/zrange regs %08X %08X", RegFloat(PA_CL_VPORT_ZSCALE), RegFloat(PA_CL_VPORT_ZOFFSET), gpureg::Get(0x2206), gpureg::Get(0x2204), gpureg::Get(0x2280), gpureg::Get(0x2281));
        Log("    alpha ref %g (pinfo.x %u) edram mode %u colormask %X", RegFloat(RB_ALPHA_REF), pc->info[0], edramMode, gpureg::Get(RB_COLOR_MASK));
        Log("    surface %08X color %08X depth %08X depthctl %08X stencil %08X colorctl %08X mode %X", gpureg::Get(0x2000), gpureg::Get(0x2001), gpureg::Get(0x2002),
            gpureg::Get(RB_DEPTHCONTROL), gpureg::Get(RB_STENCILREFMASK), gpureg::Get(RB_COLORCONTROL), gpureg::Get(PA_SU_SC_MODE_CNTL));
        Log("    poly offset front %g x + %g, back %g x + %g (depth info %08X)", RegFloat(0x2380), RegFloat(0x2381), RegFloat(0x2382), RegFloat(0x2383), gpureg::Get(0x2002));
        for (uint32_t c : { 0u, 1u, 5u, 6u, 7u, 14u, 22u, 254u, 255u })
            Log("    pc%u = %g %g %g %g", c, pc->c[c][0], pc->c[c][1], pc->c[c][2], pc->c[c][3]);
    }
    {
        const auto t0 = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const D3D_PRIMITIVE_TOPOLOGY topology = indexedCount && prim == 6 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (topology != g_drawState.topology)
        {
            g_drawState.topology = topology;
            C().list->IASetPrimitiveTopology(topology);
        }
        if (indexedCount)
        {
            if (memcmp(&indexView, &g_drawState.indexBuffer, sizeof(indexView)) != 0)
            {
                g_drawState.indexBuffer = indexView;
                C().list->IASetIndexBuffer(&indexView);
            }
            C().list->DrawIndexedInstanced(indexedCount, 1, 0, 0, 0);
        }
        else
            C().list->DrawInstanced(directIndex ? directVertices : UINT(list.size()), 1, 0, 0);
        if (g_perfTiming)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 1.0)
                Log("perf: DrawInstanced took %.1f ms (vs %016llx ps %016llx)", ms, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
        }
    }
    Section(6);
    g_r.drawsThisFrame++;
}

}

// ---- Benchmark (benchmark.cmd) ------------------------------------------------------
// Frame times while recording (the tour's timed race), and the report written at the end.
std::atomic<bool> g_benchRecording{ false };
std::mutex g_benchMutex;
std::vector<float> g_benchFrames; // ms

void BenchmarkRecord(bool on)
{
    g_benchRecording.store(on, std::memory_order_relaxed);
}

void BenchmarkWriteReport(const char* path)
{
    std::vector<float> frames;
    {
        std::lock_guard lock(g_benchMutex);
        frames = g_benchFrames;
    }
    std::string cpu = "?", gpu = "?";
    {
        char name[256];
        DWORD size = sizeof(name);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr,
                name, &size) == ERROR_SUCCESS)
            cpu = name;
        while (!cpu.empty() && cpu.back() == ' ')
            cpu.pop_back();
    }
    uint32_t cores = 0;
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        std::vector<uint8_t> buffer(bytes);
        if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes))
            for (DWORD at = 0; at < bytes; at += reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at)->Size)
                cores++;
    }
    MEMORYSTATUSEX memory{ sizeof(memory) };
    GlobalMemoryStatusEx(&memory);
    if (g_r.device)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(g_r.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            gpu = std::string(name) + " (" + std::to_string(desc.DedicatedVideoMemory >> 20) + " MB)";
        }
    }
    const Settings& st = GetSettings();
    static const char* upscalers[] = { "off", "FSR 1", "FSR 3", "DLSS" };
    static const char* qualities[] = { "native (AA)", "quality", "balanced", "performance", "ultra performance" };
    std::string upscaler = upscalers[std::min<uint32_t>(uint32_t(st.upscaler), 3)];
    if (st.upscaler != Settings::Upscaler::Off)
        upscaler += std::string(", ") + qualities[std::min<uint32_t>(st.quality, 4)];

    FILE* out = fopen(path, "w");
    if (!out)
        return;
    fprintf(out, "FM2 benchmark (Sunset race, 8 cars, throttle held)\n\n");
    fprintf(out, "CPU:        %s (%u cores, %u threads)\n", cpu.c_str(), cores, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    fprintf(out, "GPU:        %s\n", gpu.c_str());
    fprintf(out, "RAM:        %.0f GB\n", memory.ullTotalPhys / 1073741824.0);
    fprintf(out, "Resolution: %ux%u%s\n", OutputWidth(), OutputHeight(), st.fullscreen ? " fullscreen" : " windowed");
    fprintf(out, "Upscaler:   %s\n\n", upscaler.c_str());
    if (frames.size() < 100)
    {
        fprintf(out, "Not enough frames recorded (%zu): the run did not reach the race. Start the game once with play.cmd\n"
                     "(the first start compiles shaders), then run the benchmark again.\n", frames.size());
        fclose(out);
        return;
    }
    double total = 0;
    for (float f : frames)
        total += f;
    std::vector<float> sorted = frames;
    std::sort(sorted.begin(), sorted.end());
    // Lows: the average frame rate over the slowest 1% / 0.1% of frames.
    auto low = [&](double fraction) {
        const size_t n = std::max<size_t>(1, size_t(sorted.size() * fraction));
        double sum = 0;
        for (size_t i = sorted.size() - n; i < sorted.size(); i++)
            sum += sorted[i];
        return 1000.0 * double(n) / sum;
    };
    fprintf(out, "Average:    %.1f fps (%zu frames in %.1f s)\n", 1000.0 * double(frames.size()) / total, frames.size(), total / 1000.0);
    fprintf(out, "1%% low:     %.1f fps\n", low(0.01));
    fprintf(out, "0.1%% low:   %.1f fps\n", low(0.001));
    fprintf(out, "Slowest:    %.1f ms\n", double(sorted.back()));
    // Pacing: how much consecutive frames differ (0 for perfectly even frames).
    double jitter = 0;
    for (size_t i = 1; i < frames.size(); i++)
        jitter += std::fabs(double(frames[i]) - double(frames[i - 1]));
    fprintf(out, "Pacing:     %.2f ms average change between frames, median frame %.2f ms\n", jitter / double(frames.size() - 1),
        double(sorted[sorted.size() / 2]));
    fprintf(out, "Shaders:    %u compiled, %u from cache; pipelines %u built, %u from cache\n", g_stats.shadersBuilt.load(), g_stats.shadersCached.load(),
        g_stats.pipelinesBuilt.load(), g_stats.pipelinesLoaded.load());
    // Timeline: average and slowest frame per 5 s (does the first stretch run slower?).
    fprintf(out, "Timeline:  ");
    double windowMs = 0, windowMax = 0;
    size_t windowFrames = 0;
    for (float f : frames)
    {
        windowMs += f;
        windowMax = std::max(windowMax, double(f));
        windowFrames++;
        if (windowMs >= 5000.0)
        {
            fprintf(out, " %.0f fps (max %.1f ms) |", 1000.0 * double(windowFrames) / windowMs, windowMax);
            windowMs = windowMax = 0;
            windowFrames = 0;
        }
    }
    fprintf(out, "\n");
    fclose(out);
    Log("benchmark: report written to %s", path);
}

void RendererOnSwapImpl(uint64_t frameNumber, uint32_t frontBuffer);
void RendererOnSwap(uint64_t frameNumber, uint32_t frontBuffer)
{
    const auto t0 = std::chrono::steady_clock::now();
    RendererOnSwapImpl(frameNumber, frontBuffer);
    const auto now = std::chrono::steady_clock::now();
    g_perf.swapSeconds += std::chrono::duration<double>(now - t0).count();
    g_perf.frames++;
    static LiveSwitch perf("FM2_PERF");
    static auto lastSwap = now;
    static double frameDraw = 0, lastDrawTotal = 0;
    {
        const double dt = std::chrono::duration<double>(now - lastSwap).count() * 1000.0;
        lastSwap = now;
        if (g_benchRecording.load(std::memory_order_relaxed))
        {
            std::lock_guard lock(g_benchMutex);
            g_benchFrames.push_back(float(dt));
        }
        g_perf.maxFrame = std::max(g_perf.maxFrame, dt);
        if (dt > 20.0)
            g_perf.slowFrames++;
        const double drawNow = g_perf.drawSeconds * 1000.0;
        static double drawBefore = 0;
        const double frameDrawMs = drawNow >= drawBefore ? drawNow - drawBefore : drawNow;
        drawBefore = drawNow;
        static const double slowMs = getenv("FM2_SLOW_MS") ? atof(getenv("FM2_SLOW_MS")) : 25.0; // slow-frame log threshold
        if (perf && dt > slowMs)
        {
            void SpikeDump(double ms);
            SpikeDump(dt + 2.0);
        }
        if (perf && dt > slowMs)
            Log("perf: slow frame %llu: %.1f ms | draw %.1f ms (worst draw %.1f; shaders %.1f indices %.1f pipeline %.1f constants %.1f "
                "textures %.1f submit %.1f), waited %.1f ms on %u builds, %u pipelines started, %u resources created",
                (unsigned long long)frameNumber, dt, frameDrawMs, g_frameEvents.worstDrawMs, g_frameEvents.section[0], g_frameEvents.section[1],
                g_frameEvents.section[2], g_frameEvents.section[3], g_frameEvents.section[4], g_frameEvents.section[5], g_frameEvents.waitMs,
                g_frameEvents.waits, g_frameEvents.pipelines, g_frameEvents.created);
        g_frameEvents = {};
    }
    const double window = std::chrono::duration<double>(now - g_perf.windowStart).count();
    if (window >= 1.0)
    {
        if (perf && g_perf.frames)
            Log("perf: %.1f fps, %.2f ms/frame | renderer draw %.2f ms, swap %.2f ms per frame | %.0f draws/frame", g_perf.frames / window,
                window * 1000.0 / g_perf.frames, g_perf.drawSeconds * 1000.0 / g_perf.frames, g_perf.swapSeconds * 1000.0 / g_perf.frames,
                double(g_perf.draws) / g_perf.frames);
        if (perf && g_perf.frames)
            Log("perf:   shaders %.2f | indices %.2f | pipeline %.2f | constants %.2f | textures %.2f | submit %.2f ms/frame",
                g_perf.section[0] * 1000.0 / g_perf.frames, g_perf.section[1] * 1000.0 / g_perf.frames, g_perf.section[2] * 1000.0 / g_perf.frames,
                g_perf.section[3] * 1000.0 / g_perf.frames, g_perf.section[4] * 1000.0 / g_perf.frames, g_perf.section[5] * 1000.0 / g_perf.frames);
        if (perf && g_perf.frames)
            Log("perf:   constants split: index list %.2f | constant banks %.2f | vertex fetch %.2f (bools and loops %.2f, uploads %.2f) | the rest %.2f (MSAA scale %.2f, index buffer %.2f) ms/frame",
                g_perf.sub[0] * 1000.0 / g_perf.frames, g_perf.sub[1] * 1000.0 / g_perf.frames, g_perf.sub[2] * 1000.0 / g_perf.frames, g_perf.sub[4] * 1000.0 / g_perf.frames, g_perf.sub[5] * 1000.0 / g_perf.frames, g_perf.sub[3] * 1000.0 / g_perf.frames, g_perf.sub[6] * 1000.0 / g_perf.frames, g_perf.sub[7] * 1000.0 / g_perf.frames);
        if (perf)
            Log("perf:   cpu%s | slowest frame %.1f ms, %llu over 20 ms", GuestThreadCpuUsage(window).c_str(), g_perf.maxFrame,
                (unsigned long long)g_perf.slowFrames);
        if (perf && g_perf.frames)
            Log("perf:   waiting on host GPU %.2f ms/frame | GPU time %.2f ms/frame (worst %.1f)", g_gpuWaitSeconds * 1000.0 / g_perf.frames,
                g_gpuFrames ? g_gpuFrameMs / g_gpuFrames : 0.0, g_gpuFrameMaxMs);
        g_gpuFrameMs = g_gpuFrameMaxMs = 0;
        g_gpuFrames = 0;
        if (perf && g_perf.frames)
            Log("perf:   descriptor tables built %.0f, reused %.0f per frame", double(g_perf.tablesBuilt) / g_perf.frames, double(g_perf.tablesReused) / g_perf.frames);
        if (perf && !g_created.empty())
        {
            std::string s;
            for (auto& [what, n] : g_created)
                s += " " + what + ":" + std::to_string(n);
            Log("perf:   resources created this second:%s", s.c_str());
        }
        if (perf && g_pipelinesCreated)
            Log("perf:   pipelines started %u", g_pipelinesCreated);
        if (perf && (g_scanPrefound || g_scanMissed))
            Log("perf:   new shaders at draw time: %u already found by scan, %u not", g_scanPrefound, g_scanMissed);
        g_scanPrefound = g_scanMissed = 0;
        if (perf && g_skippedPipeline)
            Log("perf:   draws skipped waiting for pipelines: %u", g_skippedPipeline);
        g_skippedPipeline = 0;
        g_pipelineMs = 0;
        g_pipelinesCreated = 0;
        g_pipelineLibrary.MaybeSave();
        g_created.clear();
        g_gpuWaitSeconds = 0;
        g_perf = Perf{};
    }
}

// ---- Precompile (test build): before the game goes on, compile every stored shader and
// build every stored pipeline, with the overlay up. The stored set is whatever earlier
// runs recorded (shadercache/*.ucode and pipelines.manifest), so the more the game has
// been played, the more is covered. FM2_NO_PRECOMPILE=1 turns it off.
bool g_precompileDone = false;

void PrecompileAll()
{
    if (g_precompileDone)
        return;
    g_precompileDone = true;
    // From here on the game runs: speculative finds wait for a draw that needs them.
    struct ParkAfter
    {
        ~ParkAfter() { g_compiler.park = true; }
    } parkAfter;
    if (getenv("FM2_NO_PRECOMPILE"))
        return;
    const auto start = std::chrono::steady_clock::now();
    // Only the shaders stored pipelines use: older caches also hold scan finds that never drew.
    std::vector<PipelineKey> keys = g_manifest.pending;
    std::vector<std::pair<bool, uint64_t>> shaders;
    {
        std::set<std::pair<bool, uint64_t>> used;
        for (const PipelineKey& k : keys)
            for (const auto s : { std::pair{ false, k.vs }, std::pair{ true, k.ps } })
                if (used.insert(s).second)
                    shaders.push_back(s);
    }
    g_stats.warmTotal = uint32_t(keys.size());
    g_stats.warmDone = 0;
    Log("precompile: %zu shaders and %zu pipelines stored", shaders.size(), keys.size());
    size_t nextShader = 0, nextKey = 0;
    size_t shadersInstalled = 0, pipelinesStarted = 0;
    auto lastPresent = std::chrono::steady_clock::time_point{};
    for (;;)
    {
        // Phase 1: load and translate shaders a batch at a time (compiles run on worker threads).
        for (int batch = 0; batch < 64 && nextShader < shaders.size(); batch++, nextShader++)
        {
            const auto [pixel, hash] = shaders[nextShader];
            if (g_r.shaders.count(hash))
                continue;
            std::vector<uint32_t> code;
            if (Manifest::LoadShader(pixel, hash, code))
            {
                GetShader(hash, code, pixel);
                shadersInstalled++;
            }
        }
        // Phase 2: start pipelines whose shaders are ready.
        for (int batch = 0; batch < 256 && nextKey < keys.size(); batch++)
        {
            const PipelineKey key = keys[nextKey];
            const CompiledShader* vs = nullptr;
            const CompiledShader* ps = nullptr;
            if (auto f = g_r.shaders.find(key.vs); f != g_r.shaders.end())
                vs = f->second.get();
            if (auto f = g_r.shaders.find(key.ps); f != g_r.shaders.end())
                ps = f->second.get();
            if (!vs || !ps)
            {
                // Shader not loaded yet: load it now (translation queues the compile).
                std::vector<uint32_t> code;
                if (!vs && Manifest::LoadShader(false, key.vs, code))
                    vs = &GetShader(key.vs, code, false);
                code.clear();
                if (!ps && Manifest::LoadShader(true, key.ps, code))
                    ps = &GetShader(key.ps, code, true);
            }
            if (!vs || !ps || vs->state.load() == CompiledShader::Pending || ps->state.load() == CompiledShader::Pending)
                break; // wait for this one to finish compiling before going on
            if (vs->state.load() == CompiledShader::Ready && ps->state.load() == CompiledShader::Ready)
            {
                if (!g_r.pipelines.count(key))
                {
                    EnsurePipeline(*vs, *ps, key);
                    pipelinesStarted++;
                }
            }
            nextKey++;
            g_stats.warmDone++;
        }
        // Done when every shader is loaded and every pipeline is built.
        size_t building = 0;
        for (auto& [k, e] : g_r.pipelines)
            if (e->state.load() == 0)
                building++;
        const bool allSent = nextShader >= shaders.size() && nextKey >= keys.size();
        if (allSent && g_compiler.jobs.load() == 0 && building == 0)
            break;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPresent > std::chrono::milliseconds(33))
        {
            lastPresent = now;
            g_hold.active = true;
            HoldPresent();
        }
        Sleep(1);
    }
    g_hold.active = false;
    // A warm start never has a long loading hold, so without this settled skipping
    // never arms and each first-time effect in a race stalls for its compile.
    if (!keys.empty())
        g_lastLongHold = std::chrono::steady_clock::now();
    // Input recordings and replays count from here: how long the boot took (shader compiles)
    // varies from run to run, the game's own timing after it doesn't.
    extern std::atomic<int64_t> g_inputEpochNs;
    g_inputEpochNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    Log("precompile: done in %.1f s (%zu shaders loaded, %zu pipelines started)",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), shadersInstalled, pipelinesStarted);

    // Every shader in the game data goes to the compile queue as well. Compiles run on
    // the background workers and the game doesn't wait for them; cached bytecode loads
    // straight away, so only the first boot pays (FM2_NO_STATIC_SHADERS=1 skips this).
    if (getenv("FM2_NO_STATIC_SHADERS"))
        return;
    const auto staticStart = std::chrono::steady_clock::now();
    size_t queued = 0;
    std::vector<const CompiledShader*> compiling;
    // The first boot's search of the game files takes a few seconds: say so on screen.
    std::vector<uint8_t> bootFrame;
    if (!StaticShadersSaved() && BootScreenCompose(L"PREPARING SHADERS", 0, 0, bootFrame))
        BootPresent(bootFrame);
    for (const ScannedShader& s : StaticShaders())
    {
        // The scanner accepts a candidate on control flow alone, and a few of what it finds in
        // the mesh files is not a shader (ucode::Plausible). Translating one fails on an opcode
        // the ISA doesn't define, after paying a translation on every boot.
        if (!ucode::Plausible(ucode::Parse(s.code.data(), uint32_t(s.code.size()), s.pixel)))
            continue;
        const uint64_t hash = Fnv(s.code.data(), s.code.size() * 4) ^ (s.pixel ? 1 : 0);
        if (g_r.shaders.count(hash))
            continue;
        g_scanKnown.insert(hash);
        const CompiledShader& cs = GetShader(hash, s.code, s.pixel, true);
        if (cs.state.load() == CompiledShader::Pending)
            compiling.push_back(&cs);
        queued++;
    }
    // Compiling them while the game runs halves the frame rate for minutes, so the first
    // boot waits here with the overlay up; later boots find the bytecode cached and
    // queue nothing.
    const int toCompile = int(compiling.size());
    Log("static shaders: %zu new, %d to compile (prepared in %.1f s)", queued, toCompile,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - staticStart).count());
    if (!toCompile)
        return;
    g_stats.staticTotal = uint32_t(toCompile);
    g_stats.staticDone = 0;
    const auto compileStart = std::chrono::steady_clock::now();
    auto lastShown = std::chrono::steady_clock::time_point{}, lastScan = std::chrono::steady_clock::now();
    for (size_t next = 0;;)
    {
        while (next < compiling.size() && compiling[next]->state.load() != CompiledShader::Pending)
            next++;
        if (next == compiling.size())
            break;
        const auto now = std::chrono::steady_clock::now();
        // The game keeps running meanwhile and builds the shaders it makes at runtime (the
        // title's among them): compile those in the same wait.
        if (now - lastScan > std::chrono::milliseconds(1500))
        {
            lastScan = now;
            ShaderScanNow(2);
            const size_t before = compiling.size();
            DrainScan(0, &compiling);
            g_stats.staticTotal += uint32_t(compiling.size() - before);
        }
        if (now - lastShown > std::chrono::milliseconds(100))
        {
            lastShown = now;
            g_stats.staticDone = uint32_t(std::count_if(compiling.begin(), compiling.end(),
                [](const CompiledShader* cs) { return cs->state.load() != CompiledShader::Pending; }));
            if (BootScreenCompose(L"COMPILING SHADERS", g_stats.staticDone.load(), g_stats.staticTotal.load(), bootFrame))
                BootPresent(bootFrame);
            else
            {
                g_hold.active = true;
                HoldPresent();
            }
        }
        Sleep(5);
    }
    g_hold.active = false;
    g_stats.staticTotal = 0;
    g_lastLongHold = std::chrono::steady_clock::now();
    Log("static shaders: %zu compiled in %.1f s (%zu found in memory meanwhile)", compiling.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count(), compiling.size() - size_t(toCompile));
}

void RendererOnSwapImpl(uint64_t frameNumber, uint32_t frontBuffer)
{
    {
        // FM2_TRACE_DIRECT: buffers recorded last frame that the game has rewritten since.
        std::lock_guard lock(g_directTrace.mutex);
        for (auto& [address, e] : g_directTrace.seen)
            if (e.frame + 1 == g_r.frame)
            {
                g_directTrace.checked++;
                const uint64_t now = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(address)), e.bytes);
                if (now != e.hash && g_directTrace.changed++ < 200)
                    Log("direct: %08X %u bytes (vs %016llx) rewritten after frame %llu's depth-only draw (%u of %u checked)", address, e.bytes,
                        (unsigned long long)e.vs, (unsigned long long)e.frame, g_directTrace.changed, g_directTrace.checked);
            }
    }
    void MemScanOnSwap(uint64_t frame);
    MemScanOnSwap(g_r.frame);
    if (!g_r.ok)
        return;
    PrecompileAll();
    WarmupStep();
    BeginFrameIfNeeded();
    FlushPendingUpscale();
    // A frame that rendered small but never resolved a scene (a menu): upscale what it shows
    // and stay at full size for a while (menus are drawn sharp at full size).
    // FSR 1: the whole frame, HUD included, rendered at the render scale; upscaled and sharpened
    // as it is shown (at 100%: sharpened only). Nothing depends on finding the 3D scene.
    const bool fsr1 = GetSettings().upscaler == Settings::Upscaler::Fsr1;
    if (fsr1)
    {
        g_sceneScale = 1.0f;
        g_scenePhase = false;
    }
    else if (g_scenePhase && !g_upscale.switchedThisFrame)
    {
        UpscaleResolved(frontBuffer);
        g_sceneScale = 1.0f;
        g_scenePhase = false;
        g_upscale.retryFrame = g_r.frame + 120;
        g_upscale.reset = true; // the next scene has no usable history
    }
    // Show the front buffer the title resolved to; fall back to the render
    // target if it was never resolved.
    ID3D12Resource* image = C().color.Get();
    D3D12_RESOURCE_STATES imageState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto it = g_r.resolved.find(frontBuffer);
    const bool fromResolve = it != g_r.resolved.end() && it->second.width == kWidth && it->second.height == kHeight &&
        it->second.format == DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Resource* graded = nullptr; // the front buffer through the display's gamma table
    if (fromResolve)
    {
        image = it->second.resource.Get();
        if (fsr1)
            if (ID3D12Resource* fsr = Fsr1Present(it->second))
                image = fsr;
        graded = GradeFrontBuffer(image);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = image;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        C().list->ResourceBarrier(1, &b);
    }
    // The render target is half float; without an 8-bit resolve there is
    // nothing presentable this frame.
    if (fromResolve)
        g_hold.image = it->second.resource;
    ID3D12Resource* overlay = fromResolve ? PrepareOverlay() : nullptr;
    ID3D12Resource* shown = graded ? graded : image;
    if (fromResolve)
        if (ID3D12Resource* enhanced = EnhanceFrontBuffer(shown))
            shown = enhanced;
    const bool presenting = fromResolve && PresenterRecordCopy(C().list->Real(), shown, overlay, kOverlayW, kOverlayH);
    {
        const uint64_t traceFrame = TraceFrame();
        if (traceFrame && g_r.frame + 1 == traceFrame)
            Log("swap: front buffer %08X, %s", frontBuffer, fromResolve ? "resolved" : "not resolved: showing the render target");
    }

    static uint64_t dumpFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
    // A capture asked for some other way (fm2ctl capture: RendererRequestCapture) dumps that frame too.
    if (const uint64_t hot = g_hotkeyFrame.load(std::memory_order_relaxed); hot > dumpFrame && hot > g_r.frame)
        dumpFrame = hot;
    {
        // F12 in the game window: capture the next frame (frame.png, the resolves in out\res,
        // and the frame's draws in out/play.log) to report rendering bugs.
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (down && !wasDown && pid == GetCurrentProcessId())
        {
            g_hotkeyFrame = g_r.frame + 2;
            dumpFrame = g_r.frame + 2;
            Log("capture: F12 pressed, tracing frame %llu", (unsigned long long)dumpFrame);
        }
        wasDown = down;
    }
    // FM2_CAPTURE_TALL=1: the same capture as F12, once a pass taller than the screen was drawn.
    {
        static LiveSwitch captureTall("FM2_CAPTURE_TALL");
        static bool capturedTall = false;
        if (captureTall && !capturedTall && g_tallSeen)
        {
            capturedTall = true;
            dumpFrame = g_r.frame + 1; // the frame the tall pass was drawn in (already being traced)
            Log("capture: tall pass seen, dumping frame %llu", (unsigned long long)dumpFrame);
        }
    }
    // FM2_CAPTURE_AT=<seconds>: the same capture as F12, that long after the start (for replays).
    {
        static const double captureAt = getenv("FM2_CAPTURE_AT") ? atof(getenv("FM2_CAPTURE_AT")) : 0;
        static const auto captureStart = std::chrono::steady_clock::now();
        static bool captured = false;
        if (captureAt > 0 && !captured && std::chrono::duration<double>(std::chrono::steady_clock::now() - captureStart).count() >= captureAt)
        {
            captured = true;
            g_hotkeyFrame = g_r.frame + 2;
            dumpFrame = g_r.frame + 2;
            Log("capture: FM2_CAPTURE_AT reached, tracing frame %llu", (unsigned long long)dumpFrame);
        }
    }
    ++g_r.frame;
    prehash::StartFrame();
    DebugOnSwap(g_r.frame);
    if (int st = g_drawStats.load(); st) // drawstats: arm at one swap, stop at the next
        g_drawStats = st == 1 ? 2 : 0;
    // The debug server's "shot": this frame (when it has a picture), to the file it names.
    std::string shotPath;
    const bool shot = fromResolve && DebugShotPending(shotPath);
    if (shot)
        dumpFrame = g_r.frame;
    // FM2_DUMP_AT=<seconds>: dump the first resolved frame after that long (comparable
    // screenshots whatever the frame rate).
    {
        static const double dumpAt = getenv("FM2_DUMP_AT") ? atof(getenv("FM2_DUMP_AT")) : 0;
        static const auto start = std::chrono::steady_clock::now();
        if (dumpAt > 0 && !dumpFrame && fromResolve && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= dumpAt)
            dumpFrame = g_r.frame;
    }
    g_tile = TileState{};
    static LiveSwitch dumpSecond("FM2_SKIP_VS");
    static const uint64_t dumpEvery = getenv("FM2_DUMP_EVERY") ? strtoull(getenv("FM2_DUMP_EVERY"), nullptr, 10) : 0;
    static uint32_t sweepCountForDump = [] { uint32_t a = 0, b = 0, c = 0; if (const char* e = getenv("FM2_SWEEP")) sscanf(e, "%u,%u,%u", &a, &b, &c); return c; }();
    const bool sweepDump = fromResolve && sweepCountForDump && dumpFrame && g_r.frame >= dumpFrame && g_r.frame < dumpFrame + sweepCountForDump;
    const bool dump = sweepDump || (fromResolve && ((dumpFrame && (g_r.frame == dumpFrame || (dumpSecond && g_r.frame == dumpFrame + 1))) || (dumpEvery && g_r.frame % dumpEvery == 0)));
    if (dump)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_r.readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, OutputWidth(), OutputHeight(), 1, (OutputWidth() * 4 + 255) & ~255u };
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = shown;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    // FM2_DUMP_RESOLVES=1 (with FM2_DUMP_FRAME): also write every resolved texture to out/res.
    static std::vector<ResolveDump> resolveDumps;
    resolveDumps.clear();
    if (dump && (getenv("FM2_DUMP_RESOLVES") || (g_hotkeyFrame.load() && g_r.frame == dumpFrame)))
    {
        auto add = [&](uint32_t base, ResolvedTexture& r, int variant) {
            if (!r.resource)
                return;
            ResolveDump d;
            d.base = base;
            d.variant = variant;
            d.width = r.hostWidth; // the texture's own size
            d.height = r.hostHeight;
            d.format = r.format;
            const UINT bpp = r.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
            d.pitch = (d.width * bpp + 255) & ~255u;
            d.buffer = CreateBuffer(UINT64(d.pitch) * d.height, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER bar{};
            bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar.Transition.pResource = r.resource.Get();
            bar.Transition.Subresource = 0;
            bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            C().list->ResourceBarrier(1, &bar);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = d.buffer.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { r.format, d.width, d.height, 1, d.pitch };
            src.pResource = r.resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
            C().list->ResourceBarrier(1, &bar);
            resolveDumps.push_back(std::move(d));
        };
        for (auto& [base, r] : g_r.resolved)
        {
            add(base, r, 0);
            int v = 1;
            for (ResolvedTexture& spare : r.spares)
                add(base, spare, v++);
        }
    }
    if (fromResolve)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = image;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        C().list->ResourceBarrier(1, &b);
        if (graded)
        {
            b.Transition.pResource = graded;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            C().list->ResourceBarrier(1, &b);
        }
    }

    FlushIndexCopies(); // this frame's index uploads into the arena, after its draws
    if (g_gpuPasses && g_passQueries && !g_passMarks[UINT(&Frame() - g_r.frames)].empty())
    {
        const UINT frameIndex = UINT(&Frame() - g_r.frames);
        PassMark("frame end %u%u", 0, 0);
        const UINT n = UINT(g_passMarks[frameIndex].size());
        C().list->ResolveQueryData(g_passQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * kPassMarks, n, g_passReadback.Get(),
            UINT64(frameIndex) * kPassMarks * 8);
    }
    if (g_timestamps)
    {
        const UINT frameIndex = UINT(&Frame() - g_r.frames);
        C().list->EndQuery(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2 + 1);
        C().list->ResolveQueryData(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2, 2, g_timestampReadback.Get(), UINT64(frameIndex) * 16);
        Frame().timed = true;
    }
    C().list->Close();
    ID3D12CommandList* lists[] = { C().list->Real() };
    SubmitLists(1, lists);
    if (presenting)
        PresenterPresent();
    Frame().fence = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), Frame().fence);
    g_r.listOpen = false;

    if (dump)
    {
        g_r.fence->SetEventOnCompletion(Frame().fence, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, INFINITE);
        for (ResolveDump& d : resolveDumps)
        {
            uint8_t* mapped = nullptr;
            d.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            std::vector<uint8_t> out(size_t(d.width) * d.height * 4);
            for (UINT y = 0; y < d.height; y++)
                for (UINT x = 0; x < d.width; x++)
                {
                    uint8_t* o = &out[(size_t(y) * d.width + x) * 4];
                    if (d.format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                    {
                        const uint16_t* h = reinterpret_cast<const uint16_t*>(mapped + size_t(y) * d.pitch) + x * 4;
                        for (int c = 0; c < 4; c++)
                        {
                            const uint32_t e = (h[c] >> 10) & 31, m = h[c] & 1023;
                            float f = e == 0 ? m / 1024.0f * 6.1035e-5f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, int(e) - 15);
                            if (h[c] & 0x8000)
                                f = -f;
                            o[c] = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                    }
                    else if (d.format == DXGI_FORMAT_R32_FLOAT)
                    {
                        const float f = reinterpret_cast<const float*>(mapped + size_t(y) * d.pitch)[x];
                        if (x == 0 && y == 0)
                            d.minDepth = d.maxDepth = f;
                        d.minDepth = std::min(d.minDepth, f);
                        d.maxDepth = std::max(d.maxDepth, f);
                        if (f > 0.0f && f < 1.0f)
                            d.midCount++;
                        o[0] = o[1] = o[2] = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                        o[3] = 255;
                    }
                    else
                        memcpy(o, mapped + size_t(y) * d.pitch + x * 4, 4);
                    o[3] = 255;
                }
            d.buffer->Unmap(0, nullptr);
            if (d.format == DXGI_FORMAT_R32_FLOAT)
                Log("resolve dump: depth %08X %ux%u min %g max %g strictly-inside %llu", d.base, d.width, d.height, d.minDepth, d.maxDepth,
                    (unsigned long long)d.midCount);
            std::filesystem::create_directories("out/res");
            char name[96];
            snprintf(name, sizeof(name), "out/res/%08X_%ux%u_v%d.png", d.base, d.width, d.height, d.variant);
            WritePng(name, d.width, d.height, out);
        }
        resolveDumps.clear();
        uint8_t* data = nullptr;
        g_r.readback->Map(0, nullptr, reinterpret_cast<void**>(&data));
        // Written at the render size; the region probes below work on a 1280x720 copy.
        const uint32_t dw = OutputWidth(), dh = OutputHeight(), dpitch = (dw * 4 + 255) & ~255u;
        std::vector<uint8_t> full(size_t(dw) * dh * 4);
        for (uint32_t y = 0; y < dh; y++)
            memcpy(&full[size_t(y) * dw * 4], data + size_t(y) * dpitch, size_t(dw) * 4);
        g_r.readback->Unmap(0, nullptr);
        for (size_t i = 3; i < full.size(); i += 4)
            full[i] = 255;
        std::vector<uint8_t> rgba(size_t(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; y++)
            for (uint32_t x = 0; x < kWidth; x++)
                memcpy(&rgba[(size_t(y) * kWidth + x) * 4], &full[(size_t(y) * dh / kHeight * dw + size_t(x) * dw / kWidth) * 4], 4);
        static const uint64_t every = getenv("FM2_DUMP_EVERY") ? strtoull(getenv("FM2_DUMP_EVERY"), nullptr, 10) : 0;
        // FM2_REGION_HASH="x,y,w,h" (with FM2_DUMP_EVERY): instead of a PNG, log a hash of the bright
        // pixels in that part of the output, to see how often it changes (text, animations).
        static uint32_t rx = 0, ry = 0, rw = 0, rh = 0;
        static const bool regionHash = [] {
            const char* e = getenv("FM2_REGION_HASH");
            return e && sscanf(e, "%u,%u,%u,%u", &rx, &ry, &rw, &rh) == 4;
        }();
        if (regionHash && every && g_r.frame % every == 0)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t y = ry; y < ry + rh && y < dh; y++)
                for (uint32_t x = rx; x < rx + rw && x < dw; x++)
                {
                    const uint8_t* px = &full[(size_t(y) * dw + x) * 4];
                    h = (h ^ (std::max({ px[0], px[1], px[2] }) > 90 ? 1u : 0u)) * 0x100000001B3ull;
                }
            Log("region: frame %llu hash %08X", (unsigned long long)g_r.frame, uint32_t(h ^ (h >> 32)));
        }
        else if (every && g_r.frame % every == 0 && g_r.frame != dumpFrame)
        {
            char name[64];
            std::filesystem::create_directories("out/frames");
            snprintf(name, sizeof(name), "out/frames/%05llu.png", (unsigned long long)g_r.frame);
            WritePng(name, dw, dh, full);
        }
        else if (sweepDump && getenv("FM2_SWEEP_STAT"))
        {
            // FM2_SWEEP_STAT="x,y,w,h": log the region's mean colour instead of writing a PNG.
            uint32_t x0 = 0, y0 = 0, w = 1, h = 1;
            sscanf(getenv("FM2_SWEEP_STAT"), "%u,%u,%u,%u", &x0, &y0, &w, &h);
            uint64_t sum[3] = {};
            for (uint32_t y = y0; y < y0 + h && y < kHeight; y++)
                for (uint32_t x = x0; x < x0 + w && x < kWidth; x++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += rgba[(size_t(y) * kWidth + x) * 4 + c];
            Log("sweep %llu: %llu %llu %llu", (unsigned long long)(g_r.frame - dumpFrame), (unsigned long long)(sum[0] / (w * h)),
                (unsigned long long)(sum[1] / (w * h)), (unsigned long long)(sum[2] / (w * h)));
        }
        else if (sweepDump)
        {
            std::filesystem::create_directories("out/sweep");
            char name[64];
            snprintf(name, sizeof(name), "out/sweep/%03llu.png", (unsigned long long)(g_r.frame - dumpFrame));
            WritePng(name, dw, dh, full);
        }
        else if (shot && g_r.frame == dumpFrame)
        {
            std::error_code ec;
            if (const auto dir = std::filesystem::path(shotPath).parent_path(); !dir.empty())
                std::filesystem::create_directories(dir, ec);
            WritePng(shotPath.c_str(), dw, dh, full);
            DebugShotDone(shotPath);
        }
        else
            WritePng(g_r.frame == dumpFrame ? "frame.png" : "frame2.png", dw, dh, full);
        Log("renderer: wrote frame.png (frame %llu: %u draws, %u skipped)", (unsigned long long)frameNumber, g_r.drawsThisFrame.load(),
            g_r.skippedThisFrame.load());
    }
    if (frameNumber % 600 == 0)
        Log("renderer: frame %llu: %u draws, %u skipped, %zu shaders, %zu pipelines, %zu textures", (unsigned long long)frameNumber,
            g_r.drawsThisFrame.load(), g_r.skippedThisFrame.load(), g_r.shaders.size(), g_r.pipelines.size(), g_r.textures.size());
    g_r.drawsThisFrame = g_r.skippedThisFrame = 0;
    g_r.frameIndex = (g_r.frameIndex + 1) % kFrames;
}

bool RendererTracing()
{
    const uint64_t traceFrame = TraceFrame();
    return traceFrame && g_r.frame + 1 == traceFrame;
}

// The camera aspect the game is given (hooks/camera_aspect.cpp): 16/9 widened by the view
// extent, so its projection and frustum culling cover the wider view.
float GameAspect()
{
    return 16.0f / 9.0f * ViewExtent();
}

void RendererSetGammaEntry(uint32_t index, uint32_t rgb10)
{
    if (index < 256)
    {
        g_gammaTable[index].store(rgb10, std::memory_order_relaxed);
        g_gammaSet.store(true, std::memory_order_relaxed);
    }
}

// The debug server's "capture": what F12 does.
// Two ahead, as F12: the next frame may already be part drawn (its first draws went untraced).
void RendererRequestCapture() { g_hotkeyFrame = g_r.frame + 2; }

// The debug server's "ps HASH [EXPR]": an empty expression drops the edit. Returns the alias (0 if dropped).
uint64_t RendererEditPixelShader(uint64_t hash, const std::string& expr)
{
    static std::atomic<uint64_t> serial{ 0 };
    std::lock_guard lock(g_psEditMutex);
    if (auto it = g_psEditAlias.find(hash); it != g_psEditAlias.end())
    {
        g_psEditExpr.erase(it->second);
        g_psEditAlias.erase(it);
    }
    uint64_t alias = 0;
    if (!expr.empty())
    {
        alias = hash ^ (0x9E3779B97F4A7C15ull * ++serial) ^ 0xD3B6000000000000ull;
        g_psEditAlias[hash] = alias;
        g_psEditExpr[alias] = expr;
        g_psEditEver.insert(alias);
    }
    g_psEdits = !g_psEditAlias.empty();
    return alias;
}

// The debug server's "hlsl HASH": the shader's translated source ("" if it hasn't been seen).
std::string RendererShaderHlsl(uint64_t hash)
{
    std::lock_guard lock(g_shaderMutex);
    auto it = g_r.shaders.find(hash);
    if (it == g_r.shaders.end())
        return {};
    const CompiledShader* cs = it->second.get();
    return cs->hlslSource ? cs->hlslSource->info.hlsl : cs->info.hlsl;
}

// Whether a live edit's alias is drawing: its shader compiled and a pipeline with it built.
bool RendererPsEditReady(uint64_t alias)
{
    std::lock_guard lock(g_shaderMutex);
    auto it = g_r.shaders.find(alias);
    if (it == g_r.shaders.end() || !it->second->IsReady())
        return false;
    for (const auto& [key, entry] : g_r.pipelines)
        if ((key.ps == alias || key.vs == alias) && entry->state.load(std::memory_order_acquire) == 1)
            return true;
    return false;
}

// drawstats: counts the next whole frame's draws by shader pair (blocks until it's done).
std::string RendererDrawStats()
{
    {
        std::lock_guard lock(g_drawStatsMutex);
        g_drawStatsCounts.clear();
    }
    g_drawStats = 1;
    for (int i = 0; i < 400 && g_drawStats.load() != 0; i++)
        Sleep(5);
    std::lock_guard lock(g_drawStatsMutex);
    std::string out;
    char buf[96];
    for (const auto& [k, n] : g_drawStatsCounts)
    {
        snprintf(buf, sizeof(buf), "%s{\"vs\":\"%016llx\",\"ps\":\"%016llx\",\"draws\":%u}", out.empty() ? "" : ",", (unsigned long long)k.first,
            (unsigned long long)k.second, n);
        out += buf;
    }
    return out;
}
