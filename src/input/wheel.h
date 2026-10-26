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

// Controllers XInput doesn't see (DualSense, DualShock 4, Switch Pro and other DirectInput pads),
// through SDL's gamepad mapping, in the order they connected: the players after the XInput pads.
struct GamepadInput
{
    bool present = false;
    uint16_t buttons = 0; // XINPUT_GAMEPAD_* bits
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0; // XInput's directions (up is positive)
};
GamepadInput GamepadRead(uint32_t index);
void GamepadRumble(uint32_t index, uint16_t low, uint16_t high);
// Which controller's button prompts to show: the last one used (gpu/renderer.cpp swaps the game's
// Xbox 360 icons for PlayStation or Xbox One / Series ones; hooks/lua.cpp relabels for the DualSense).
enum class PromptStyle { Xbox360, PlayStation, XboxSeries };
PromptStyle PromptStyleInUse();
inline bool PlayStationPadInUse() { return PromptStyleInUse() == PromptStyle::PlayStation; }
// The last XInput pad use (kernel/imports_xam.cpp): Xbox prompts, 360 or Series by what is connected.
void NoteXInputPadUsed();
