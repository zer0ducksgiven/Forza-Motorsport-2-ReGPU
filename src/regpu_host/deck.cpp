// The Steam Deck profile (after the FM3/FM4 ports' src/deck.cpp): what a Steam Deck under Proton gets, kept apart from
// every other PC. The general work (early fences, direct indices, the PGO build, the Wine stereo mix) applies everywhere
// and isn't here. Each setting is a default: what settings.ini or the environment sets still wins.
//
// settings.ini deck=auto|on|off (default auto); FM2_DECK=0|1 overrides it.
//   auto: on under Wine/Proton on a Steam Deck APU (CPU "AMD Custom APU 0405": LCD, 60 Hz; "0932": OLED, 90 Hz).
//
// What it sets (in memory; settings.ini keeps its own values):
//   frame cap    the display's refresh rate (the panel's, or an external screen's) when settings.ini asks for more and
//                FM2_MAX_FPS isn't set. Frames past it are never seen, and the APU's 15 W are shared: CPU and GPU work
//                for unseen frames takes clock from the seen ones. (FM2 keeps game time by the clock: any cap is safe.)
//   resolution   1280x800, the panel (ReGPU draws the 16:9 game in it with bars), when settings.ini sets none.
//   MSAA         2x when settings.ini sets none (the benchmark race holds ~95-100 fps uncapped at 720p 2x).
//   CPU          the command processor sleeps while idle under the cap (regpu_host/gpu_host.cpp, for every capped PC).
// FM2_DECK_PRESETS=0: the profile without the presets (resolution, MSAA).
#include <windows.h>

#include <intrin.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "log.h"
#include "settings.h"

namespace
{
bool g_deck = false;

std::string CpuBrand()
{
    int regs[4]{};
    char brand[49]{};
    __cpuid(regs, int(0x80000000));
    if (unsigned(regs[0]) < 0x80000004u)
        return {};
    for (int i = 0; i < 3; i++)
    {
        __cpuid(regs, int(0x80000002 + i));
        memcpy(brand + i * 16, regs, 16);
    }
    std::string s(brand);
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

bool OnWine()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}
} // namespace

bool DeckProfile()
{
    return g_deck;
}

void DeckProfileInit()
{
    Settings& st = GetSettings();
    const std::string cpu = CpuBrand();
    const bool wine = OnWine();
    const bool lcd = cpu.find("AMD Custom APU 0405") != std::string::npos;
    const bool oled = cpu.find("AMD Custom APU 0932") != std::string::npos;
    std::string mode = st.deck;
    if (const char* v = getenv("FM2_DECK"); v && *v)
        mode = strcmp(v, "0") == 0 ? "off" : "on";
    if (mode != "on" && mode != "off")
        mode = "auto";
    g_deck = mode == "on" || (mode == "auto" && wine && (lcd || oled));
    Log("deck: Steam Deck profile %s (deck=%s; CPU \"%s\", %s)", g_deck ? "on" : "off", mode.c_str(), cpu.c_str(),
        wine ? "under Wine/Proton" : "not under Wine");
    if (!g_deck)
        return;
    // The display's refresh rate (the built-in panel's or an external screen's); else the model's.
    uint32_t hz = oled || !lcd ? 90 : 60;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 240)
        hz = dm.dmDisplayFrequency;
    if (st.maxFps > hz && !getenv("FM2_MAX_FPS"))
    {
        Log("deck: frame cap %u -> %u (the display's refresh rate; settings.ini unchanged)", st.maxFps, hz);
        st.maxFps = hz;
    }
    const char* presets = getenv("FM2_DECK_PRESETS");
    if (presets && strcmp(presets, "0") == 0)
        return;
    if (!st.fromFile.count("resolution") && !getenv("FM2_RESOLUTION") && !getenv("FM2_RENDER_SCALE"))
    {
        Log("deck: preset resolution %ux%u -> 1280x800 (the panel; not set in settings.ini)", st.width, st.height);
        st.width = 1280, st.height = 800;
    }
    if (!st.fromFile.count("max_fps") && !getenv("FM2_MAX_FPS") && st.maxFps != hz)
    {
        Log("deck: preset frame cap %u -> %u (the display's refresh rate; not set in settings.ini)", st.maxFps, hz);
        st.maxFps = hz;
    }
    if (!st.fromFile.count("msaa") && !getenv("FM2_MSAA") && st.msaa < 2)
    {
        Log("deck: preset MSAA %u -> 2x (not set in settings.ini)", st.msaa);
        st.msaa = 2;
    }
}
