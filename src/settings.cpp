// Player settings (see settings.h): a small key=value file.
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "kernel/runtime.h"
#include "log.h"

namespace
{
Settings g_settings;
bool g_loaded = false;

std::filesystem::path Path() { return UserDir() / "settings.ini"; }

Settings::Upscaler ParseUpscaler(const std::string& v)
{
    if (v == "fsr1")
        return Settings::Upscaler::Fsr1;
    // DLSS and FSR 3 aren't finished (0.8.0): off unless FM2_TEMPORAL_UPSCALERS=1, for testing.
    static const bool temporal = getenv("FM2_TEMPORAL_UPSCALERS") != nullptr;
    if (v == "fsr3" && temporal)
        return Settings::Upscaler::Fsr3;
    if (v == "dlss" && temporal)
        return Settings::Upscaler::Dlss;
    return Settings::Upscaler::Off;
}

const char* UpscalerName(Settings::Upscaler u)
{
    switch (u)
    {
    case Settings::Upscaler::Fsr1: return "fsr1";
    case Settings::Upscaler::Fsr3: return "fsr3";
    case Settings::Upscaler::Dlss: return "dlss";
    default: return "off";
    }
}

void Load()
{
    std::ifstream in(Path());
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#' || line[0] == ';')
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "profile" && !value.empty())
            g_settings.profile = value.substr(0, 15);
        else if (key == "resolution")
            sscanf(value.c_str(), "%ux%u", &g_settings.width, &g_settings.height);
        else if (key == "aspect")
        {
            if (sscanf(value.c_str(), "%u:%u", &g_settings.aspectW, &g_settings.aspectH) != 2)
                g_settings.aspectW = g_settings.aspectH = 0;
        }
        else if (key == "fullscreen")
            g_settings.fullscreen = value == "1";
        else if (key == "vsync")
            g_settings.vsync = value == "1";
        else if (key == "max_fps")
            g_settings.maxFps = uint32_t(atoi(value.c_str()));
        else if (key == "enhance")
            g_settings.enhance = value == "1";
        else if (key == "language")
            g_settings.language = uint32_t(atoi(value.c_str()));
        else if (key == "skip_intro")
            g_settings.skipIntro = value == "1";
        else if (key == "xbox_prompts")
            g_settings.xboxPrompts = value == "1";
        else if (key == "hide_shader_overlay")
            g_settings.hideShaderOverlay = value == "1";
        else if (key == "low_performance")
            g_settings.lowPerformance = value == "1";
        else if (key == "vertex_arena")
            g_settings.vertexArena = value != "0";
        else if (key == "wheel_strength")
            g_settings.wheelStrength = uint32_t(atoi(value.c_str()));
        else if (key == "wheel_spring")
            g_settings.wheelSpring = uint32_t(atoi(value.c_str()));
        else if (key == "wheel_reverse")
            g_settings.wheelReverse = value == "1";
        else if (key == "wheel_rotation")
            g_settings.wheelRotation = uint32_t(atoi(value.c_str()));
        else if (key == "msaa")
            g_settings.msaa = uint32_t(atoi(value.c_str()));
        else if (key == "smaa")
            g_settings.smaa = uint32_t(atoi(value.c_str()));
        else if (key == "anisotropy")
            g_settings.anisotropy = uint32_t(atoi(value.c_str()));
        else if (key == "upscaler")
            g_settings.upscaler = ParseUpscaler(value);
        else if (key == "render_scale")
            g_settings.renderScale = uint32_t(atoi(value.c_str()));
        else if (key == "upscale_quality")
            g_settings.quality = uint32_t(atoi(value.c_str()));
        else if (key == "sharpness")
            g_settings.sharpness = float(atof(value.c_str()));
    }
    // Overrides for testing.
    if (const char* v = getenv("FM2_PROFILE"))
        g_settings.profile = std::string(v).substr(0, 15);
    if (const char* v = getenv("FM2_RESOLUTION"))
        sscanf(v, "%ux%u", &g_settings.width, &g_settings.height);
    else if (const char* k = getenv("FM2_RENDER_SCALE"))
    {
        const uint32_t s = std::clamp<uint32_t>(uint32_t(atoi(k)), 1, 4);
        g_settings.width = 1280 * s;
        g_settings.height = 720 * s;
    }
    if (const char* v = getenv("FM2_MAX_FPS"))
        g_settings.maxFps = uint32_t(atoi(v));
    if (const char* v = getenv("FM2_VSYNC"))
        g_settings.vsync = atoi(v) != 0;
    if (const char* v = getenv("FM2_UPSCALER"))
        g_settings.upscaler = ParseUpscaler(v);
    if (const char* v = getenv("FM2_FSR_SCALE"))
        g_settings.renderScale = uint32_t(atoi(v));
    g_settings.renderScale = std::clamp<uint32_t>(g_settings.renderScale, 50, 100);
    if (const char* v = getenv("FM2_UPSCALE_QUALITY"))
        g_settings.quality = uint32_t(atoi(v));
    if (const char* v = getenv("FM2_SHARPNESS"))
        g_settings.sharpness = float(atof(v));
    g_settings.quality = std::min<uint32_t>(g_settings.quality, 4);
    g_settings.sharpness = std::clamp(g_settings.sharpness, 0.0f, 1.0f);
    if (const char* v = getenv("FM2_FULLSCREEN"))
        g_settings.fullscreen = atoi(v) != 0;
    if (const char* v = getenv("FM2_ENHANCE"))
        g_settings.enhance = atoi(v) != 0;
    if (const char* v = getenv("FM2_LANGUAGE"))
        g_settings.language = uint32_t(atoi(v));
    if (!g_settings.language)
        g_settings.language = 1;
    if (const char* v = getenv("FM2_SKIP_INTRO"))
        g_settings.skipIntro = atoi(v) != 0;
    if (const char* v = getenv("FM2_LOW_PERFORMANCE"))
        g_settings.lowPerformance = atoi(v) != 0;
    if (const char* v = getenv("FM2_WHEEL_STRENGTH"))
        g_settings.wheelStrength = uint32_t(atoi(v));
    if (const char* v = getenv("FM2_WHEEL_SPRING"))
        g_settings.wheelSpring = uint32_t(atoi(v));
    if (const char* v = getenv("FM2_WHEEL_REVERSE"))
        g_settings.wheelReverse = atoi(v) != 0;
    if (const char* v = getenv("FM2_WHEEL_ROTATION"))
        g_settings.wheelRotation = uint32_t(atoi(v));
    g_settings.wheelStrength = std::min<uint32_t>(g_settings.wheelStrength, 100);
    g_settings.wheelSpring = std::min<uint32_t>(g_settings.wheelSpring, 100);
    g_settings.wheelRotation = std::min<uint32_t>(g_settings.wheelRotation, 3600);
    if (const char* v = getenv("FM2_MSAA"))
        g_settings.msaa = uint32_t(atoi(v));
    g_settings.msaa = g_settings.msaa >= 8 ? 8 : g_settings.msaa >= 4 ? 4 : g_settings.msaa >= 2 ? 2 : 1;
    // DLSS and FSR 3 anti-alias the image themselves; with MSAA as well the device is removed at
    // the first scene resolve.
    if (g_settings.upscaler == Settings::Upscaler::Dlss || g_settings.upscaler == Settings::Upscaler::Fsr3)
        g_settings.msaa = 1;
    if (const char* v = getenv("FM2_SMAA"))
        g_settings.smaa = uint32_t(atoi(v));
    g_settings.smaa = std::min<uint32_t>(g_settings.smaa, 4);
    if (const char* v = getenv("FM2_ANISOTROPY"))
        g_settings.anisotropy = uint32_t(atoi(v));
    {
        const uint32_t a = g_settings.anisotropy;
        g_settings.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0;
    }
    g_settings.width = std::clamp<uint32_t>(g_settings.width, 640, 15360);
    g_settings.height = std::clamp<uint32_t>(g_settings.height, 360, 8640);
    g_settings.maxFps = std::clamp<uint32_t>(g_settings.maxFps ? g_settings.maxFps : 60, 30, 360);
    if (g_settings.profile.empty())
        g_settings.profile = "Player";
}
}

void Settings::RenderSize(uint32_t& w, uint32_t& h) const
{
    w = width;
    h = height;
    // The forced aspect, or the resolution's own; never narrower than 16:9 (narrower views
    // are not supported yet: such resolutions get 16:9 with bars).
    uint64_t aw = aspectW, ah = aspectH;
    if (!aw || !ah)
        aw = width, ah = height;
    if (aw * 9 < ah * 16)
        aw = 16, ah = 9;
    // Fit that aspect inside the resolution.
    if (uint64_t(width) * ah > uint64_t(height) * aw)
        w = uint32_t(uint64_t(height) * aw / ah) & ~1u;
    else
        h = uint32_t(uint64_t(width) * ah / aw) & ~1u;
}

float Settings::SceneScale() const
{
    if (upscaler == Upscaler::Off)
        return 1.0f;
    if (upscaler == Upscaler::Fsr1)
        return float(renderScale) / 100.0f;
    // The usual presets (native, 1.5x, 1.7x, 2x, 3x per axis).
    static const float scales[] = { 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 0.5f, 1.0f / 3.0f };
    return scales[std::min<uint32_t>(quality, 4)];
}

Settings& GetSettings()
{
    if (!g_loaded)
    {
        g_loaded = true;
        Load();
    }
    return g_settings;
}

void SaveSettings()
{
    const Settings& s = GetSettings();
    std::ofstream out(Path());
    out << "# Forza Motorsport 2 (fm2) settings; the launcher edits this file.\n";
    out << "profile=" << s.profile << "\n";
    out << "resolution=" << s.width << "x" << s.height << "\n";
    out << "aspect=" << (s.aspectW ? std::to_string(s.aspectW) + ":" + std::to_string(s.aspectH) : std::string("auto")) << "\n";
    out << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
    out << "vsync=" << (s.vsync ? 1 : 0) << "\n";
    out << "max_fps=" << s.maxFps << "\n";
    out << "enhance=" << (s.enhance ? 1 : 0) << "\n";
    out << "skip_intro=" << (s.skipIntro ? 1 : 0) << "\n";
    out << "xbox_prompts=" << (s.xboxPrompts ? 1 : 0) << "\n";
    out << "hide_shader_overlay=" << (s.hideShaderOverlay ? 1 : 0) << "\n";
    out << "low_performance=" << (s.lowPerformance ? 1 : 0) << "\n";
    out << "vertex_arena=" << (s.vertexArena ? 1 : 0) << "\n";
    out << "wheel_strength=" << s.wheelStrength << "\n";
    out << "wheel_spring=" << s.wheelSpring << "\n";
    out << "wheel_reverse=" << (s.wheelReverse ? 1 : 0) << "\n";
    out << "wheel_rotation=" << s.wheelRotation << "\n";
    out << "language=" << s.language << "\n";
    out << "msaa=" << s.msaa << "\n";
    out << "smaa=" << s.smaa << "\n";
    out << "anisotropy=" << s.anisotropy << "\n";
    out << "upscaler=" << UpscalerName(s.upscaler) << "\n";
    out << "upscale_quality=" << s.quality << "\n";
    out << "sharpness=" << s.sharpness << "\n";
    out << "render_scale=" << s.renderScale << "\n";
}
