#pragma once
// Racing wheels through SDL3 (input/wheel.cpp): the wheel and its pedals as an Xbox 360 wheel on
// player 1, and the game's force feedback played on it.
#include <cstdint>

#include "input/wheel_effects.h"

// Starts the wheel thread (once; FM2_NO_WHEEL=1 never does).
void WheelStart();
// Every force off now, for good (exits and crashes).
void WheelStop();

struct WheelInput
{
    bool present = false; // a wheel is connected
    int16_t steer = 0;    // the left stick's X
    uint8_t gas = 0, brake = 0;
    uint16_t buttons = 0; // XINPUT_GAMEPAD_* bits
};
WheelInput WheelRead();
bool WheelHasForceFeedback();

// The game's XInputdFF* calls (kernel/wheel_ff.cpp).
void WheelFfDefine(const wheel::Effect& e);
void WheelFfOperate(uint32_t block, uint32_t op);
void WheelFfControl(uint32_t command);
void WheelFfDeviceGain(uint32_t gain);
// Whether the player is driving, from the game (hooks/wheel_driving.cpp): forces fade out when not.
void WheelSetDriving(bool driving);
