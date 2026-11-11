// AMD FSR 3 frame generation (FidelityFX API, MIT): see fsrfg.h. Needs amd_fidelityfx_framegeneration_dx12.dll
// next to the executable (copied there by the build) besides the loader.
//
// Each frame gets exactly one ffxConfigure with a frame id one above the last: with generation on from
// FsrFgPrepare (a frame with a scene: depth and motion given), otherwise with it off from FsrFgBeforePresent.
// The swap chain then calls back (Dispatch) from Present to make the frame in between.
#include "gpu/fsrfg.h"

#include <ffx_api.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.h"
#include "settings.h"

bool FsrFgAsync();

namespace
{
struct FrameGen
{
    std::mutex mutex; // the frame generation context is not thread safe (prepare, configure, destroy)
    ffxContext swapChainContext = nullptr;
    IDXGISwapChain4* swapChain = nullptr;
    ffxContext context = nullptr;
    uint32_t displayWidth = 0, displayHeight = 0, renderWidth = 0, renderHeight = 0;
    bool depthInverted = false;
    uint64_t frameId = 1;
    bool prepared = false; // this frame was configured (generation on) by FsrFgPrepare
    bool on = false;       // the state last configured
    bool failed = false;
    uint64_t made = 0;     // frames configured with generation on
} g_fg;

ffxReturnCode_t Dispatch(ffxDispatchDescFrameGeneration* params, void* user)
{
    return ffxDispatch(reinterpret_cast<ffxContext*>(user), &params->header);
}

// (With g_fg.mutex held.)
void Configure(bool on, ID3D12Resource* hudless = nullptr)
{
    if (!g_fg.context)
        return;
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_fg.swapChain;
    c.frameGenerationCallback = Dispatch;
    c.frameGenerationCallbackUserContext = &g_fg.context;
    c.frameGenerationEnabled = on;
    if (on && hudless)
        c.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    c.allowAsyncWorkloads = FsrFgAsync();
    c.generationRect.left = 0;
    c.generationRect.top = 0;
    c.generationRect.width = int32_t(g_fg.displayWidth);
    c.generationRect.height = int32_t(g_fg.displayHeight);
    static const bool tearLines = std::getenv("FM2_FRAMEGEN_DEBUG") != nullptr; // AMD's debug lines on made frames
    c.flags = tearLines ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES : 0;
    static const bool onlyMade = std::getenv("FM2_FRAMEGEN_ONLY_MADE") != nullptr; // testing: show only the made frames
    c.onlyPresentGenerated = onlyMade;
    c.frameID = g_fg.frameId;
    const ffxReturnCode_t r = ffxConfigure(&g_fg.context, &c.header);
    if (r != FFX_API_RETURN_OK)
    {
        static int logged = 0;
        if (logged++ < 3)
            Log("framegen: configure failed (%d)", int(r));
    }
    if (on != g_fg.on)
        Log("framegen: %s at frame id %llu", on ? "making frames" : "off (no scene this frame)", (unsigned long long)g_fg.frameId);
    g_fg.on = on;
}

// The version to ask for: FSR 3's own (not the ML one newer SDKs list on some cards), unless
// FM2_FRAMEGEN_VERSION=<text> names another. False: the SDK's choice.
bool PickVersion(ID3D12Device* device, uint64_t& id)
{
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    q.device = device;
    q.outputCount = &count;
    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0)
        return false;
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK)
        return false;
    for (uint64_t i = 0; i < count; ++i)
        Log("framegen: version %llu: %s", (unsigned long long)ids[i], names[i] ? names[i] : "?");
    const char* want = std::getenv("FM2_FRAMEGEN_VERSION");
    for (uint64_t i = 0; i < count; ++i)
    {
        const char* n = names[i] ? names[i] : "";
        const bool match = (want && *want) ? std::strstr(n, want) != nullptr : n[0] == '3';
        if (match)
        {
            id = ids[i];
            Log("framegen: using %s", n);
            return true;
        }
    }
    return false;
}

// (With g_fg.mutex held.)
bool EnsureContext(ID3D12Device* device, const FsrFgFrame& f)
{
    if (g_fg.context && g_fg.displayWidth == f.displayWidth && g_fg.displayHeight == f.displayHeight && g_fg.renderWidth == f.renderWidth &&
        g_fg.renderHeight == f.renderHeight && g_fg.depthInverted == f.depthInverted)
        return true;
    if (g_fg.context)
    {
        // Generation off first: that waits for the swap chain's work that still uses the context.
        Configure(false);
        ffxDestroyContext(&g_fg.context, nullptr);
        g_fg.context = nullptr;
    }
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxOverrideVersion version{};
    uint64_t versionId = 0;
    if (PickVersion(device, versionId))
    {
        version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        version.versionId = versionId;
        backend.header.pNext = &version.header;
    }
    ffxCreateContextDescFrameGenerationVersion api{};
    api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    api.version = FFX_FRAMEGENERATION_VERSION;
    api.header.pNext = &backend.header;
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &api.header;
    create.flags = (f.depthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) | (FsrFgAsync() ? FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT : 0);
    create.displaySize = { f.displayWidth, f.displayHeight };
    create.maxRenderSize = { f.renderWidth, f.renderHeight };
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    try
    {
        r = ffxCreateContext(&g_fg.context, &create.header, nullptr);
    }
    catch (...)
    {
        Log("framegen: ffxCreateContext threw");
    }
    if (r != FFX_API_RETURN_OK)
    {
        Log("framegen: context failed (%d): amd_fidelityfx_framegeneration_dx12.dll missing?", int(r));
        g_fg.context = nullptr;
        g_fg.failed = true;
        return false;
    }
    g_fg.displayWidth = f.displayWidth;
    g_fg.displayHeight = f.displayHeight;
    g_fg.renderWidth = f.renderWidth;
    g_fg.renderHeight = f.renderHeight;
    g_fg.depthInverted = f.depthInverted;
    g_fg.on = false;
    Log("framegen: context ready, scene %ux%u, shown at %ux%u, %s", f.renderWidth, f.renderHeight, f.displayWidth, f.displayHeight,
        FsrFgAsync() ? "on its own compute queue" : "on the game's queue");
    return true;
}
} // namespace

bool FsrFgWanted()
{
    // (Only the FSR 3 / FSR 4 upscaler draws the depth and motion it needs.)
    const Settings& s = GetSettings();
    return s.frameGen && (s.upscaler == Settings::Upscaler::Fsr3 || s.upscaler == Settings::Upscaler::Fsr4);
}

bool FsrFgAsync()
{
    static const bool async = [] {
        const char* v = std::getenv("FM2_FRAMEGEN_ASYNC");
        return v && *v == '1';
    }();
    return async;
}

uint64_t FsrFgMade()
{
    return g_fg.made;
}

bool FsrFgHasSwapChain()
{
    return g_fg.swapChain != nullptr;
}

bool FsrFgCreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc, IDXGISwapChain4** out)
{
    std::lock_guard lock(g_fg.mutex);
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 api{};
    api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    api.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.header.pNext = &api.header;
    create.swapchain = out;
    create.hwnd = hwnd;
    create.desc = desc;
    create.fullscreenDesc = nullptr;
    create.dxgiFactory = factory;
    create.gameQueue = queue;
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    try
    {
        r = ffxCreateContext(&g_fg.swapChainContext, &create.header, nullptr);
    }
    catch (...)
    {
        Log("framegen: creating the swap chain threw");
    }
    if (r != FFX_API_RETURN_OK || !*out)
    {
        Log("framegen: swap chain failed (%d): amd_fidelityfx_framegeneration_dx12.dll missing? Frame generation is off", int(r));
        g_fg.swapChainContext = nullptr;
        return false;
    }
    g_fg.swapChain = *out;
    Log("framegen: AMD frame generation swap chain made (%ux%u)", desc->Width, desc->Height);
    return true;
}

bool FsrFgPrepare(ID3D12Device* device, ID3D12GraphicsCommandList* list, const FsrFgFrame& f)
{
    std::lock_guard lock(g_fg.mutex);
    if (!g_fg.swapChain || g_fg.failed || g_fg.prepared)
        return false;
    if (!EnsureContext(device, f))
        return false;
    Configure(true, f.hudless);
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = g_fg.frameId;
    p.commandList = list;
    p.renderSize = { f.renderWidth, f.renderHeight };
    p.jitterOffset = { f.jitterX, f.jitterY };
    p.motionVectorScale = { f.motionScaleX, f.motionScaleY };
    p.frameTimeDelta = f.frameTimeMs;
    p.reset = f.reset;
    // The game's camera is unknown here (as for the upscaler): typical values, looking down +z.
    p.cameraNear = 0.1f;
    p.cameraFar = 10000.0f;
    p.cameraFovAngleVertical = 1.0f;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = ffxApiGetResourceDX12(f.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors = ffxApiGetResourceDX12(f.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.cameraUp[1] = 1.0f;
    p.cameraRight[0] = 1.0f;
    p.cameraForward[2] = 1.0f;
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    try
    {
        r = ffxDispatch(&g_fg.context, &p.header);
    }
    catch (...)
    {
        Log("framegen: prepare threw");
        g_fg.failed = true;
    }
    if (r != FFX_API_RETURN_OK)
    {
        static int logged = 0;
        if (logged++ < 3)
            Log("framegen: prepare failed (%d)", int(r));
        Configure(false);
        g_fg.prepared = true; // (this frame is configured: off)
        return false;
    }
    g_fg.prepared = true;
    g_fg.made++;
    return true;
}

void FsrFgBeforePresent()
{
    if (!g_fg.swapChain)
        return;
    std::lock_guard lock(g_fg.mutex);
    if (!g_fg.prepared)
        Configure(false);
    g_fg.prepared = false;
    g_fg.frameId++;
}
