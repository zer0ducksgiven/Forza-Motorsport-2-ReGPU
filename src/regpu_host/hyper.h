#pragma once
// ReGPU-V build: Hypermode (low-spec presets) is ReGPU-V's (include/regpu/hyper.h, ported from this port's hyper-prototype).
#include "regpu/hyper.h"

using HyperSettings = regpu::hypermode::Settings;
using HyperPreset = regpu::hypermode::Preset;
inline void HyperConfigure(const HyperSettings& s) { regpu::hypermode::Configure(s); }
inline HyperPreset HyperLevel() { return regpu::hypermode::Level(); }
inline const char* HyperName(HyperPreset p) { return regpu::hypermode::Name(p); }
inline uint32_t HyperSceneScalePercent() { return regpu::hypermode::SceneScalePercent(); }
inline bool HyperCheapUpscale() { return regpu::hypermode::CheapUpscale(); }
inline uint32_t HyperAnisotropyCap() { return regpu::hypermode::AnisotropyCap(); }
inline void HyperLogSummary() { regpu::hypermode::LogSummary(); }
