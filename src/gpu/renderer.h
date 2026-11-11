#pragma once
#include <cstdint>
#include <vector>

// Native renderer, called on the command processor thread.
void RendererOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords);
// The same with the used length and hash already known (Fnv of the used dwords ^ type).
void RendererSetShader(uint32_t type, const uint32_t* microcodeBE, uint32_t usedDwords, uint64_t hash);
void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
// frontBuffer: physical address of the resolved image to show.
void RendererOnSwap(uint64_t frame, uint32_t frontBuffer);
// Adds the compilation/performance overlay to a CPU RGBA8 frame before presentation.
void RendererOverlayImage(uint32_t width, uint32_t height, std::vector<uint8_t>& rgba);
// The overlay itself, for a renderer that draws it on the GPU: RGBA8 rows of `width` pixels, of which the top
// `visibleHeight` are in use; `version` changes when the pixels do. False when there is nothing to show.
bool RendererOverlayPixels(const uint8_t*& rgba, uint32_t& width, uint32_t& height, uint32_t& visibleHeight, uint64_t& version);
// Lookahead hooks (see GpuLookahead): shader loads and draws the command processor has
// not executed yet, seen on a shadow copy of the registers.
void RendererPredictBegin();
void RendererPredictShader(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords);
void RendererPredictDraw(uint32_t initiator);
// True when no shader/pipeline compile is running and none finished in the last 2 s
// (the shader tour waits for this between steps).
bool RendererCompileIdle();
// Captures the next presented frame (the tour's CAPTURE step).
void RendererRequestCapture();
bool RendererCaptureComplete();
void RendererMarkCaptureComplete();
// Flushes the pipeline library to disk (called when the tour finishes).
void RendererSaveCaches();
// Saves what can be saved quickly when the window is closed (the Vulkan pipeline cache), just before the process exits.
void RendererSaveOnClose();
// Compile activity of a renderer that compiles on its own (Vulkan), for the shader compile pop-up: see renderer.cpp.
struct RendererCompileReport
{
    uint32_t shadersBuilt = 0, shadersCached = 0, pipelinesBuilt = 0, pipelinesCached = 0;
    uint32_t running = 0;
    const char* what = nullptr;
    bool waited = false;
    uint32_t warmDone = 0, warmTotal = 0, staticDone = 0, staticTotal = 0;
};
void RendererReportCompiles(const RendererCompileReport& report);
// The display gamma table entry `index` (10:10:10 RGB), applied when presenting.
void RendererSetGammaEntry(uint32_t index, uint32_t rgb10);
// Fill the 17 display-curve points; false when the table is unset or the identity map.
bool RendererDisplayGamma(float points[17]);
// Benchmark: frame times are recorded while on; the report summarises them.
void BenchmarkRecord(bool on);
void BenchmarkWriteReport(const char* path);
// True while FM2_TRACE_DRAWS's frame is being recorded.
bool RendererTracing();

// The graphics options screen: `change` edits the settings at the next frame start, with the GPU idle; the resolution,
// aspect ratio, display mode, MSAA and filtering then follow them, and they are saved.
#include <functional>
struct Settings;
void RendererChangeSettings(std::function<void(Settings&)> change);

// Parallel draw translation (gpu.cpp cuts the draw stream between resolves into pieces).
#include <functional>
struct RendererTile
{
    int32_t originY = 0;
    int32_t rect[4] = { 0, 0, 1 << 14, 1 << 14 };
};
// Whether draws may be translated on workers now (game settled, no upscaler, ...).
bool RendererParallelReady();
uint32_t RendererDrawWorkers();
// The current draw is a resolve (EDRAM copy): a point the stream is not cut across.
bool RendererIsResolveDraw();
// Occlusion queries (EVENT_WRITE_ZPD): Begin starts counting the samples later draws pass;
// End stops and gives the count in console samples (of the same query in the last completed
// frame), false while there is none yet.
void RendererOcclusionBegin();
bool RendererOcclusionEnd(uint32_t& samples);
// Follows the tile state draws set (see DrawImpl), for the state at each piece's start.
void RendererTrackTile(RendererTile& tile);
void RendererSetTile(const RendererTile& tile);
// Runs translate(i), i in [0, n), on the draw workers (and this thread), each into its own
// command list, and executes them in order after everything recorded so far.
void RendererRunPieces(uint32_t n, const std::function<void(uint32_t)>& translate);
