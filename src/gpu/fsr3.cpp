// AMD FSR 3 and FSR 4 upscaling (FidelityFX API, MIT): the same inputs as DLSS (see dlss.h). Needs
// amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll next to the
// executable (copied there by the build). FSR 3 runs on any D3D12 GPU. FSR 4 is the same upscaler
// DLL's ML version: the FidelityFX API lists the versions this GPU can run and one is picked by name
// (FM2_FSR_VERSION=<text> picks the first whose name contains the text, for testing).
// Context lifetime, sticky HDR, the exception guards and the FSR 4 version pick are from Aiden's FM4 port.
#include "gpu/fsr3.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "log.h"

namespace
{
struct Fsr3
{
    bool failed = false;
    ffxContext context = nullptr;
    DlssSetup setup{};
    // A replaced context is destroyed kRetireFrames evaluations later, once the frames in flight that
    // used it are done (destroying it at once could free resources the GPU was still using).
    struct Retired
    {
        ffxContext context;
        int left;
    };
    std::vector<Retired> retired;
    bool hdrSeen = false; // sticky: the scene format flips in menus, which would rebuild the context
    bool ml = false;      // the context is an FSR 4 (ML) one
    bool versionsLogged = false;
} g_fsr3;
constexpr int kRetireFrames = 8;

bool Contains(const char* name, const char* text)
{
    return name && std::strstr(name, text) != nullptr;
}

// A name from the list of upscaler versions the API offers: "FSR 4" ones are the ML models. The names'
// wording is the SDK's, so FM2_FSR_VERSION can override the match.
bool IsMlVersion(const char* name)
{
    // (A bare version such as "4.1.2" starts with 4; "3.1.4" must not match.)
    return Contains(name, "FSR 4") || Contains(name, "FSR4") || (name && name[0] == '4' && name[1] == '.');
}

// FSR 4 is the ML upscaler. The FidelityFX API lists it on RDNA 2/3 too (a slower fallback path in newer
// SDKs), where in the FM4 port it worked but hung the device and crashed inside amdxc64.dll in some long
// runs. So it is opt-in: the "fsr4" upscaler setting runs FSR 3 unless FM2_FSR4=1 is set. The adapter is
// logged either way.
bool Fsr4Enabled(ID3D12Device* device)
{
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    const char* v = std::getenv("FM2_FSR4");
    const bool on = v && *v == '1';
    const char* state = on ? "enabled (FM2_FSR4=1)" : "off by default, using FSR 3 (FM2_FSR4=1 to try it)";
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        Log("fsr: adapter %04x:%04x, FSR 4 %s", d.VendorId, d.DeviceId, state);
    }
    else
        Log("fsr: FSR 4 %s", state);
    cached = on ? 1 : 0;
    return on;
}

// The version id to ask for, or false to leave the choice to the SDK (which takes the newest one the
// GPU runs, so FSR 4 where supported and FSR 3.1 elsewhere). `found`: a version of the wanted kind was listed.
bool PickVersion(ID3D12Device* device, bool ml, uint64_t& id, bool& found)
{
    found = false;
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device;
    q.outputCount = &count;
    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0)
        return false;
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.outputCount = &count;
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK)
        return false;
    if (!g_fsr3.versionsLogged)
    {
        g_fsr3.versionsLogged = true;
        for (uint64_t i = 0; i < count; ++i)
            Log("fsr: upscaler version %llu: %s", (unsigned long long)ids[i], names[i] ? names[i] : "?");
    }
    const char* want = std::getenv("FM2_FSR_VERSION");
    for (uint64_t i = 0; i < count; ++i)
    {
        const bool match = (want && *want) ? Contains(names[i], want) : IsMlVersion(names[i]) == ml;
        if (match)
        {
            id = ids[i];
            found = true;
            Log("fsr: using %s", names[i]);
            return true;
        }
    }
    if (!ml && !(want && *want))
        Log("fsr: no non-ML upscaler version listed; the SDK default may be FSR 4");
    return false;
}

bool FsrEvaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness, bool ml);
}

bool Fsr3Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness)
{
    return FsrEvaluate(device, list, setup, frame, sharpness, false);
}

bool Fsr4Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness)
{
    return FsrEvaluate(device, list, setup, frame, sharpness, Fsr4Enabled(device));
}

namespace
{
bool FsrEvaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness, bool ml)
{
    if (g_fsr3.failed)
        return false;
    for (size_t i = 0; i < g_fsr3.retired.size();)
    {
        if (--g_fsr3.retired[i].left <= 0)
        {
            ffxDestroyContext(&g_fsr3.retired[i].context, nullptr);
            g_fsr3.retired.erase(g_fsr3.retired.begin() + ptrdiff_t(i));
        }
        else
            ++i;
    }
    g_fsr3.hdrSeen = g_fsr3.hdrSeen || setup.hdr;
    DlssSetup want = setup;
    want.hdr = g_fsr3.hdrSeen;
    if (!g_fsr3.context || !(want == g_fsr3.setup) || ml != g_fsr3.ml)
    {
        const char* why = !g_fsr3.context ? "first context" : ml != g_fsr3.ml ? "FSR 3/4 switched"
            : (want.renderWidth != g_fsr3.setup.renderWidth || want.renderHeight != g_fsr3.setup.renderHeight) ? "render size changed"
            : (want.outputWidth != g_fsr3.setup.outputWidth || want.outputHeight != g_fsr3.setup.outputHeight) ? "output size changed"
            : want.hdr != g_fsr3.setup.hdr ? "scene became HDR" : "quality or depth flags changed";
        Log("%s: new context (%s): %ux%u -> %ux%u", ml ? "fsr4" : "fsr3", why, want.renderWidth, want.renderHeight, want.outputWidth, want.outputHeight);
        if (g_fsr3.context)
        {
            g_fsr3.retired.push_back({ g_fsr3.context, kRetireFrames });
            g_fsr3.context = nullptr;
        }
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        // The version is pinned by name: FSR 4 only when asked for, and FSR 3 otherwise even where the SDK's
        // own choice would be FSR 4. FSR 4 asked for but not listed for this GPU: FSR 3.
        ffxOverrideVersion version{};
        uint64_t versionId = 0;
        bool found = false;
        bool pinned = PickVersion(device, ml, versionId, found);
        const bool runsMl = ml && found;
        if (ml && !found)
        {
            static bool said = false;
            if (!said)
                Log("fsr4: this GPU and driver list no FSR 4 version; using FSR 3");
            said = true;
            pinned = PickVersion(device, false, versionId, found);
        }
        if (pinned)
        {
            version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            version.versionId = versionId;
            backend.header.pNext = &version.header;
        }
        ffxCreateContextDescUpscale create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext = &backend.header;
        create.maxRenderSize = { want.renderWidth, want.renderHeight };
        create.maxUpscaleSize = { want.outputWidth, want.outputHeight };
        create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | (want.depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
            (want.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0);
        ffxReturnCode_t r = FFX_API_RETURN_ERROR;
        try
        {
            r = ffxCreateContext(&g_fsr3.context, &create.header, nullptr);
        }
        catch (...)
        {
            // (A C++ exception out of the FidelityFX DLL: seen in FM4 when the device was lost.)
            Log("fsr3: ffxCreateContext threw; the device is probably lost");
        }
        if (r != FFX_API_RETURN_OK)
        {
            Log("fsr3: create failed (%d): amd_fidelityfx_*_dx12.dll missing?", int(r));
            g_fsr3.context = nullptr;
            g_fsr3.failed = true;
            return false;
        }
        g_fsr3.setup = want;
        g_fsr3.ml = ml;
        Log("%s: context ready%s%s", ml ? "fsr4" : "fsr3", ml && !runsMl ? " (running FSR 3)" : "", want.hdr ? ", HDR" : "");
    }
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list;
    d.color = ffxApiGetResourceDX12(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(frame.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (frame.reactive)
        d.reactive = ffxApiGetResourceDX12(frame.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = { frame.jitterX, frame.jitterY };
    d.motionVectorScale = { frame.motionScaleX, frame.motionScaleY };
    d.renderSize = { setup.renderWidth, setup.renderHeight };
    d.upscaleSize = { setup.outputWidth, setup.outputHeight };
    d.enableSharpening = sharpness > 0.0f;
    d.sharpness = sharpness;
    d.frameTimeDelta = frame.frameTimeMs;
    d.preExposure = 1.0f;
    d.reset = frame.reset;
    // The game's camera is unknown here; typical values (used for depth-based heuristics).
    d.cameraNear = 0.1f;
    d.cameraFar = 10000.0f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    try
    {
        r = ffxDispatch(&g_fsr3.context, &d.header);
    }
    catch (...)
    {
        Log("fsr3: ffxDispatch threw; the device is probably lost");
        g_fsr3.failed = true;
    }
    if (r != FFX_API_RETURN_OK)
    {
        static int logged = 0;
        if (logged++ < 3)
            Log("fsr3: dispatch failed (%d)", int(r));
        return false;
    }
    return true;
}
} // namespace
