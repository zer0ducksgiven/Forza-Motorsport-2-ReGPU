// The player's system in out\play.log at boot: OS (with its update), Wine/Proton, CPU, memory,
// every GPU with its driver version, the displays, and what the D3D12 driver supports. Bug reports
// arrive as a log; this makes them say what they ran on.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include "log.h"
#include "settings.h"
#include "version.h"

using Microsoft::WRL::ComPtr;

namespace
{
std::string Narrow(const wchar_t* w)
{
    char s[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, sizeof(s), nullptr, nullptr);
    return s;
}

std::string RegString(HKEY root, const char* key, const char* value)
{
    char s[256];
    DWORD size = sizeof(s);
    if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, s, &size) != ERROR_SUCCESS)
        return "";
    std::string r = s;
    while (!r.empty() && r.back() == ' ')
        r.pop_back();
    return r;
}

DWORD RegDword(HKEY root, const char* key, const char* value)
{
    DWORD v = 0, size = sizeof(v);
    return RegGetValueA(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : 0;
}

void LogOs()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOW v{ sizeof(v) };
    if (auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr)
        get(&v);
    const char* nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = RegString(HKEY_LOCAL_MACHINE, nt, "ProductName");
    // Windows 11 still says "Windows 10" in ProductName; the build tells them apart.
    if (v.dwBuildNumber >= 22000 && product.find("Windows 10") == 0)
        product.replace(8, 2, "11");
    const std::string display = RegString(HKEY_LOCAL_MACHINE, nt, "DisplayVersion");
    const std::string edition = RegString(HKEY_LOCAL_MACHINE, nt, "EditionID");
    Log("system: OS %s %s (build %lu.%lu.%lu.%lu%s%s)", product.c_str(), display.c_str(), v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
        RegDword(HKEY_LOCAL_MACHINE, nt, "UBR"), edition.empty() ? "" : ", ", edition.c_str());
    // Wine / Proton (Linux, Steam Deck): ntdll exports its version.
    using WineVersionFn = const char*(CDECL*)();
    using WineHostFn = void(CDECL*)(const char**, const char**);
    if (auto wine = ntdll ? reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr)
    {
        const char *sys = "?", *release = "?";
        if (auto host = reinterpret_cast<WineHostFn>(GetProcAddress(ntdll, "wine_get_host_version")))
            host(&sys, &release);
        const char* proton = getenv("PROTON_VERSION") ? getenv("PROTON_VERSION") : getenv("SteamAppId") ? "(Steam)" : "";
        Log("system: Wine %s on %s %s %s", wine(), sys, release, proton);
    }
}

void LogCpuAndMemory()
{
    const char* key = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    uint32_t cores = 0;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<uint8_t> buffer(bytes);
    if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes))
        for (DWORD at = 0; at < bytes; at += reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at)->Size)
            cores++;
    Log("system: CPU %s, %u cores, %u threads, %lu MHz", RegString(HKEY_LOCAL_MACHINE, key, "ProcessorNameString").c_str(), cores,
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), RegDword(HKEY_LOCAL_MACHINE, key, "~MHz"));
    MEMORYSTATUSEX m{ sizeof(m) };
    GlobalMemoryStatusEx(&m);
    Log("system: RAM %llu MB, %llu MB free; page file %llu MB", m.ullTotalPhys >> 20, m.ullAvailPhys >> 20, m.ullTotalPageFile >> 20);
}

// Driver version as the vendor's control panel shows it.
std::string DriverVersion(IDXGIAdapter1* adapter, UINT vendor)
{
    LARGE_INTEGER umd{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        return "?";
    const unsigned a = HIWORD(umd.HighPart), b = LOWORD(umd.HighPart), c = HIWORD(umd.LowPart), d = LOWORD(umd.LowPart);
    char s[64];
    if (vendor == 0x10DE) // NVIDIA: 32.0.15.6094 is 560.94
    {
        const unsigned n = (c % 10) * 10000 + d;
        snprintf(s, sizeof(s), "%u.%u.%u.%u (NVIDIA %u.%02u)", a, b, c, d, n / 100, n % 100);
    }
    else
        snprintf(s, sizeof(s), "%u.%u.%u.%u", a, b, c, d);
    return s;
}

const char* Vendor(UINT id)
{
    return id == 0x10DE ? "NVIDIA" : id == 0x1002 ? "AMD" : id == 0x8086 ? "Intel" : id == 0x1414 ? "Microsoft" : "other";
}

void LogGpus(ID3D12Device* device)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;
    const LUID used = device ? device->GetAdapterLuid() : LUID{};
    ComPtr<IDXGIAdapter1> adapter;
    std::vector<std::wstring> seen; // the same GPU can be listed twice (display routing): once each
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        const bool inUse = d.AdapterLuid.LowPart == used.LowPart && d.AdapterLuid.HighPart == used.HighPart;
        const std::wstring id = std::wstring(d.Description) + L"/" + std::to_wstring(d.DeviceId) + L"/" + std::to_wstring(d.DedicatedVideoMemory);
        if (!inUse && std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        seen.push_back(id);
        Log("system: GPU %s%s: %s %04X:%04X rev %02X, %llu MB VRAM, %llu MB shared, driver %s", inUse ? "(used) " : "", Narrow(d.Description).c_str(),
            Vendor(d.VendorId), d.VendorId, d.DeviceId, d.Revision, (unsigned long long)(d.DedicatedVideoMemory >> 20),
            (unsigned long long)(d.SharedSystemMemory >> 20), DriverVersion(adapter.Get(), d.VendorId).c_str());
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++)
        {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
            UINT hdr = 0;
            ComPtr<IDXGIOutput6> o6;
            DXGI_OUTPUT_DESC1 od1{};
            if (SUCCEEDED(output.As(&o6)) && SUCCEEDED(o6->GetDesc1(&od1)))
                hdr = od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            Log("system:   display %u: %lux%lu at %lu Hz%s", o, mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, hdr ? ", HDR on" : "");
        }
    }
}

void LogD3D12(ID3D12Device* device)
{
    if (!device)
        return;
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{ UINT(std::size(levels)), levels };
    device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_7 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        sm.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    ComPtr<ID3D12Device13> d13;
    const bool device13 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d13)));
    Log("system: D3D12 feature level %u.%u, shader model %u.%u, resource binding tier %d, tiled resources tier %d, ID3D12Device13 %s",
        unsigned(fl.MaxSupportedFeatureLevel >> 12), unsigned((fl.MaxSupportedFeatureLevel >> 8) & 15), unsigned(sm.HighestShaderModel >> 4), unsigned(sm.HighestShaderModel & 15),
        int(o.ResourceBindingTier), int(o.TiledResourcesTier), device13 ? "yes" : "no (the ID3D12Device3 fallback is tried: see the renderer's guest memory line)");
}
}

void LogSystemInfo(ID3D12Device* device)
{
    Log("system: Forza Motorsport 2 ReGPU %s", FM2_VERSION);
    LogOs();
    LogCpuAndMemory();
    LogGpus(device);
    LogD3D12(device);
    const Settings& st = GetSettings();
    static const char* upscalers[] = { "off", "FSR 1", "FSR 3", "DLSS" };
    Log("system: settings %ux%u %s, vsync %s, max %u fps, MSAA %ux, anisotropic %ux, upscaler %s%s%s", st.width, st.height, st.fullscreen ? "fullscreen" : "windowed",
        st.vsync ? "on" : "off", st.maxFps, st.msaa, st.anisotropy, upscalers[std::min<uint32_t>(uint32_t(st.upscaler), 3)], st.enhance ? ", enhanced lighting" : "", st.lowPerformance ? ", low performance mode" : "");
}
