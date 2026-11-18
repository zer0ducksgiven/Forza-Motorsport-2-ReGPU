#pragma once
// Hyper: a low-spec, high-performance mode (see HYPER.md in the ReGPU_Hyper workspace). It degrades how the GPU
// work is done (resolution, precision, sampling, refresh) and never removes any of it: every pass, draw and effect
// still runs. Strictly generic: decided from what the renderer sees, with no game-specific knowledge.
// Game-neutral, written to move to ReGPU unchanged.
//
// REGPU_HYPER=off|mild|low|extreme|potato picks a preset (FM2_HYPER also works). Each cut can be set on its own, over the
// preset, for A/B tests:
//   REGPU_HYPER_SCENE=<percent>   the frame's render scale, 25-100 (FSR 1 upscales it to the output)
//   REGPU_HYPER_UPSCALE=0|1       the final upscale as one cheap bilinear pass (blurrier than FSR 1)
//
// Every value can also come from the port's own settings (a settings file, a launcher): HyperConfigure, called before
// the renderer starts. Order: the environment (A/B tests) wins over the settings, which win over the preset.
#include <cstdint>
#include <string>

// What a port's settings say; empty or negative means "not set" (the preset decides).
struct HyperSettings
{
    std::string preset;   // off, mild, low, extreme
    int scenePercent = -1; // 25-100
    int cheapUpscale = -1; // 0 or 1
    int noDiscard = -1;    // 0 or 1
    int helpers = -1;      // percent, 25-100
    int resolve1010 = -1;  // 0 or 1
    int aniso = -1;        // 0, 2, 4, 8 or 16
    int decimate = -1;     // percent of triangles kept
};
void HyperConfigure(const HyperSettings& settings);

enum class HyperPreset
{
    Off,
    Mild,
    Low,
    Extreme,
    Potato, // native resolution: the strongest kept native cuts (no scene scale, no upscale) [laptop]
};

HyperPreset HyperLevel();
const char* HyperName(HyperPreset preset);

// Cut 2: the render scale (percent of the output per axis) Hyper asks for, 0 when it leaves it alone.
uint32_t HyperSceneScalePercent();

// Cut: the final upscale as one bilinear pass with the display gamma (instead of FSR 1's EASU and RCAS plus a gamma
// pass): blurrier, fewer full-frame passes. REGPU_HYPER_UPSCALE=0|1; on in the Low and Extreme presets.
bool HyperCheapUpscale();

// Cut (lossless): draws without the alpha test use a pixel shader compiled without its discard, so the GPU keeps
// early depth. REGPU_HYPER_NODISCARD=0|1, settings key hyper_nodiscard=; on in every preset.
bool HyperNoDiscard();

// Cut (lossless): resolves into the guest's 32-bit k_2_10_10_10 kept as R10G10B10A2 on the host (not RGBA16F): half
// the bandwidth. REGPU_HYPER_RESOLVE1010=0|1, settings key hyper_resolve1010=; on in every preset.
bool HyperResolve1010();

// Cut: offscreen helper surfaces (narrower than half the screen: reflection views, blur and bloom) drawn at this
// percent of their size in each direction; the screen keeps its resolution. 100 with Hyper off; Mild 75, Low and
// Extreme 50. REGPU_HYPER_HELPERS=<25-100>, settings key hyper_helpers=.
uint32_t HyperHelperScalePercent();

// Cut: the most anisotropic filtering samplers may use (the player's level when lower). 16 with Hyper off;
// Mild 4, Low and Extreme 2. REGPU_HYPER_ANISO=<0|2|4|8|16>, settings key hyper_aniso=.
uint32_t HyperAnisotropyCap();

// Cut: big meshes drawn with fewer triangles (index buffers simplified on a worker thread from the positions the
// vertex shader fetches; borders kept so pieces still meet). Percent of triangles kept, 0 off.
// REGPU_HYPER_DECIMATE=<10-90>, settings key hyper_decimate=; 25 in the Extreme preset. Tuning (measurement):
// REGPU_HYPER_DECIMATE_MIN=<indices> smallest mesh (300), REGPU_HYPER_DECIMATE_ERROR=<0-1> largest change (0.2).
uint32_t HyperDecimatePercent();

// Lean texture fetch (renderer.cpp) [laptop]: 0 off, 1 lossless, 2 baked texture state, 3 baked + plain multiplies on 3D
// draws. REGPU_HYPER_LEAN_FETCH=<0-3>; 3 in the Potato preset.
int HyperLeanFetch();
// Lean vertex fetch (renderer.cpp, lossless) [laptop]: each vertex shader's fetch layout baked in as constants.
// REGPU_HYPER_LEAN_VFETCH=0|1; on in the Potato preset.
bool HyperLeanVFetch();
// Draw culling by pixel count (renderer.cpp drawcull) [laptop]: -1 off, else the pixel threshold.
// REGPU_HYPER_CULL_PIXELS=<n>; 64 in the Potato preset, with every depth-tested draw eligible (REGPU_HYPER_CULL_ALL=0|1).
int HyperCullPixels();
bool HyperCullAll();

// One line for the log: the preset and every cut's value.
void HyperLogSummary();
