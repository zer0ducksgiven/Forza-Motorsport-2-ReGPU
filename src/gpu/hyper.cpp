// Hyper presets and cuts (hyper.h).
#include "gpu/hyper.h"

#include "log.h"

#include <algorithm>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
HyperSettings g_settings; // the port's settings (HyperConfigure)

const char* Env(const char* regpu, const char* fm2)
{
    if (const char* v = getenv(regpu))
        return v;
    return getenv(fm2);
}

// The environment, else the port's setting (when set: >= 0), else the preset's default.
int Value(const char* regpu, const char* fm2, int setting, int fallback)
{
    const char* v = Env(regpu, fm2);
    if (v && *v)
        return atoi(v);
    return setting >= 0 ? setting : fallback;
}
}

// Phase 7 (Intel thread): the GPU class, read once from the first hardware adapter. Generic: the GPU, never the game.
struct GpuClass
{
    uint32_t vendor = 0;     // PCI vendor: 0x8086 Intel, 0x1002 AMD, 0x10DE NVIDIA
    bool integrated = false; // 512 MB or less of dedicated memory: shares system memory
};

GpuClass DetectGpu()
{
    GpuClass g;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return g;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        g.vendor = d.VendorId;
        g.integrated = d.DedicatedVideoMemory <= 512ull * 1024 * 1024;
        break;
    }
    return g;
}

void Default(const char* name, const char* value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

void HyperConfigure(const HyperSettings& settings)
{
    g_settings = settings;
    // hyper=auto (or REGPU_HYPER=auto): an integrated GPU gets Potato, anything else stays off.
    const char* env = Env("REGPU_HYPER", "FM2_HYPER");
    const bool autoPick = env ? !_stricmp(env, "auto") : !_stricmp(g_settings.preset.c_str(), "auto");
    const GpuClass gpu = DetectGpu();
    if (autoPick)
    {
        const char* pick = gpu.integrated ? "potato" : "off";
        _putenv_s("REGPU_HYPER", pick);
        Log("hyper: auto picked %s (GPU vendor %04X, %s)", pick, gpu.vendor, gpu.integrated ? "integrated" : "dedicated");
    }
    // Potato on Intel: the cuts measured on the Intel thread (HD 630, INTEL-PATH.md); every value only a default, the
    // environment and the hyper_* settings keys win. Also on AMD since 15 Nov 2026: on the Vega laptop this set measured
    // 77.3 fps against 71.5 for the laptop's own Potato (docs/hyper/INTEL-VS-AMD.md). REGPU_HYPER_POTATO_SET=laptop keeps
    // the older laptop set (hyper.cpp getters), which is also what other vendors get.
    const char* potatoSet = getenv("REGPU_HYPER_POTATO_SET");
    const bool laptopSet = potatoSet && !_stricmp(potatoSet, "laptop");
    if (HyperLevel() == HyperPreset::Potato && (gpu.vendor == 0x8086 || gpu.vendor == 0x1002) && !laptopSet)
    {
        static const char* const intel[][2] = {
            { "REGPU_HYPER_SHADER_OPT", "1" }, { "REGPU_HYPER_VCACHE", "2" }, { "REGPU_HYPER_PLAIN_MUL", "5" },
            { "REGPU_HYPER_FETCHSPEC", "1" }, { "REGPU_HYPER_SWIZZLE_SEL", "1" }, { "REGPU_HYPER_LEAN_LOD", "1" },
            { "REGPU_HYPER_CULL_PASSES", "1" }, { "REGPU_HYPER_FREEZE", "120" }, { "REGPU_HYPER_FREEZE_REFRESH", "4" },
            { "REGPU_HYPER_TAILSPEC", "1" }, { "REGPU_HYPER_CULL_PIXELS", "256" }, { "REGPU_HYPER_CULL_PROBE", "1" }, { "REGPU_HYPER_CULL_HYST", "200" }, { "REGPU_HYPER_RESOLVE_BY_SIZE", "1" },
            { "REGPU_HYPER_LEAN_FETCH", "0" }, { "REGPU_HYPER_LEAN_VFETCH", "0" }, // the laptop's fetch path: superseded here
        };
        for (const auto& [name, value] : intel)
            Default(name, value);
        Log("hyper: potato, shader-cut set (fetch specialisation, smart multiply, swizzle selects, lean LOD, culling, frozen helpers)");
    }
}

HyperPreset HyperLevel()
{
    static const HyperPreset level = [] {
        const char* v = Env("REGPU_HYPER", "FM2_HYPER");
        if (!v || !*v)
            v = g_settings.preset.c_str();
        if (!*v)
            return HyperPreset::Off;
        if (!_stricmp(v, "mild") || !strcmp(v, "1"))
            return HyperPreset::Mild;
        if (!_stricmp(v, "low") || !strcmp(v, "2"))
            return HyperPreset::Low;
        if (!_stricmp(v, "extreme") || !strcmp(v, "3"))
            return HyperPreset::Extreme;
        if (!_stricmp(v, "potato") || !strcmp(v, "4"))
            return HyperPreset::Potato;
        return HyperPreset::Off;
    }();
    return level;
}

const char* HyperName(HyperPreset preset)
{
    switch (preset)
    {
    case HyperPreset::Mild: return "mild";
    case HyperPreset::Low: return "low";
    case HyperPreset::Extreme: return "extreme";
    case HyperPreset::Potato: return "potato";
    default: return "off";
    }
}

uint32_t HyperSceneScalePercent()
{
    static const uint32_t percent = [] {
        static const int byPreset[] = { 0, 50, 35, 25, 0 };
        const int v = Value("REGPU_HYPER_SCENE", "FM2_HYPER_SCENE", g_settings.scenePercent, byPreset[int(HyperLevel())]);
        return v <= 0 ? 0u : uint32_t(std::clamp(v, 25, 100));
    }();
    return percent;
}

bool HyperCheapUpscale()
{
    static const bool on = [] {
        const bool byPreset = HyperLevel() == HyperPreset::Low || HyperLevel() == HyperPreset::Extreme;
        return Value("REGPU_HYPER_UPSCALE", "FM2_HYPER_UPSCALE", g_settings.cheapUpscale, byPreset ? 1 : 0) != 0;
    }();
    return on;
}

uint32_t HyperDecimatePercent()
{
    static const uint32_t percent = [] {
        const int v = Value("REGPU_HYPER_DECIMATE", "FM2_HYPER_DECIMATE", g_settings.decimate, HyperLevel() == HyperPreset::Extreme ? 25 : HyperLevel() == HyperPreset::Potato ? 10 : 0);
        return v <= 0 || v >= 100 ? 0u : uint32_t(std::clamp(v, 10, 90));
    }();
    return percent;
}

uint32_t HyperAnisotropyCap()
{
    static const uint32_t cap = [] {
        static const int byPreset[] = { 16, 4, 2, 2, 0 };
        const int v = Value("REGPU_HYPER_ANISO", "FM2_HYPER_ANISO", g_settings.aniso, byPreset[int(HyperLevel())]);
        return v >= 16 ? 16u : v >= 8 ? 8u : v >= 4 ? 4u : v >= 2 ? 2u : 0u;
    }();
    return cap;
}

uint32_t HyperHelperScalePercent()
{
    static const uint32_t percent = [] {
        static const int byPreset[] = { 100, 75, 50, 50, 25 };
        const int v = Value("REGPU_HYPER_HELPERS", "FM2_HYPER_HELPERS", g_settings.helpers, byPreset[int(HyperLevel())]);
        return uint32_t(std::clamp(v <= 0 ? 100 : v, 25, 100));
    }();
    return percent;
}

bool HyperResolve1010()
{
    static const bool on = Value("REGPU_HYPER_RESOLVE1010", "FM2_HYPER_RESOLVE1010", g_settings.resolve1010,
                               HyperLevel() != HyperPreset::Off ? 1 : 0) != 0;
    return on;
}

bool HyperNoDiscard()
{
    static const bool on = Value("REGPU_HYPER_NODISCARD", "FM2_HYPER_NODISCARD", g_settings.noDiscard,
                               HyperLevel() != HyperPreset::Off ? 1 : 0) != 0;
    return on;
}

int HyperLeanFetch()
{
    static const int v = std::clamp(Value("REGPU_HYPER_LEAN_FETCH", "FM2_HYPER_LEAN_FETCH", -1, HyperLevel() == HyperPreset::Potato ? 3 : 0), 0, 3);
    return v;
}

bool HyperLeanVFetch()
{
    static const bool on = Value("REGPU_HYPER_LEAN_VFETCH", "FM2_HYPER_LEAN_VFETCH", -1, HyperLevel() == HyperPreset::Potato ? 1 : 0) != 0;
    return on;
}

int HyperCullPixels()
{
    static const int v = [] {
        const char* e = Env("REGPU_HYPER_CULL_PIXELS", "FM2_HYPER_CULL_PIXELS");
        if (e && *e)
            return atoi(e);
        return HyperLevel() == HyperPreset::Potato ? 64 : -1;
    }();
    return v;
}

bool HyperCullAll()
{
    static const bool on = Value("REGPU_HYPER_CULL_ALL", "FM2_HYPER_CULL_ALL", -1, HyperLevel() == HyperPreset::Potato ? 1 : 0) != 0;
    return on;
}

void HyperLogSummary()
{
    const uint32_t scene = HyperSceneScalePercent();
    // Nothing to say when nothing is on (most players): the cuts set one by one are logged below.
    if (HyperLevel() != HyperPreset::Off || scene || HyperCheapUpscale())
    Log("hyper: preset %s; scene scale %s; upscale %s", HyperName(HyperLevel()), scene ? (std::to_string(scene) + "%").c_str() : "as set",
        HyperCheapUpscale() ? "bilinear + gamma, one pass" : "FSR 1");
    if (HyperNoDiscard())
        Log("hyper: pixel shaders without the alpha test drop their discard");
    if (HyperLeanFetch())
        Log("hyper: lean texture fetch %d", HyperLeanFetch());
    if (HyperLeanVFetch())
        Log("hyper: lean vertex fetch");
    if (HyperCullPixels() >= 0)
        Log("hyper: draws of %d pixels or fewer culled (%s)", HyperCullPixels(), HyperCullAll() ? "every depth-tested draw" : "opaque only");
    if (HyperResolve1010())
        Log("hyper: 10:10:10:2 resolves kept at 32 bits");
    if (HyperHelperScalePercent() < 100)
        Log("hyper: offscreen helper surfaces at %u%% size", HyperHelperScalePercent());
    if (HyperAnisotropyCap() < 16)
        Log("hyper: anisotropic filtering at most %ux", HyperAnisotropyCap());
    if (HyperDecimatePercent())
        Log("hyper: big meshes keep %u%% of their triangles", HyperDecimatePercent());
}
