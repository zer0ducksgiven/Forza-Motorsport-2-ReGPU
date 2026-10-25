#pragma once
// The live debug server (FM2_DEBUG=1, port FM2_DEBUG_PORT or 7341, 127.0.0.1 only): drive and inspect the
// running game without restarting it. tools/fm2ctl.py is its client; the commands are listed there.
#include <cstdint>
#include <string>

void DebugServerStart();

// Input from the server, merged into player 1's pad (imports_xam.cpp ReadPad). False when nothing is held.
struct DebugPad
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool sticks = false, triggers = false; // replace the pad's sticks / triggers
};
// True once the server has sent input (player 1 then stays connected, for a PC with no pad).
bool DebugPadState(DebugPad& pad);

// The renderer's side: each swap, and screenshots (a pending one names its file).
void DebugOnSwap(uint64_t frame);
bool DebugShotPending(std::string& path);
void DebugShotDone(const std::string& path);
