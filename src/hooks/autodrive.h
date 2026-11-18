#pragma once
// Auto-drive (MVP): drives player 1's car from the picture, for hands-off benchmark runs.
// FM2_AUTODRIVE=1 (live: fm2ctl.py switch FM2_AUTODRIVE 1). Start it in a race; in menus it holds the
// throttle and steers at nothing, so switch it on once the race has started.
//
// The renderer copies every third frame back (no stall, read when its frame slot comes round) and calls
// AutodriveAnalyze; the pad code adds AutodrivePad's stick and triggers to player 1's input.
// FM2_AUTODRIVE_DEBUG=1 logs each decision and writes out/autodrive.png (the road mask) every 2 s.
#include <cstdint>

bool AutodriveOn();
void AutodriveAnalyze(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch);
// The current input: left stick x (-32768..32767), right and left triggers. False when off.
bool AutodrivePad(int16_t& lx, uint8_t& throttle, uint8_t& brake);
// Test tooling: true once the analyser has seen road (the race view is up); FM2_DUMP_AFTER_ROAD keys captures on it.
bool AutodriveRoadSeen();
