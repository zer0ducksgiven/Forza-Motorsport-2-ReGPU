#pragma once
#include <cstdint>
// The benchmark (regpu_host/bench.cpp, FM2_AI_BENCH=<seconds>): frozen grid, then the AI drives the player's car.
// The game's "driving" state (hooks/wheel_driving.cpp): the first time it comes on, the race has started.
void BenchDriving(bool driving);
// ReGPU's frame end (Host::onSwap): the frame times.
void BenchSwap();
// Frame-locked game time (kernel/runtime.cpp's TickCount): >= 0 while the benchmark's start runs on frames (FM2_BENCH_FIXED,
// default on), -1 otherwise; BenchTickEndNs is where it stopped (real time goes on from there).
int64_t BenchTickNs();
int64_t BenchTickEndNs();
