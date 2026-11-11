// Native output: a window with a D3D12 flip-model swap chain. The guest's
// VdSwap hands a frame over; a render thread presents it without blocking
// the guest. Until the D3D hooks draw real content the frame is a clear
// whose shade pulses with the frame count, proving the path end to end.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include "gpu/fsrfg.h"
#include "input/wheel.h"
#include "log.h"
#include "render/presenter.h"
#include "settings.h"

void LogSystemInfo(ID3D12Device* device); // sysinfo.cpp

using Microsoft::WRL::ComPtr;

void DealerOnChar(wchar_t c); // hooks/dealer.cpp: a typed easter-egg code

namespace
{
constexpr UINT kBuffers = 3;

struct Presenter
{
    HWND window = nullptr;
    HWND surface = nullptr; // the child window holding the swap chain
    std::atomic<bool> fullscreen{ false };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvStride = 0;
    ComPtr<ID3D12Resource> backBuffers[kBuffers];
    ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValues[kBuffers] = {};
    UINT64 nextFence = 1;
    HANDLE fenceEvent = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    uint64_t pendingFrame = 0;
    std::vector<uint8_t> image; // latest submitted RGBA8, imageWidth x imageHeight
    uint32_t imageWidth = 0, imageHeight = 0;
    uint64_t uploadBytes[kBuffers]{};
    ComPtr<ID3D12Resource> upload[kBuffers];
    uint64_t presentedFrame = 0;
    std::atomic<bool> ready{ false };
    // The Vulkan renderer presents through its own swap chain on the window: this one is then never made. It is
    // made late instead if that renderer has to fall back to handing over images (PresenterSubmitImage).
    std::atomic<bool> claimed{ false };
};

Presenter g_p;
}

// The size the game renders at (settings: resolution and forced aspect ratio).
void RenderSize(uint32_t& width, uint32_t& height)
{
    GetSettings().RenderSize(width, height);
}

namespace
{

std::atomic<bool> g_cursorHidden{ false };
constexpr UINT kHideCursor = WM_APP + 1;

void LayoutSurface();

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE && hwnd == g_p.window)
        LayoutSurface();
    // The cursor hides after controller use (PresenterControllerUsed) and comes back when the mouse moves.
    if (msg == kHideCursor || (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g_cursorHidden))
    {
        SetCursor(nullptr);
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE)
    {
        static LPARAM last = -1;
        if (lp != last && last != -1 && g_cursorHidden.exchange(false))
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        last = lp;
    }
    if (msg == WM_CLOSE)
    {
        Log("presenter: window closed, exiting");
        WheelStop();
        std::_Exit(0);
    }
    if (msg == WM_CHAR)
    {
        DealerOnChar(wchar_t(wp));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The swap chain's child window in its parent: fullscreen, the largest rectangle of the output's aspect ratio,
// centred (the parent's black background shows around it, and other windows cannot cover the bars); windowed, the
// whole client area (the picture stretches with the window).
void LayoutSurface()
{
    if (!g_p.surface)
        return;
    RECT c{};
    GetClientRect(g_p.window, &c);
    LONG cw = c.right, ch = c.bottom, w = cw, h = ch;
    const Settings& st = GetSettings();
    if (g_p.fullscreen && st.width && st.height)
    {
        if (int64_t(cw) * st.height > int64_t(ch) * st.width)
            w = LONG(int64_t(ch) * st.width / st.height);
        else
            h = LONG(int64_t(cw) * st.height / st.width);
    }
    const bool own = GetWindowThreadProcessId(g_p.surface, nullptr) == GetCurrentThreadId();
    SetWindowPos(g_p.surface, nullptr, (cw - w) / 2, (ch - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE | (own ? 0u : UINT(SWP_ASYNCWINDOWPOS)));
}

// The window for a client area of this size, shrunk (aspect kept) to fit the screen.
SIZE WindowSizeFor(uint32_t width, uint32_t height)
{
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    LONG cw = LONG(width), ch = LONG(height);
    const LONG maxW = (work.right - work.left) * 9 / 10, maxH = (work.bottom - work.top) * 9 / 10;
    if (cw > maxW || ch > maxH)
    {
        const double f = std::min(double(maxW) / cw, double(maxH) / ch);
        cw = LONG(cw * f);
        ch = LONG(ch * f);
    }
    RECT r{ 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return { r.right - r.left, r.bottom - r.top };
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fatal("presenter: %s failed (%08lX)", what, hr);
}

std::mutex g_deviceMutex;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
std::atomic<bool> g_rendererActive{ false };
std::atomic<uint32_t> g_presents{ 0 }; // frames shown (for the fps in the window's title bar)

// Windowed: the frames shown in the last second, in the title bar ("Forza Motorsport 2 - 143 fps"). Called
// from the window's own thread, so setting the text cannot wait on another thread.
void UpdateTitleFps()
{
    static auto last = std::chrono::steady_clock::now();
    static bool shown = false;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    if (seconds < 1.0)
        return;
    last = now;
    const uint32_t frames = g_presents.exchange(0);
    if (g_p.fullscreen)
    {
        if (shown)
            SetWindowTextW(g_p.window, L"Forza Motorsport 2");
        shown = false;
        return;
    }
    wchar_t title[64];
    swprintf(title, 64, L"Forza Motorsport 2 - %.0f fps", double(frames) / seconds);
    SetWindowTextW(g_p.window, title);
    shown = true;
}

void EnsureDevice()
{
    std::lock_guard lock(g_deviceMutex);
    if (g_device)
        return;
    if (getenv("FM2_D3D12_DEBUG"))
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            // FM2_D3D12_GBV=1 (with the debug layer): GPU-based validation and synchronized command
            // queue validation, which report resource state and barrier hazards as the GPU meets them
            // (slow). From the FM4 port.
            ComPtr<ID3D12Debug1> debug1;
            if (getenv("FM2_D3D12_GBV") && SUCCEEDED(debug.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
                debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
                Log("presenter: D3D12 GPU-based validation on (FM2_D3D12_GBV)");
            }
        }
    }
    // FM2_DRED=1: device-removed breadcrumbs and page-fault reporting, so a removed device logs the
    // command list and operation the GPU stopped at (renderer.cpp, LogDebugMessages). From the FM4 port.
    if (getenv("FM2_DRED"))
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Log("presenter: device-removed breadcrumbs and page-fault reporting on (FM2_DRED)");
        }
    }
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice");
    LogSystemInfo(g_device.Get());
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");
}

void CreateWindowAndDevice()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"fm2";
    wc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
    wc.hIcon = HICON(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    RegisterClassW(&wc);
    // The swap chain is the chosen resolution (the game's image is centred in it with black
    // bars when an aspect ratio is forced). Windowed: the client area starts at that size,
    // shrunk (aspect kept) to fit the screen. Fullscreen: a borderless window over the
    // primary display. Either way the swap chain stretches to the window.
    const Settings& settings = GetSettings();
    const uint32_t outW = settings.width, outH = settings.height;
    if (settings.fullscreen)
    {
        // Borderless window over the whole display; the picture is its child window (LayoutSurface).
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        g_p.fullscreen = true;
        g_p.window = CreateWindowExW(0, L"fm2", L"Forza Motorsport 2", WS_POPUP | WS_VISIBLE, 0, 0, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);
    }
    else
    {
        SIZE size = WindowSizeFor(outW, outH);
        g_p.window = CreateWindowW(L"fm2", L"Forza Motorsport 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            size.cx, size.cy, nullptr, nullptr, wc.hInstance, nullptr);
    }
    // The swap chain goes on the window itself, except fullscreen at an aspect ratio other than the display's:
    // there the picture is a child window between black bars (LayoutSurface). A child covering the whole
    // display showed black while focused on an AMD card (report #59). Switching live into fullscreen at another
    // aspect ratio stretches the picture until the next start.
    RECT client{};
    GetClientRect(g_p.window, &client);
    if (g_p.fullscreen && int64_t(client.right) * outH != int64_t(client.bottom) * outW)
    {
        g_p.surface = CreateWindowExW(0, L"fm2", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, g_p.window, nullptr, wc.hInstance, nullptr);
        LayoutSurface();
    }
    Log("presenter: picture in %s", g_p.surface ? "a child window (black bars)" : "the game window");
}

void CreateSwapChain()
{
    const Settings& settings = GetSettings();
    const uint32_t outW = settings.width, outH = settings.height;
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    EnsureDevice();
    g_p.device = g_device;
    g_p.queue = g_queue;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = outW;
    sd.Height = outH;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    // Frame generation (frame_generation=1): AMD's swap chain, which shows the frames it makes between ours.
    ComPtr<IDXGISwapChain4> generated;
    if (FsrFgWanted() && FsrFgCreateSwapChain(factory.Get(), g_p.queue.Get(), g_p.surface ? g_p.surface : g_p.window, &sd, &generated))
        Check(generated.As(&g_p.swapChain), "IDXGISwapChain3 (frame generation)");
    else
    {
        ComPtr<IDXGISwapChain1> sc1;
        Check(factory->CreateSwapChainForHwnd(g_p.queue.Get(), g_p.surface ? g_p.surface : g_p.window, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
        Check(sc1.As(&g_p.swapChain), "IDXGISwapChain3");
    }
    factory->MakeWindowAssociation(g_p.window, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    Check(g_p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_p.rtvHeap)), "CreateDescriptorHeap");
    g_p.rtvStride = g_p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; i++)
    {
        Check(g_p.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_p.backBuffers[i])), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * g_p.rtvStride;
        g_p.device->CreateRenderTargetView(g_p.backBuffers[i].Get(), nullptr, h);
        Check(g_p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_p.allocators[i])), "CreateCommandAllocator");
    }
    Check(g_p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_p.allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_p.list)),
        "CreateCommandList");
    g_p.list->Close();
    Check(g_p.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_p.fence)), "CreateFence");
    g_p.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

}

void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g_p.list->ResourceBarrier(1, &b);
}

void DrawFrame(uint64_t frame)
{
    const UINT i = g_p.swapChain->GetCurrentBackBufferIndex();
    if (g_p.fence->GetCompletedValue() < g_p.fenceValues[i])
    {
        g_p.fence->SetEventOnCompletion(g_p.fenceValues[i], g_p.fenceEvent);
        WaitForSingleObject(g_p.fenceEvent, INFINITE);
    }
    g_p.allocators[i]->Reset();
    g_p.list->Reset(g_p.allocators[i].Get(), nullptr);
    ID3D12Resource* backBuffer = g_p.backBuffers[i].Get();

    // The submitted image is any size (the Vulkan backend sends the output size); it is centred in the back buffer and
    // cropped to it. It used to be assumed 1280x720 and copied to the top-left corner of a larger swap chain.
    bool copied = false;
    uint32_t imageW = 0, imageH = 0;
    {
        std::lock_guard lock(g_p.mutex);
        imageW = g_p.imageWidth;
        imageH = g_p.imageHeight;
        // D3D12 wants the rows of a placed footprint 256-byte aligned.
        const uint32_t rowPitch = (imageW * 4 + 255) & ~255u;
        const uint64_t bytes = uint64_t(rowPitch) * imageH;
        if (bytes && g_p.image.size() == size_t(imageW) * imageH * 4)
        {
            if (g_p.uploadBytes[i] < bytes)
            {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = bytes;
                rd.Height = 1;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = 1;
                rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                // This buffer's last use was waited for above (fenceValues[i]), so it can be replaced.
                g_p.upload[i].Reset();
                g_p.uploadBytes[i] = SUCCEEDED(g_p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_p.upload[i]))) ? bytes : 0;
            }
            if (g_p.uploadBytes[i] >= bytes)
            {
                void* mapped = nullptr;
                g_p.upload[i]->Map(0, nullptr, &mapped);
                if (rowPitch == imageW * 4)
                    memcpy(mapped, g_p.image.data(), g_p.image.size());
                else
                    for (uint32_t y = 0; y < imageH; y++)
                        memcpy(static_cast<uint8_t*>(mapped) + size_t(y) * rowPitch, g_p.image.data() + size_t(y) * imageW * 4, size_t(imageW) * 4);
                g_p.upload[i]->Unmap(0, nullptr);
                copied = true;
            }
        }
    }

    if (copied)
    {
        const D3D12_RESOURCE_DESC backDesc = backBuffer->GetDesc();
        const uint32_t backW = uint32_t(backDesc.Width), backH = backDesc.Height;
        const uint32_t copyW = std::min(imageW, backW), copyH = std::min(imageH, backH);
        if (copyW < backW || copyH < backH)
        {
            Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += SIZE_T(i) * g_p.rtvStride;
            const float black[4] = { 0, 0, 0, 1 };
            g_p.list->ClearRenderTargetView(rtv, black, 0, nullptr);
            Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        else
            Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_p.upload[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, imageW, imageH, 1, (imageW * 4 + 255) & ~255u };
        const D3D12_BOX box{ (imageW - copyW) / 2, (imageH - copyH) / 2, 0, (imageW - copyW) / 2 + copyW, (imageH - copyH) / 2 + copyH, 1 };
        g_p.list->CopyTextureRegion(&dst, (backW - copyW) / 2, (backH - copyH) / 2, 0, &src, &box);
        Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    else
    {
        Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(i) * g_p.rtvStride;
        const float pulse = 0.15f + 0.1f * float((frame % 120) < 60 ? frame % 60 : 60 - frame % 60) / 60.0f;
        const float color[4] = { 0.05f, pulse, 0.12f, 1.0f };
        g_p.list->ClearRenderTargetView(rtv, color, 0, nullptr);
        Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    g_p.list->Close();
    ID3D12CommandList* lists[] = { g_p.list.Get() };
    g_p.queue->ExecuteCommandLists(1, lists);
    FsrFgBeforePresent();
    g_p.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    g_presents++;
    g_p.fenceValues[i] = g_p.nextFence++;
    g_p.queue->Signal(g_p.fence.Get(), g_p.fenceValues[i]);
}

void RenderThread()
{
    SetThreadDescription(GetCurrentThread(), L"presenter");
    CreateWindowAndDevice();
#if defined(FM2_VULKAN)
    const bool deferSwapChain = GetSettings().renderer == Settings::Renderer::Vulkan;
#else
    const bool deferSwapChain = false;
#endif
    if (!deferSwapChain)
        CreateSwapChain();
    g_p.ready = true;
    Log("presenter: window %s", deferSwapChain ? "ready (the Vulkan renderer presents)" : "and D3D12 swap chain ready");
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UpdateTitleFps();
        uint64_t frame;
        {
            std::unique_lock lock(g_p.mutex);
            g_p.cv.wait_for(lock, std::chrono::milliseconds(16), [] { return g_p.pendingFrame != g_p.presentedFrame; });
            frame = g_p.pendingFrame;
            if (frame == g_p.presentedFrame || g_rendererActive)
                continue;
            g_p.presentedFrame = frame;
            // Without a swap chain there is nothing to draw into, unless an image was handed over: then the
            // renderer that deferred it is falling back to this path and the swap chain is made now.
            if (!g_p.swapChain && (g_p.claimed || g_p.image.empty()))
                continue;
        }
        if (!g_p.swapChain)
        {
            CreateSwapChain();
            Log("presenter: D3D12 swap chain made for handed-over images");
        }
        DrawFrame(frame);
    }
}
}

void PresenterInit()
{
    if (getenv("FM2_HEADLESS"))
        return;
    std::thread(RenderThread).detach();
}

void PresenterSubmitFrame(uint64_t frame)
{
    {
        std::lock_guard lock(g_p.mutex);
        g_p.pendingFrame = frame;
    }
    g_p.cv.notify_one();
}

bool PresenterHasFocus()
{
    return !g_p.window || GetForegroundWindow() == g_p.window;
}

void PresenterControllerUsed()
{
    if (g_p.window && !g_cursorHidden.exchange(true))
        PostMessageW(g_p.window, kHideCursor, 0, 0);
}

namespace
{
struct TextBox
{
    HWND edit = nullptr;
    int result = 0; // IDOK or IDCANCEL once closed
};

LRESULT CALLBACK TextBoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* box = reinterpret_cast<TextBox*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COMMAND && box && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
    {
        box->result = LOWORD(wp);
        return 0;
    }
    if (msg == WM_CLOSE && box)
    {
        box->result = IDCANCEL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

bool PresenterAskText(const std::wstring& title, const std::wstring& description, std::wstring& text, uint32_t maxLength)
{
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TextBoxProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"fm2text";
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered)
        return false;
    const UINT dpi = g_p.window ? GetDpiForWindow(g_p.window) : 96;
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    const int w = px(420), h = px(150);
    RECT r{};
    if (!g_p.window || !GetWindowRect(g_p.window, &r))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    TextBox box;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"fm2text", title.empty() ? L"Forza Motorsport 2" : title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, (r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2, w, h, g_p.window, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd)
        return false;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(&box));
    RECT c{};
    GetClientRect(hwnd, &c);
    HFONT font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto add = [&](const wchar_t* cls, const wchar_t* label, DWORD style, int x, int y, int cw, int ch, int id) {
        HWND child = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, label, WS_CHILD | WS_VISIBLE | style,
            x, y, cw, ch, hwnd, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, WPARAM(font), TRUE);
        return child;
    };
    const int m = px(12);
    add(L"STATIC", description.empty() ? L"Type the name, then press Enter." : description.c_str(), SS_LEFT, m, m, c.right - 2 * m, px(20), -1);
    box.edit = add(L"EDIT", text.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, m, m + px(26), c.right - 2 * m, px(26), 100);
    if (maxLength)
        SendMessageW(box.edit, EM_SETLIMITTEXT, maxLength, 0);
    add(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, c.right - m - px(170), c.bottom - m - px(28), px(80), px(28), IDOK);
    add(L"BUTTON", L"Cancel", WS_TABSTOP, c.right - m - px(80), c.bottom - m - px(28), px(80), px(28), IDCANCEL);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(box.edit);
    SendMessageW(box.edit, EM_SETSEL, 0, -1);
    MSG msg;
    while (!box.result && GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    if (box.result == IDOK)
    {
        const int n = GetWindowTextLengthW(box.edit);
        text.assign(size_t(n), L'\0');
        GetWindowTextW(box.edit, text.data(), n + 1);
    }
    DestroyWindow(hwnd);
    DeleteObject(font);
    if (g_p.window)
        SetForegroundWindow(g_p.window);
    return box.result == IDOK;
}

std::vector<uint8_t> PresenterExchangeImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba)
{
    if (!width || !height || rgba.size() != size_t(width) * height * 4)
        return rgba;
    {
        std::lock_guard lock(g_p.mutex);
        g_p.image.swap(rgba);
        g_p.imageWidth = width;
        g_p.imageHeight = height;
        g_p.pendingFrame++;
    }
    g_p.cv.notify_one();
    return rgba; // the image shown before this one: its storage, for the caller's next frame
}

void PresenterSubmitImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba)
{
    PresenterExchangeImage(width, height, std::move(rgba));
}

ID3D12Device* RenderDevice()
{
    EnsureDevice();
    return g_device.Get();
}

ID3D12CommandQueue* RenderQueue()
{
    EnsureDevice();
    return g_queue.Get();
}

void* PresenterClaimWindow()
{
    if (getenv("FM2_HEADLESS"))
        return nullptr;
    for (int i = 0; i < 2000 && !g_p.ready; i++) // the window is made on the presenter's thread
        Sleep(5);
    if (!g_p.ready || g_p.swapChain)
        return nullptr;
    g_p.claimed = true;
    return g_p.surface ? g_p.surface : g_p.window;
}

void PresenterReleaseWindow()
{
    g_p.claimed = false;
}

void PresenterCountFrame()
{
    g_presents++;
}

void PresenterUseRenderer()
{
    g_rendererActive = true;
}

bool PresenterRecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* overlay, uint32_t overlayW, uint32_t overlayH,
                         ID3D12Resource* toast, uint32_t toastW, uint32_t toastH, float toastRise)
{
    if (!g_p.ready || !g_p.swapChain)
        return false;
    const UINT i = g_p.swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = g_p.backBuffers[i].Get();
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backBuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    // The image is the render size; with a forced aspect ratio it is smaller than the back
    // buffer and goes in the middle, on black.
    const D3D12_RESOURCE_DESC sd = src->GetDesc(), bd = backBuffer->GetDesc();
    const UINT offsetX = UINT((bd.Width - std::min(sd.Width, bd.Width)) / 2), offsetY = (bd.Height - std::min(sd.Height, bd.Height)) / 2;
    if (offsetX || offsetY)
    {
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(i) * g_p.rtvStride;
        const float black[4] = { 0, 0, 0, 1 };
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    list->ResourceBarrier(1, &b);
    if (!offsetX && !offsetY && sd.Width == bd.Width && sd.Height == bd.Height)
        list->CopyResource(backBuffer, src);
    else
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = src;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{ 0, 0, 0, UINT(std::min(sd.Width, bd.Width)), std::min(sd.Height, bd.Height), 1 };
        list->CopyTextureRegion(&dst, offsetX, offsetY, 0, &from, &box);
    }
    if (overlay)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = overlay;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{ 0, 0, 0, overlayW, overlayH, 1 };
        list->CopyTextureRegion(&dst, offsetX + 24, offsetY + 24, 0, &from, &box);
    }
    const UINT shownW = UINT(std::min(sd.Width, bd.Width)), shownH = std::min(sd.Height, bd.Height);
    if (toast && toastW && toastH && toastW <= shownW && toastH < shownH)
    {
        // up from just below the image to a twentieth of its height above the bottom; what is still below is cut off
        const UINT bottom = offsetY + shownH, margin = shownH / 20;
        const UINT y = bottom - UINT(float(toastH + margin) * std::clamp(toastRise, 0.0f, 1.0f));
        if (y < bottom)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
            dst.pResource = backBuffer;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.pResource = toast;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{ 0, 0, 0, toastW, std::min(toastH, bottom - y), 1 };
            list->CopyTextureRegion(&dst, offsetX + (shownW - toastW) / 2, y, 0, &from, &box);
        }
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    return true;
}

void PresenterResize(uint32_t width, uint32_t height)
{
    if (!g_p.swapChain)
        return;
    for (auto& b : g_p.backBuffers)
        b.Reset();
    if (FAILED(g_p.swapChain->ResizeBuffers(kBuffers, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)))
    {
        Log("presenter: swap chain resize to %ux%u failed", width, height);
        return;
    }
    for (UINT i = 0; i < kBuffers; i++)
    {
        Check(g_p.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_p.backBuffers[i])), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * g_p.rtvStride;
        g_p.device->CreateRenderTargetView(g_p.backBuffers[i].Get(), nullptr, h);
    }
    Log("presenter: swap chain %ux%u", width, height);
    LayoutSurface(); // (fullscreen: the bars follow the new aspect ratio)
}

void PresenterSetWindowSize(uint32_t width, uint32_t height)
{
    if (!g_p.window || g_p.fullscreen || IsZoomed(g_p.window))
        return;
    const SIZE size = WindowSizeFor(width, height);
    SetWindowPos(g_p.window, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Log("presenter: window %ldx%ld", size.cx, size.cy);
}

void PresenterSetFullscreen(bool on)
{
    if (!g_p.window || g_p.fullscreen == on)
        return;
    g_p.fullscreen = on;
    if (on)
    {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(g_p.window, MONITOR_DEFAULTTONEAREST), &mi);
        if (IsZoomed(g_p.window))
            ShowWindowAsync(g_p.window, SW_RESTORE);
        SetWindowLongW(g_p.window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_p.window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
    }
    else
    {
        SetWindowLongW(g_p.window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        const SIZE size = WindowSizeFor(GetSettings().width, GetSettings().height);
        SetWindowPos(g_p.window, nullptr, 40, 40, size.cx, size.cy, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
    }
    Log("presenter: %s (live)", on ? "fullscreen" : "windowed");
}

void PresenterPresent()
{
    // Vsync: wait for the display's refresh; off: present at once (tearing allowed).
    if (g_p.ready && g_p.swapChain)
    {
        FsrFgBeforePresent();
        if (GetSettings().vsync)
            g_p.swapChain->Present(1, 0);
        else
            g_p.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        g_presents++;
    }
}

uint32_t DisplayRefreshHz()
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return mode.dmDisplayFrequency;
    return 60;
}
