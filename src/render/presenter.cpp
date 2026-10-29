// Native output: a window with a D3D12 flip-model swap chain. The guest's
// VdSwap hands a frame over; a render thread presents it without blocking
// the guest. Until the D3D hooks draw real content the frame is a clear
// whose shade pulses with the frame count, proving the path end to end.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

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
constexpr UINT kWidth = 1280, kHeight = 720;

struct Presenter
{
    HWND window = nullptr;
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
    std::vector<uint8_t> image; // latest submitted RGBA8, kWidth x kHeight
    ComPtr<ID3D12Resource> upload[kBuffers];
    uint64_t presentedFrame = 0;
    std::atomic<bool> ready{ false };
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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
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

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fatal("presenter: %s failed (%08lX)", what, hr);
}

std::mutex g_deviceMutex;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
std::atomic<bool> g_rendererActive{ false };

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
    HWND surface = nullptr;
    if (settings.fullscreen)
    {
        // Borderless window over the whole display; its black background shows around the picture.
        // The picture (the swap chain) is a child window: the largest rectangle of the chosen
        // aspect ratio, centred. Other windows cannot cover the black bars this way.
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        g_p.window = CreateWindowExW(0, L"fm2", L"Forza Motorsport 2", WS_POPUP | WS_VISIBLE, 0, 0, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);
        LONG w = sw, h = sh;
        if (int64_t(sw) * outH > int64_t(sh) * outW)
            w = LONG(int64_t(sh) * outW / outH);
        else
            h = LONG(int64_t(sw) * outH / outW);
        surface = CreateWindowExW(0, L"fm2", nullptr, WS_CHILD | WS_VISIBLE, (sw - w) / 2, (sh - h) / 2, w, h,
            g_p.window, nullptr, wc.hInstance, nullptr);
    }
    else
    {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        LONG cw = LONG(outW), ch = LONG(outH);
        const LONG maxW = (work.right - work.left) * 9 / 10, maxH = (work.bottom - work.top) * 9 / 10;
        if (cw > maxW || ch > maxH)
        {
            const double f = std::min(double(maxW) / cw, double(maxH) / ch);
            cw = LONG(cw * f);
            ch = LONG(ch * f);
        }
        RECT r{ 0, 0, cw, ch };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_p.window = CreateWindowW(L"fm2", L"Forza Motorsport 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }

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
    ComPtr<IDXGISwapChain1> sc1;
    Check(factory->CreateSwapChainForHwnd(g_p.queue.Get(), surface ? surface : g_p.window, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
    Check(sc1.As(&g_p.swapChain), "IDXGISwapChain3");
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

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = UINT64(kWidth) * kHeight * 4;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < kBuffers; i++)
        Check(g_p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                  IID_PPV_ARGS(&g_p.upload[i])),
            "CreateCommittedResource(upload)");
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

    bool copied = false;
    {
        std::lock_guard lock(g_p.mutex);
        if (g_p.image.size() == size_t(kWidth) * kHeight * 4)
        {
            void* mapped = nullptr;
            g_p.upload[i]->Map(0, nullptr, &mapped);
            memcpy(mapped, g_p.image.data(), g_p.image.size());
            g_p.upload[i]->Unmap(0, nullptr);
            copied = true;
        }
    }

    if (copied)
    {
        Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_p.upload[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, kWidth * 4 };
        g_p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
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
    g_p.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    g_p.fenceValues[i] = g_p.nextFence++;
    g_p.queue->Signal(g_p.fence.Get(), g_p.fenceValues[i]);
}

void RenderThread()
{
    SetThreadDescription(GetCurrentThread(), L"presenter");
    CreateWindowAndDevice();
    g_p.ready = true;
    Log("presenter: window and D3D12 swap chain ready");
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        uint64_t frame;
        {
            std::unique_lock lock(g_p.mutex);
            g_p.cv.wait_for(lock, std::chrono::milliseconds(16), [] { return g_p.pendingFrame != g_p.presentedFrame; });
            frame = g_p.pendingFrame;
            if (frame == g_p.presentedFrame || g_rendererActive)
                continue;
            g_p.presentedFrame = frame;
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

void PresenterSubmitImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba)
{
    if (width != kWidth || height != kHeight)
        return;
    {
        std::lock_guard lock(g_p.mutex);
        g_p.image = std::move(rgba);
        g_p.pendingFrame++;
    }
    g_p.cv.notify_one();
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

void PresenterUseRenderer()
{
    g_rendererActive = true;
}

bool PresenterRecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* overlay, uint32_t overlayW, uint32_t overlayH,
                         ID3D12Resource* toast, uint32_t toastW, uint32_t toastH, float toastRise)
{
    if (!g_p.ready)
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
        list->CopyTextureRegion(&dst, offsetX + 24, offsetY + 24, 0, &from, nullptr);
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

void PresenterPresent()
{
    // Vsync: wait for the display's refresh; off: present at once (tearing allowed).
    if (g_p.ready)
    {
        if (GetSettings().vsync)
            g_p.swapChain->Present(1, 0);
        else
            g_p.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
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
