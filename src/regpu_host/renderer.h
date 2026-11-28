#pragma once
// ReGPU-V build: the renderer calls the rest of the port makes, on ReGPU-V (regpu_host/gpu_host.cpp).
#include <functional>
#include "regpu/regpu.h"

struct Settings;

void RendererSaveCaches(); // (ReGPU-V's own: the shader and pipeline caches)
inline void RendererSaveOnClose() { regpu::SaveCaches(); }
inline bool RendererCompileIdle() { return regpu::CompileIdle(); }
inline void BenchmarkRecord(bool on) { regpu::BenchmarkRecord(on); }
inline void BenchmarkWriteReport(const char* path) { regpu::BenchmarkWriteReport(path); }
// F12 frame capture (FM2's own tooling): not available with ReGPU-V; the screenshot is the debug server's "shot".
inline void RendererRequestCapture() {}
inline bool RendererCaptureComplete() { return true; }
// The in-game graphics menu: the change is saved and applied where ReGPU can change it while running (regpu_host/gpu_host.cpp).
void RendererChangeSettings(std::function<void(Settings&)> change);
