#pragma once
// ReGPU-V build: the window and presentation are ReGPU-V's. What the port asks of the window (regpu_host/presenter_glue.cpp).
#include <cstdint>
#include <string>

inline void PresenterInit() {}
// Keyboard input goes to the game only while the window has focus.
bool PresenterHasFocus();
// A controller was used: the mouse cursor hides over the game window until the mouse moves (GitHub #12).
void PresenterControllerUsed();
// The console's on-screen keyboard (XamShowKeyboardUI): a small text box over the game window. Blocks
// until OK (true, text set) or Cancel (false). text holds the default on entry.
bool PresenterAskText(const std::wstring& title, const std::wstring& description, std::wstring& text, uint32_t maxLength);
