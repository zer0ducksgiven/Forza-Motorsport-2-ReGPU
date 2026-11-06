// Options > Graphics: the launcher's graphics settings in game, among the game's own option categories. The screen is
// the HUD options scene with its rows bound to this file's data (kernel/zip_overlay.cpp makes that copy and adds the
// category to the Options flow; kBehavior below is its behaviour): the rows come from SQL written here, with the current
// values in a Lua table, as the flow's script loads (GraphicsMenuLuaSource). Going back reports the choices through a
// caught Lua error ("FM2GFX ...", seen by the luaD_throw hook in lua.cpp), and the renderer applies them at its next
// frame start. Vertex arena alone waits for the next start.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gpu/renderer.h"
#include "log.h"
#include "settings.h"

namespace
{
// The prefix's length is the HUD scene's profile path it replaces in the copied scene.
const char kValues[] = "FM2GraphicsOptionValuesOfTheGraphicsScreenInTheOptionsMenu___";
static_assert(sizeof(kValues) - 1 == sizeof("ProfileManager.activePlayerProfile.profileOptionsPropBag.Main") - 1);

// As the launcher lists them (Options in launcher/FM2Launcher/Install.cs).
const uint32_t kFps[] = { 30, 60, 120, 144, 165, 240, 360 };
const std::pair<uint32_t, const char*> kAspects[] = { { 0, "Auto" }, { 169, "16:9" }, { 219, "21:9 (experimental)" }, { 329, "32:9 (experimental)" } };
const std::pair<uint32_t, const char*> kMsaa[] = { { 1, "Off" }, { 2, "2x MSAA" }, { 4, "4x MSAA" }, { 8, "8x MSAA" } };
const std::pair<uint32_t, const char*> kSmaa[] = { { 0, "Off" }, { 1, "Low" }, { 2, "Medium" }, { 3, "High" }, { 4, "Ultra" } };
const std::pair<uint32_t, const char*> kAniso[] = { { 0, "Off (trilinear)" }, { 2, "2x" }, { 4, "4x" }, { 8, "8x" }, { 16, "16x" } };
const std::pair<uint32_t, const char*> kScale[] = { { 50, "50%" }, { 59, "59%" }, { 67, "67%" }, { 77, "77%" }, { 85, "85%" }, { 100, "100% (native)" } };
const std::pair<uint32_t, const char*> kSharp[] = { { 0, "Off" }, { 25, "Low" }, { 50, "Medium" }, { 75, "High" }, { 100, "Maximum" } };
const std::pair<uint32_t, const char*> kOnOff[] = { { 0, "Off" }, { 1, "On" } };
const std::pair<uint32_t, const char*> kMode[] = { { 0, "Windowed" }, { 1, "Fullscreen" } };

std::vector<std::pair<uint32_t, uint32_t>> g_resolutions; // as listed on the screen
uint32_t g_shownSharp = 0, g_shownScale = 100;

// The display's modes from 1280x720 up, smallest first (as the launcher lists them), and the current setting.
void ListResolutions()
{
    std::set<std::pair<uint32_t, uint32_t>> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720)
            modes.insert({ dm.dmPelsWidth, dm.dmPelsHeight });
    const Settings& s = GetSettings();
    modes.insert({ s.width, s.height });
    g_resolutions.assign(modes.begin(), modes.end());
    std::sort(g_resolutions.begin(), g_resolutions.end(), [](auto a, auto b) {
        const uint64_t pa = uint64_t(a.first) * a.second, pb = uint64_t(b.first) * b.second;
        return pa != pb ? pa < pb : a.first < b.first;
    });
}

// A row's values as SQL, in the single-quoted form a Lua long string and SQLite both take.
template <typename Values>
std::string ValuesSql(const Values& values)
{
    std::string sql;
    for (const auto& [value, label] : values)
        sql += std::string(sql.empty() ? "SELECT " : " UNION ALL SELECT ") + std::to_string(value) + " AS Value, ''" + label + "'' AS DisplayName";
    return sql;
}

std::string Lua()
{
    ListResolutions();
    const Settings& s = GetSettings();
    std::vector<std::pair<uint32_t, std::string>> resolutions, fps;
    uint32_t resNow = 0;
    for (size_t i = 0; i < g_resolutions.size(); i++)
    {
        const auto [w, h] = g_resolutions[i];
        resolutions.push_back({ uint32_t(i), std::to_string(w) + " x " + std::to_string(h) });
        if (w == s.width && h == s.height)
            resNow = uint32_t(i);
    }
    std::set<uint32_t> caps(std::begin(kFps), std::end(kFps));
    caps.insert(s.maxFps); // a custom cap the launcher set
    for (const uint32_t c : caps)
        fps.push_back({ c, std::to_string(c) + " FPS" });
    auto toStrings = [](const auto& values) {
        std::vector<std::pair<uint32_t, std::string>> out;
        for (const auto& [value, label] : values)
            out.push_back({ value, label });
        return out;
    };
    const uint32_t aspectNow = s.aspectW == 16 && s.aspectH == 9 ? 169 : s.aspectW == 21 && s.aspectH == 9 ? 219 : s.aspectW == 32 && s.aspectH == 9 ? 329 : 0;
    // The launcher's two settings behind the upscaler (GameSettings.Save), to the nearest it lists.
    g_shownScale = s.upscaler == Settings::Upscaler::Off ? 100 : s.renderScale;
    {
        uint32_t best = 100;
        for (const auto& [value, label] : kScale)
            if (std::abs(int(value) - int(g_shownScale)) < std::abs(int(best) - int(g_shownScale)))
                best = value;
        g_shownScale = best;
    }
    const uint32_t sharpPercent = s.upscaler == Settings::Upscaler::Off ? 0 : uint32_t(std::lround(s.sharpness * 100));
    g_shownSharp = std::min<uint32_t>((sharpPercent + 12) / 25 * 25, 100);

    struct Row
    {
        const char* token;
        const char* label;
        std::vector<std::pair<uint32_t, std::string>> values;
        uint32_t now;
    };
    const Row rows[] = {
        { "Resolution", "Resolution", resolutions, resNow },
        { "Aspect", "Aspect Ratio", toStrings(kAspects), aspectNow },
        { "DisplayMode", "Display Mode", toStrings(kMode), s.fullscreen ? 1u : 0u },
        { "Msaa", "MSAA", toStrings(kMsaa), s.msaa },
        { "Smaa", "SMAA", toStrings(kSmaa), s.smaa },
        { "Anisotropy", "Anisotropic Filtering", toStrings(kAniso), s.anisotropy },
        { "RenderScale", "Render Scale (FSR 1)", toStrings(kScale), g_shownScale },
        { "Sharpening", "Sharpening", toStrings(kSharp), g_shownSharp },
        { "MaxFps", "Max FPS", fps, s.maxFps },
        { "Vsync", "VSync", toStrings(kOnOff), s.vsync ? 1u : 0u },
        { "LowPerformance", "Low Performance Mode", toStrings(kOnOff), s.lowPerformance ? 1u : 0u },
        { "VertexArena", "Vertex Arena", toStrings(kOnOff), s.vertexArena ? 1u : 0u },
        { "HideShaderOverlay", "Hide Shader Compile Pop-up", toStrings(kOnOff), s.hideShaderOverlay ? 1u : 0u },
    };
    std::string table, values;
    int id = 1;
    for (const Row& r : rows)
    {
        table += std::string(table.empty() ? "SELECT " : " UNION ALL SELECT ") + std::to_string(id++) + " AS Id, '" + r.label + "' AS DisplayName, '" + r.token +
            "' AS Token, '" + ValuesSql(r.values) + "' AS ValuesSql";
        values += std::string(r.token) + " = " + std::to_string(r.now) + ", ";
    }
    return std::string(kValues) + " = { " + values + "}\nHUDOptionsTable = DB:execute([[" + table + "]])\n";
}
}

// The flow's script, with the marker in the Options flow's Graphics scene replaced; or empty when it isn't that script.
std::string GraphicsMenuLuaSource(const char* text, uint32_t size)
{
    static const char kMarker[] = "--FM2GRAPHICS";
    const std::string_view view(text, size);
    const size_t at = view.find(kMarker);
    if (at == std::string_view::npos)
        return {};
    std::string out(view);
    out.replace(at, sizeof(kMarker) - 1, Lua());
    return out;
}

// "FM2GFX <resolution index> <aspect> <mode> <msaa> <smaa> <anisotropy> <scale> <sharpening> <fps> <vsync> <low> <arena> <overlay>" from
// the screen's Back. True when it was this, so the caller doesn't log it as an error.
bool GraphicsMenuCommand(const char* text, size_t len)
{
    constexpr char kTag[] = "FM2GFX ";
    if (len < sizeof kTag - 1 || memcmp(text, kTag, sizeof kTag - 1) != 0)
        return false;
    double v[13] = {};
    const std::string args(text + sizeof kTag - 1, len - (sizeof kTag - 1));
    if (sscanf(args.c_str(), "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12]) != 13)
    {
        Log("ui: graphics options: bad report: %s", args.c_str());
        return true;
    }
    const size_t res = size_t(v[0]);
    const auto size = res < g_resolutions.size() ? g_resolutions[res] : std::make_pair(0u, 0u);
    const uint32_t aspect = uint32_t(v[1]), msaa = uint32_t(v[3]), smaa = uint32_t(v[4]), aniso = uint32_t(v[5]), scale = uint32_t(v[6]), sharp = uint32_t(v[7]), fps = uint32_t(v[8]);
    const bool full = v[2] != 0, vsync = v[9] != 0, low = v[10] != 0, arena = v[11] != 0, overlay = v[12] != 0;
    const uint32_t shownScale = g_shownScale, shownSharp = g_shownSharp;
    RendererChangeSettings([=](Settings& s) {
        if (size.first)
        {
            s.width = size.first;
            s.height = size.second;
        }
        s.aspectW = aspect ? aspect / 10 : 0;
        s.aspectH = aspect ? 9 : 0;
        s.fullscreen = full;
        s.msaa = msaa;
        s.smaa = smaa;
        s.anisotropy = aniso;
        // As the launcher's GameSettings.Save writes them; left alone when not changed here (a test's upscaler).
        if (scale != shownScale || sharp != shownSharp)
        {
            s.renderScale = scale;
            s.sharpness = float(sharp) / 100.0f;
            s.upscaler = scale < 100 || sharp > 0 ? Settings::Upscaler::Fsr1 : Settings::Upscaler::Off;
        }
        s.maxFps = fps;
        s.vsync = vsync;
        s.lowPerformance = low;
        s.vertexArena = arena;
        s.hideShaderOverlay = overlay;
    });
    Log("ui: graphics options: %ux%u, aspect %u, %s, MSAA %u, SMAA %u, filtering %u, scale %u, sharpening %u, cap %u, vsync %d, low %d, arena %d, overlay hidden %d",
        size.first, size.second, aspect, full ? "fullscreen" : "windowed", msaa, smaa, aniso, scale, sharp, fps, vsync, low, arena, overlay);
    return true;
}
