#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

// AMD FSR 3 frame generation (fsrfg.cpp; experimental): a frame made between each two the game renders, from
// the picture as presented and the scene's depth and motion vectors (so only with the FSR 3 / FSR 4 upscaler,
// which draws them). It lives in AMD's own swap chain, which presents the made frames and paces them.

// frame_generation=1 in settings.ini (FM2_FRAMEGEN): decided once, when the window is made.
bool FsrFgWanted();
// The window's swap chain as AMD's frame generation swap chain (an IDXGISwapChain4 like any other). False:
// make the plain one (the DLL is missing or the call failed; logged).
bool FsrFgCreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc, IDXGISwapChain4** out);
bool FsrFgHasSwapChain();
// Frames handed to the frame generation so far (each one gets a made frame shown before it).
uint64_t FsrFgMade();
// AMD's frame generation work runs on its own compute queue beside the next frame's rendering, instead of on
// the game's queue inside Present. Off unless FM2_FRAMEGEN_ASYNC=1 (not yet shown to be faster). A HUD-less picture must then
// stay untouched for a frame longer: the renderer keeps two and alternates.
bool FsrFgAsync();

struct FsrFgFrame
{
    // In NON_PIXEL_SHADER_RESOURCE, both exactly the render size and the way up the picture is presented.
    ID3D12Resource *depth, *motion;
    // The picture as it is presented but without the HUD (back buffer size and format, in
    // NON_PIXEL_SHADER_RESOURCE until the next frame), or null: where it differs from the presented picture is
    // HUD, which the made frame takes as it is instead of moving it with the scene behind.
    ID3D12Resource* hudless = nullptr;
    uint32_t renderWidth, renderHeight, displayWidth, displayHeight;
    float jitterX, jitterY;           // render pixels
    float motionScaleX, motionScaleY; // motion texture values to render pixels
    float frameTimeMs;
    bool reset, depthInverted;
};
// This frame has a scene: its depth and motion go to the frame generation context (made on first use; records
// into `list`, the real command list) and the present that follows shows a made frame before the real one.
bool FsrFgPrepare(ID3D12Device* device, ID3D12GraphicsCommandList* list, const FsrFgFrame& frame);
// Before every Present of the swap chain: a frame nothing was prepared for (menus, loading, a held frame) is
// presented as it is.
void FsrFgBeforePresent();
