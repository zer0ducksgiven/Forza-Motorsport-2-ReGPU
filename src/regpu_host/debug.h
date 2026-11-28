#pragma once
// ReGPU-V build: the debug server is ReGPU's (REGPU_DEBUG=1, port REGPU_DEBUG_PORT or 7341; thirdparty/ReGPU's
// tools/regpuctl.py): it drives the pad, takes screenshots, flips switches. FM2's own server (debug.cpp,
// tools/fm2ctl.py) belongs to the port's renderer and is not built.
#include "regpu/regpu.h"

using DebugPad = regpu::DebugPad;
using regpu::DebugPadState;
inline void DebugServerStart() {}
