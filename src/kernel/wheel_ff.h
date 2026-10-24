#pragma once
#include <cstdint>

// FM2_FAKE_WHEEL=1: player 1 is reported as a force feedback wheel (no wheel behind it yet).
bool WheelFakeEnabled();
// The force feedback calls are handed to the game (always, unless FM2_NO_WHEEL=1: it only uses them
// once a wheel with force feedback is reported).
bool WheelFfServed();
// The guest address of a call the wheel's force feedback needs: xboxkrnl's XInputdFF* (ordinals
// 0x282-0x289) and xam's XamInputGetUserVibrationLevel (442) and XamUserGetDeviceContext (520).
// 0 for others. returnAddress: the caller's, inside the game's lookup helper (see wheel_ff.cpp).
uint32_t WheelFfProcedure(uint32_t module, uint32_t ordinal, uint32_t returnAddress);
// Stand for xboxkrnl.exe and xam.xex in XexGetModuleHandle and XexGetProcedureAddress.
constexpr uint32_t kKernelModuleHandle = 0x4B524E4C; // "KRNL"
constexpr uint32_t kXamModuleHandle = 0x58414D20;    // "XAM "
