#pragma once
// FPS meter: frame rate now, average and 1% low since the last reset, drawn in the overlay panel.
// Game-neutral (no FM2 code), written to move to ReGPU unchanged.
//
// On with REGPU_FPS_OVERLAY=1 (FM2_FPS_OVERLAY=1 also works), live: the debug server can flip it.
// F10 in the game window, or flipping REGPU_FPS_RESET (or FM2_FPS_RESET) either way, restarts the average and the lows.
// While it is on, the log gets one "fps meter:" line every 10 s.
#include <string>
#include <vector>

// Once per presented frame, with the time since the previous one.
void FpsMeterFrame(float frameMs);

// Appends the meter's lines when it is on; false when it is off.
bool FpsMeterLines(std::vector<std::string>& lines);
