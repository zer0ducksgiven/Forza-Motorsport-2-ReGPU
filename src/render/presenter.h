#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Window + D3D12 swap chain (FM2_HEADLESS=1 disables it).
void PresenterInit();
// The size the game renders at (settings.h: resolution and forced aspect ratio). The swap
// chain is the resolution; the image is centred in it and the swap chain stretches to the
// window.
void RenderSize(uint32_t& width, uint32_t& height);
// The primary display's refresh rate (the frame cap with vsync on).
uint32_t DisplayRefreshHz();
// Called from VdSwap: the guest finished a frame.
void PresenterSubmitFrame(uint64_t frame);
// Shows a CPU-side RGBA8 image (scaled 1:1 into the 1280x720 back buffer).
void PresenterSubmitImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba);
// Keyboard input goes to the game only while the window has focus.
bool PresenterHasFocus();
// A controller was used: the mouse cursor hides over the game window until the mouse moves (GitHub #12).
void PresenterControllerUsed();
// The console's on-screen keyboard (XamShowKeyboardUI): a small text box over the game window. Blocks
// until OK (true, text set) or Cancel (false). text holds the default on entry.
bool PresenterAskText(const std::wstring& title, const std::wstring& description, std::wstring& text, uint32_t maxLength);

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// Shared D3D12 device and queue (created on first use, also when headless).
ID3D12Device* RenderDevice();
ID3D12CommandQueue* RenderQueue();
// Records copying src (COPY_SOURCE, RGBA8 at RenderSize()) into the current back
// buffer; returns false when there is no window. Call PresenterPresent()
// after executing the list.
// overlay (COPY_SOURCE RGBA8, overlayW x overlayH) is copied over the top-left corner when given.
// toast: a box for the bottom centre (its used size), risen from below the image by `toastRise` (0..1).
bool PresenterRecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* overlay = nullptr, uint32_t overlayW = 0, uint32_t overlayH = 0,
                         ID3D12Resource* toast = nullptr, uint32_t toastW = 0, uint32_t toastH = 0, float toastRise = 0.0f);
void PresenterPresent();
// Called by the renderer: the presenter stops drawing its own frames.
void PresenterUseRenderer();
