// What the port asks of the game window, now that ReGPU-V owns it (window class "regpu").
#include <windows.h>

#include <string>

#include "regpu/regpu.h"
#include "render/presenter.h"

namespace
{
// The game window ReGPU opened (nullptr before it exists or when headless).
HWND GameWindow()
{
    static HWND cached = nullptr;
    if (cached && IsWindow(cached))
        return cached;
    struct Find { HWND hwnd = nullptr; } find;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t cls[16] = {};
        if (pid == GetCurrentProcessId() && GetClassNameW(h, cls, 16) && !wcscmp(cls, L"regpu"))
        {
            reinterpret_cast<Find*>(p)->hwnd = h;
            return FALSE;
        }
        return TRUE;
    }, LPARAM(&find));
    return cached = find.hwnd;
}
} // namespace

// (PresenterHasFocus is ReGPU-V's own: its presenter has the same function.)

// (ReGPU's window shows the cursor as Windows does; FM2's hide-after-controller-use isn't carried over.)
void PresenterControllerUsed() {}

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
    const UINT dpi = GameWindow() ? GetDpiForWindow(GameWindow()) : 96;
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    const int w = px(420), h = px(150);
    RECT r{};
    if (!GameWindow() || !GetWindowRect(GameWindow(), &r))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    TextBox box;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"fm2text", title.empty() ? L"Forza Motorsport 2" : title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, (r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2, w, h, GameWindow(), nullptr,
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
    if (GameWindow())
        SetForegroundWindow(GameWindow());
    return box.result == IDOK;
}

