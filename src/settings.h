#pragma once
#include <cstdint>
#include <string>

// Player settings: Documents\fm2\settings.ini, edited by the launcher. Environment
// variables (FM2_RESOLUTION, FM2_RENDER_SCALE, FM2_MAX_FPS, FM2_VSYNC, FM2_FULLSCREEN,
// FM2_PROFILE, FM2_SKIP_INTRO) override the file for testing.
struct Settings
{
    std::string profile = "Player";     // the gamer profile name the game sees
    uint32_t width = 1280, height = 720; // output resolution
    uint32_t aspectW = 0, aspectH = 0;   // forced aspect ratio (0 = from the resolution)
    bool fullscreen = false;             // borderless fullscreen on the primary display
    bool vsync = true;
    uint32_t maxFps = 60;                // frame cap, 30-360 (the physics tick rate)
    bool enhance = false;                // enhanced lighting post pass (experimental)
    bool skipIntro = false;              // straight to the title screen (no legal text or splash videos)
    bool xboxPrompts = false;            // keep the Xbox 360 button icons with a PlayStation pad
    bool hideShaderOverlay = true;       // no shader compiler box in the top left
    bool raceMusic = false;              // the player's MP3s (Documents\fm2\music) while driving (audio/music.cpp)
    uint32_t musicVolume = 40;           // percent
    // Car sound mix: each car's exhaust and turbo/supercharger loudness in percent of the game's (0-400), as
    // its audio tuning loads (kernel/zip_overlay.cpp).
    bool carSoundMix = false;
    uint32_t exhaustVolume = 100, turboVolume = 100;
    // Low performance mode: for old CPUs, turns off renderer changes that help fast PCs but cost
    // slow ones (staged draw constants: an FX-8350 lost time copying them).
    bool lowPerformance = false;
    // Large vertex buffers kept in video memory, copied again only when the game writes them
    // (renderer.cpp's vertex arena). A debug setting: off reads them in place again (or copies
    // them every frame where guest memory isn't mapped for the GPU).
    bool vertexArena = true;
    // Racing wheels (input/wheel.cpp): force feedback strength and the centring spring and damper
    // on top of it (percent), the steering force reversed, and the rotation the wheel is set to in
    // its own software (degrees; the game steers as the 360 wheel's 270 from it; 0 uses the
    // wheel's range as it is).
    uint32_t wheelStrength = 50, wheelSpring = 50;
    bool wheelReverse = false;
    uint32_t wheelRotation = 0;
    uint32_t language = 1;               // the console language the game sees (XGetLanguage: 1 English, 3 German, ...)
    // Online (FM2 Live, server/live): the server's base URL, https://... Set, the game signs in to Xbox Live there:
    // sessions over wss://.../live, the auction house over wss://.../xts, leaderboards at https://.../stats.
    // FM2_LIVE, FM2_SERVER, FM2_TITLE_SERVER and FM2_STATS_URL override it for testing.
    std::string onlineServer;
    uint32_t msaa = 1;                   // anti-aliasing samples per pixel: 1 (off), 2, 4 or 8
    // SMAA 1x on the whole frame as it is shown (after MSAA, before FSR 1): 0 off, 1 low, 2 medium,
    // 3 high, 4 ultra (the SMAA presets).
    uint32_t smaa = 0;
    uint32_t anisotropy = 16;            // anisotropic filtering of surface textures: 0 (off: trilinear), 2, 4, 8 or 16
    // Upscaling. FSR 1 (the launcher's): the whole frame, HUD included, renders at render_scale
    // and is upscaled (EASU) and sharpened (RCAS) as it is shown; at 100% it is only sharpened.
    // FSR 3 and DLSS (testing only): the 3D scene renders smaller and is reconstructed before
    // post-processing and the HUD, at the size `quality` picks.
    enum class Upscaler : uint32_t { Off, Fsr1, Fsr3, Dlss };
    Upscaler upscaler = Upscaler::Off;
    uint32_t renderScale = 100; // FSR 1: percent of the output size per axis, 50-100
    uint32_t quality = 1;   // 0 native (anti-aliasing only), 1 quality, 2 balanced, 3 performance, 4 ultra performance
    float sharpness = 0.5f; // 0-1
    // The scene's resolution as a fraction of the output (1 when upscaling is off).
    float SceneScale() const;

    // The size the game renders at: the resolution, narrowed or shortened to the forced
    // aspect ratio (the rest of the screen is black bars).
    void RenderSize(uint32_t& w, uint32_t& h) const;
};

// Loaded on first use (after RuntimeInit has set the user directory).
Settings& GetSettings();
void SaveSettings();
