// Hyper Settings (prototype, 16 Nov 2026; restyled the same day): a custom-drawn Win32 window that picks a Hyper
// preset or each cut by hand, the upscaler and the resolutions, and starts the game with them. Hovering a setting shows
// what it does and what it measured on the Vega laptop and the HD 630 (docs/hyper/INTEL-VS-AMD.md,
// HYPER-SETTINGS-GUIDE.md, HYPER-CHANGES.md).
// Nothing is written to settings.ini and no game code is involved: every choice is passed to fm2.exe as an environment
// variable (REGPU_HYPER_*, FM2_*), which win over the settings file. REGPU_HYPER is always "off" and every cut is set
// explicitly, so what the window shows is exactly what runs.
// Custom presets and the last-used state: Documents\fm2\hyper_menu.ini (one section per preset; same keys as before).
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace Gdiplus
{
using std::max;
using std::min;
}
#include <gdiplus.h>

namespace
{
// ---------------------------------------------------------------------------------------------------------------------
// Settings and what is known about them
// ---------------------------------------------------------------------------------------------------------------------
enum Kind { Check, Combo, Radio };
enum Group { Rend, Surf, Disp, Geom, Shad, Expt, Foot };
struct Item
{
    const wchar_t* key;     // state key, also the ini key
    Kind kind;
    Group group;
    const wchar_t* label;
    const wchar_t* choices; // Combo/Radio: labels, '|' separated
    const wchar_t* values;  // Combo/Radio: the value of each choice
    const wchar_t* def;     // the stock value (Hyper off)
    const wchar_t* tip;     // what it does and what you see
    const wchar_t* perf;    // measured rows "value|choice|laptop|HD 630", ';' separated; a cell starts with g/b/n (tone)
    const wchar_t* note;    // under the table
    const wchar_t* caption; // the table's caption (default: alone vs stock)
    const wchar_t* parent;  // a sub-setting: the key it depends on
};
const Item kItems[] = {
    // Rendering
    { L"upscaler", Radio, Rend, L"Upscaler", L"Normal|FSR 1|FSR 3", L"off|fsr1|fsr3", L"off",
      L"How the frame reaches the screen. Normal draws at the output size. FSR 1 draws smaller and sharpens it up to the "
      L"output. FSR 3 is temporal (it reuses earlier frames) and still experimental here.",
      L"fsr1|FSR 1, 80% \u2192 900p|b\u221216%|b\u221222%;fsr3|FSR 3, 80% \u2192 900p|b\u221256%|b\u221259%",
      L"Potato upscaled to 1600\u00d7900 from 80%, against Potato drawn at plain 1280\u00d7720: a bigger, sharper picture "
      L"bought with frame rate.", L"Cost vs Potato at 720p", nullptr },
    { L"resolution", Combo, Rend, L"Output resolution", L"1280\u00d7720|1600\u00d7900|1920\u00d71080|2560\u00d71440|3840\u00d72160",
      L"1280x720|1600x900|1920x1080|2560x1440|3840x2160", L"1280x720",
      L"The size of the picture on screen. Normal draws at this size; with an upscaler the game draws smaller and scales "
      L"up to it.", nullptr, L"On the HD 630, output above 720p cost 20\u201332% even from a 720p scene.", nullptr, nullptr },
    { L"scale", Combo, Rend, L"Render scale", L"100%|90%|80%|75%|70%|67%|60%|50%", L"100|90|80|75|70|67|60|50", L"100",
      L"With FSR 1 or FSR 3: how much of the output the game draws, per side. 75% draws about 56% of the pixels. The "
      L"game's lowest is 50%.",
      L"90|90%|nnot measured|b\u22124.6%;80|80%|nnot measured|g+5.5%;70|70%|nnot measured|g+16.4%",
      L"Culling's threshold is in drawn pixels, so a lower scale also culls more (the crowd went at 50%).",
      L"FSR 1 vs native 720p", L"upscaler" },
    { L"sharpness", Combo, Rend, L"Sharpening", L"0.0|0.1|0.2|0.3|0.4|0.5|0.6|0.8|1.0", L"0.0|0.1|0.2|0.3|0.4|0.5|0.6|0.8|1.0",
      L"0.3", L"FSR 1's edge sharpening strength. Higher looks crisper and can leave halos around edges.", nullptr,
      L"Part of the FSR 1 pass: no separate cost measured.", nullptr, L"upscaler" },
    { L"cheap_upscale", Check, Rend, L"One-pass bilinear", nullptr, nullptr, L"0",
      L"With FSR 1: replaces its three full-screen passes with one bilinear scale. Blurrier, cheaper.",
      L"1|On|gsaves 1.8 ms|nnot measured", L"FSR 1's passes measured about 1.8 ms a frame in "
      L"a GPU capture on the laptop.", L"GPU time saved per frame", L"upscaler" },
    { L"fsr_version", Combo, Rend, L"FSR 3 version", L"Default|Version 0|Version 1|Version 2", L"|0|1|2", L"",
      L"Which upscaler version inside the FidelityFX library FSR 3 uses. Default lets the library choose.", nullptr,
      nullptr, nullptr, L"upscaler" },

    // Surfaces
    { L"helpers", Radio, Surf, L"Helper size", L"100%|75%|50%|25%", L"100|75|50|25", L"100",
      L"Offscreen surfaces narrower than half the screen (reflections, bloom and blur levels) drawn at this size per "
      L"side; the screen keeps its resolution. Reflections in paint and glass get blurrier and the glow softer.",
      L"75|75%|nnot measured|nnot measured;50|50%|g+17.2%|g+12.8%;25|25%|g+20.9%|g+18.8%", nullptr, nullptr, nullptr },
    { L"aniso", Radio, Surf, L"Anisotropy", L"16\u00d7|8\u00d7|4\u00d7|2\u00d7|Off", L"16|8|4|2|0", L"16",
      L"Caps texture filtering at shallow angles. Lower blurs the road, kerbs and markings sooner in the distance.",
      L"8|8\u00d7|gsaves 0.7 ms|nnot measured;4|4\u00d7|gsaves 1.6 ms|nnot measured;2|2\u00d7|g+7.9%|n+1.3%;0|Off|g+9.0%|n+2.7%",
      L"8\u00d7 and 4\u00d7: GPU time saved per frame on the laptop. The HD 630 is limited by shader maths, so filtering "
      L"barely matters there.", nullptr, nullptr },
    { L"resolve1010", Check, Surf, L"32-bit 10:10:10:2 copies", nullptr, nullptr, L"0",
      L"Copies into the 10:10:10:2 colour format stay 32-bit instead of 64-bit float: half the memory traffic. No "
      L"visible change.", L"1|On|n+2.6%|n+1.3%", L"Both within run-to-run noise (3%).", nullptr, nullptr },
    { L"resolve_by_size", Check, Surf, L"Resolve by size", nullptr, nullptr, L"0",
      L"Keeps a copy of a resolved surface for each size it is read at, so a texture read at another size gets a "
      L"matching copy. Lossless; it fixed the flicker of frozen shadow maps.", L"1|On|nwithin noise|nwithin noise",
      nullptr, nullptr, nullptr },
    { L"freeze", Check, Surf, L"Freeze helper views", nullptr, nullptr, L"0",
      L"3D helper views (reflections, environment, lighting) are drawn normally for 120 frames, then only every few "
      L"frames; in between they keep their last picture. Reflections lag slightly behind the car.",
      L"1|On|g+4.2%|g+12.8%", nullptr, nullptr, nullptr },
    { L"freeze_refresh", Combo, Surf, L"Redraw every", L"2 frames|3 frames|4 frames|6 frames", L"2|3|4|6", L"3",
      L"How often a frozen helper view is redrawn. Odd values keep double-buffered views alternating.", nullptr,
      L"4, 5 or 6 gained nothing measurable over 3 on the HD 630.", nullptr, L"freeze" },

    // Display
    { L"fullscreen", Check, Disp, L"Fullscreen", nullptr, nullptr, L"0", L"Fullscreen at the output resolution instead of a window.",
      nullptr, nullptr, nullptr, nullptr },
    { L"vsync", Check, Disp, L"VSync", nullptr, nullptr, L"0",
      L"Waits for the display's refresh: no tearing, but the frame rate drops to a fraction of the refresh rate when "
      L"the game can't keep up.", nullptr, nullptr, nullptr, nullptr },
    { L"max_fps", Combo, Disp, L"Frame cap", L"30|60|120|Uncapped", L"30|60|120|360", L"60",
      L"Caps the frame rate. Benchmarks here run uncapped; a cap keeps the laptop cooler and the pacing steadier.",
      nullptr, nullptr, nullptr, nullptr },
    { L"fps_overlay", Check, Disp, L"FPS overlay", nullptr, nullptr, L"1", L"Shows the frame-rate meter in a corner of the game.",
      nullptr, nullptr, nullptr, nullptr },

    // Geometry
    { L"cull_pixels", Combo, Geom, L"Cull small draws", L"Off|Hidden only|\u2264 64 px|\u2264 128 px|\u2264 256 px",
      L"-1|0|64|128|256", L"-1",
      L"Counts the pixels each draw covers and skips draws at or under this size. Tiny distant things (crowd pieces, "
      L"small signs) disappear. Potato uses 256.",
      L"0|Hidden only|g+8.4%|b\u22127.4%;64|\u2264 64 px|g+12.3%|b\u22122.0%;128|\u2264 128 px|nnot measured|nnot measured;"
      L"256|\u2264 256 px|g+17.2%|g+6.7%",
      L"Hidden only and 64 px were measured without the probe and hysteresis, 256 px with both. Counting pixels is "
      L"costly on the HD 630, so only the probe + hysteresis version pays there.", nullptr, nullptr },
    { L"cull_all", Check, Geom, L"Blended draws too", nullptr, nullptr, L"0",
      L"See-through (blended) draws can be culled as well, not only opaque ones.", nullptr, nullptr, nullptr, L"cull_pixels" },
    { L"cull_passes", Check, Geom, L"Every pass", nullptr, nullptr, L"0",
      L"Culling also works in helper and shadow passes, not only the main view. Together with 25% helper surfaces this "
      L"caused the blinking car highlight that the blink fix below removes.", nullptr, nullptr, nullptr, L"cull_pixels" },
    { L"cull_probe", Check, Geom, L"Invisible re-measure", nullptr, nullptr, L"0",
      L"A culled draw is re-measured every 4th frame with an invisible draw (no colour, no depth), so it never blinks in "
      L"for one frame. Keep it on with culling.", L"1|On|nsame speed|nsame speed",
      L"HD 630: 44.9 vs 44.9 fps; it removed the 1-in-4-frame blink.", nullptr, L"cull_pixels" },
    { L"cull_hyst", Check, Geom, L"Hysteresis", nullptr, nullptr, L"0",
      L"A culled draw has to grow to twice the threshold before it comes back, so objects near the limit don't flip on "
      L"and off.", nullptr, L"Clean by eye at 200% (used here) and at 150%.", nullptr, L"cull_pixels" },
    { L"cull_pin", Check, Geom, L"Blink fix", nullptr, nullptr, L"1",
      L"A draw that returns from being culled twice within 64 frames is never culled again. Fixes the white stripe that "
      L"blinked on the car with Potato.", L"1|On|nno cost|nnot measured",
      L"Potato with and without it: 74.0\u201375.7 vs 73.8\u201375.3 fps over 3 rounds; 15 draws get pinned on the grid.",
      nullptr, L"cull_pixels" },
    { L"decimate", Combo, Geom, L"Simplify big meshes", L"Off|Keep 90%|Keep 75%|Keep 50%|Keep 25%|Keep 10%",
      L"0|90|75|50|25|10", L"0",
      L"Big meshes are drawn with fewer triangles, simplified in the background with their borders locked. Stands and "
      L"buildings look slightly angular, with a one-time pop the first time each is seen.",
      L"25|Keep 25%|nnot measured|nnot measured;10|Keep 10%|g+4.6%|n+2.7%", L"Keeping 5% was within noise on the HD 630.",
      nullptr, nullptr },
    { L"vcache", Check, Geom, L"Triangle reorder", nullptr, nullptr, L"0",
      L"Reorders the simplified meshes' triangles for the vertex cache and for less overdraw. Same triangles, no "
      L"visible change. Runs in the simplifier, so it needs mesh simplification on.", L"1|On|nnot measured|g+4.5%",
      L"The HD 630 figure was measured in its stack, not alone.", nullptr, L"decimate" },

    // Shaders
    { L"nodiscard", Check, Shad, L"Skip unused discards", nullptr, nullptr, L"0",
      L"Draws without an alpha test use a pixel shader without its discard, so the GPU can reject hidden pixels early. "
      L"No visible change.", L"1|On|n+2.1%|g+12.8%", nullptr, nullptr, nullptr },
    { L"shader_opt", Check, Shad, L"Optimise shaders", nullptr, nullptr, L"0",
      L"Compiles the translated shaders with optimisation on. No visible change; the first launch after switching "
      L"compiles slowly.", L"1|On|n+2.6%|n+1.3%",
      L"On the HD 630 it cut 6.4 ms of GPU time (10%) though the frame rate barely moved.", nullptr, nullptr },
    { L"plain_mul", Check, Shad, L"Plain multiply", nullptr, nullptr, L"0",
      L"Uses plain multiplies wherever no value can be infinite or NaN, instead of emulating the Xbox 360's "
      L"0 \u00d7 anything = 0 everywhere. No visible change in FM2.", L"1|On|g+8.8%|g+13.4%", nullptr, nullptr, nullptr },
    { L"fetchspec", Check, Shad, L"Fetch specialisation", nullptr, nullptr, L"0",
      L"Each draw gets a pixel-shader variant with its textures' sampling modes built in. No visible change; the first "
      L"race on a track can stutter briefly while the variants compile.", L"1|On|g+7.4%|g+14.8%", nullptr, nullptr, nullptr },
    { L"tailspec", Check, Shad, L"Tail specialisation", nullptr, nullptr, L"0",
      L"Each draw's alpha test and output settings are built into its pixel shader as constants. No visible change.",
      L"1|On|nwithin noise|nwithin noise", nullptr, nullptr, nullptr },
    { L"swizzle_sel", Check, Shad, L"Swizzle selects", nullptr, nullptr, L"0",
      L"Texture swizzles become simple selects instead of indexed lookups. No visible change. Turning it off again "
      L"recompiles every shader, which can take minutes.", L"1|On|n+1.2%|g+6.0%", nullptr, nullptr, nullptr },
    { L"lean_lod", Check, Shad, L"Lean LOD", nullptr, nullptr, L"0",
      L"Drops the forced-LOD test from every texture fetch. No visible change.", L"1|On|n+1.2%|n+1.3%",
      L"HD 630: 3.5 ms less GPU time a frame.", nullptr, nullptr },
    { L"lean_vfetch", Check, Expt, L"Lean vertex fetch", nullptr, nullptr, L"0",
      L"Leaner vertex-fetch code in the vertex shaders. No visible change.", L"1|On|nwithin noise|nwithin noise", nullptr,
      nullptr, nullptr },
    { L"a2m_test", Check, Expt, L"Alpha-to-mask as alpha test", nullptr, nullptr, L"0",
      L"Alpha-to-coverage draws use a plain alpha test instead. It removes content: the crowd, canopy fabric and "
      L"chain-link fences vanish. For experiments only.", L"1|On|nwithin noise|nwithin noise", nullptr, nullptr, nullptr },
    { L"lean_fetch", Radio, Expt, L"Lean texture fetch", L"Off|lv1|lv2|lv3", L"0|1|2|3", L"0",
      L"The laptop's texture-fetch shortcut, in levels: lv1 skips checks a draw's textures don't need, lv2 builds each draw's texture "
      L"state in, lv3 adds plain multiplies on 3D draws. Potato leaves it off in favour of fetch specialisation.",
      L"1|lv1|g+6%|nnot measured;2|lv2|g+8.4%|g+26.2%;3|lv3|g+9.0%|g+49.0%",
      L"lv1 was measured on an older base. On top of the other shader cuts, lv3 still added about 10% on the HD 630 and "
      L"2% on the laptop.", nullptr, nullptr },

    // Footer
    // Who drives in Test Race. Ini values stay compatible: 0 and 1 were the old Self-driving switch.
    { L"autodrive", Radio, Foot, L"Driver", L"You|Bot|AI", L"0|1|ai", L"0",
      L"Who drives the car in Test Race. You: your controller. Bot: our lane keeper, which steers from the picture "
      L"(Nissan Speedway oval only) and drives nearly the same line every run, for development. AI: the game's own "
      L"racing driver takes your car, as Career's Hire Driver does, on any track; the realistic choice for "
      L"benchmarking how the game plays.",
      L"1|Bot|n81–83 fps|nnot measured;ai|AI|n88–90 fps|nnot measured",
      L"Both repeat within 2.5% between runs. They drive different lines, so compare runs made with the same driver "
      L"only. AI and Bot apply to Test Race; Start game boots normally.", L"Potato, 720p, 40 s of racing", nullptr },
};
constexpr int kItemCount = int(sizeof(kItems) / sizeof(kItems[0]));

// The built-in presets, as hyper.cpp applies them (Potato: the shader-cut set, the default on Intel and AMD). They set
// the Hyper cuts and the upscaler; the output resolution, sharpening and the display options stay as they are.
struct Preset { const wchar_t* name; const wchar_t* set; const wchar_t* blurb; };
const Preset kPresets[] = {
    { L"Off (stock)", L"upscaler=off;scale=100", L"" },
    // Lossless: only the cuts measured with no visible change (HYPER-SETTINGS-GUIDE.md, "Free speed"), at native
    // resolution. Triangle reorder is lossless too but runs inside mesh simplification, which is not, so it stays off.
    { L"Lossless", L"upscaler=off;scale=100;resolve1010=1;resolve_by_size=1;nodiscard=1;shader_opt=1;plain_mul=1;fetchspec=1;"
                   L"tailspec=1;swizzle_sel=1;lean_lod=1",
      L"Only the settings measured with no visible change; the picture matches stock." },
    // Turbo: the user's own preset (16 Nov 2026), made built-in. Potato's cuts at native resolution, but culling only
    // draws that were entirely hidden. (Lean texture and vertex fetch, in its first version, moved to Experimental.)
    { L"Turbo", L"upscaler=off;scale=100;aniso=0;helpers=25;resolve1010=1;resolve_by_size=1;nodiscard=1;decimate=10;vcache=1;"
                L"cull_pixels=0;cull_all=1;cull_passes=1;cull_probe=1;cull_hyst=1;freeze=1;freeze_refresh=3;"
                L"shader_opt=1;plain_mul=1;fetchspec=1;tailspec=1;swizzle_sel=1;lean_lod=1",
      L"Potato's cuts, culling only fully hidden draws." },
    { L"Potato", L"upscaler=off;scale=100;aniso=0;helpers=25;resolve1010=1;resolve_by_size=1;nodiscard=1;decimate=10;vcache=1;"
                 L"cull_pixels=256;cull_all=1;cull_passes=1;cull_probe=1;cull_hyst=1;freeze=1;freeze_refresh=4;"
                 L"shader_opt=1;plain_mul=1;fetchspec=1;tailspec=1;swizzle_sel=1;lean_lod=1",
      L"Every cut, at native resolution." },
};
int PresetIndex(const wchar_t* name)
{
    for (int i = 0; i < int(sizeof(kPresets) / sizeof(kPresets[0])); i++)
        if (!wcscmp(kPresets[i].name, name)) return i;
    return -1;
}
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));
const wchar_t* const kCards[] = { L"Rendering", L"Surfaces", L"Display", L"Geometry", L"Shaders", L"Experimental" };
const wchar_t* const kCardNotes[] = { L"Output size and upscaling", L"Each one softens one thing you can name",
                                      L"Window and frame pacing", L"Fewer draws and triangles",
                                      L"Lossless; changing one recompiles shaders once",
                                      L"In no preset; for testing" };

bool PresetOwns(const Item& it)
{
    return it.group != Disp && it.group != Foot && wcscmp(it.key, L"resolution") && wcscmp(it.key, L"sharpness") &&
           wcscmp(it.key, L"fsr_version");
}
bool IsCut(const Item& it) { return it.group == Surf || it.group == Geom || it.group == Shad || it.group == Expt; }

std::vector<std::wstring> Split(const wchar_t* s, wchar_t sep = L'|')
{
    std::vector<std::wstring> out(1);
    for (; s && *s; s++)
        if (*s == sep) out.emplace_back(); else out.back() += *s;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------------------------------------------------
// Graphite with an amber accent: a pit-wall telemetry screen. COLORREF is 0x00BBGGRR.
constexpr COLORREF kBg = RGB(17, 19, 22), kChrome = RGB(11, 13, 16), kCard = RGB(24, 27, 32), kLine = RGB(36, 40, 47),
                   kRowHover = RGB(31, 35, 41), kField = RGB(34, 38, 44), kFieldLine = RGB(48, 53, 61),
                   kFieldHover = RGB(44, 49, 57), kText = RGB(233, 236, 239), kText2 = RGB(167, 175, 184),
                   kMuted = RGB(110, 119, 129), kAccent = RGB(255, 165, 58), kAccentDim = RGB(150, 104, 46),
                   kInk = RGB(26, 18, 6), kTrackOff = RGB(43, 48, 56), kKnobOff = RGB(150, 158, 168),
                   kGood = RGB(99, 210, 151), kBad = RGB(240, 122, 106), kTip = RGB(28, 32, 38);

COLORREF Mix(COLORREF a, COLORREF b, float t)
{
    auto m = [&](int x, int y) { return int(std::lround(x + (y - x) * t)); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}
Gdiplus::Color C(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

float g_s = 1.0f; // DPI scale
int P(float v) { return int(std::lround(v * g_s)); }

struct Fonts
{
    HFONT body = nullptr, bodyBold = nullptr, minor = nullptr, caps = nullptr, title = nullptr, titleLight = nullptr,
          tipTitle = nullptr, tipBody = nullptr, button = nullptr;
} g_f;
std::wstring g_textFace = L"Segoe UI", g_displayFace = L"Segoe UI";

bool FontExists(const wchar_t* face)
{
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *reinterpret_cast<bool*>(p) = true; return 0; },
                        LPARAM(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}
HFONT MakeFont(const std::wstring& face, float px, int weight)
{
    LOGFONTW lf{};
    lf.lfHeight = -P(px);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face.c_str());
    return CreateFontIndirectW(&lf);
}
void MakeFonts()
{
    for (HFONT f : { g_f.body, g_f.bodyBold, g_f.minor, g_f.caps, g_f.title, g_f.titleLight, g_f.tipTitle, g_f.tipBody, g_f.button })
        if (f) DeleteObject(f);
    g_f.body = MakeFont(g_textFace, 13.5f, FW_NORMAL);
    g_f.bodyBold = MakeFont(g_textFace, 13.5f, FW_SEMIBOLD);
    g_f.minor = MakeFont(g_textFace, 12.0f, FW_NORMAL);
    g_f.caps = MakeFont(g_textFace, 11.0f, FW_SEMIBOLD);
    g_f.title = MakeFont(g_displayFace, 21.0f, FW_BOLD);
    g_f.titleLight = MakeFont(g_displayFace, 21.0f, FW_LIGHT);
    g_f.tipTitle = MakeFont(g_displayFace, 15.5f, FW_SEMIBOLD);
    g_f.tipBody = MakeFont(g_textFace, 13.0f, FW_NORMAL);
    g_f.button = MakeFont(g_textFace, 13.5f, FW_SEMIBOLD);
}
int TextWidth(HFONT f, const std::wstring& s)
{
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &sz);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return sz.cx;
}
int TextHeight(HFONT f, const std::wstring& s, int width)
{
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{ 0, 0, width, 0 };
    DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return r.bottom;
}

// Drawing: shapes with GDI+ (anti-aliased), text with GDI through the same surface (ClearType).
void RoundPath(Gdiplus::GraphicsPath& p, const RECT& r, float rad)
{
    const float x = float(r.left), y = float(r.top), w = float(r.right - r.left), h = float(r.bottom - r.top);
    const float d = std::min({ rad * 2, w, h });
    if (d <= 0.5f)
    {
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
void Fill(Gdiplus::Graphics& g, const RECT& r, COLORREF c, float rad = 0)
{
    Gdiplus::SolidBrush b(C(c));
    if (rad <= 0)
    {
        g.FillRectangle(&b, Gdiplus::Rect(r.left, r.top, r.right - r.left, r.bottom - r.top));
        return;
    }
    Gdiplus::GraphicsPath p;
    RoundPath(p, r, rad);
    g.FillPath(&b, &p);
}
void Stroke(Gdiplus::Graphics& g, const RECT& r, COLORREF c, float rad, float width = 1.0f)
{
    Gdiplus::Pen pen(C(c), width * g_s);
    Gdiplus::GraphicsPath p;
    RECT in = r;
    in.right -= 1;
    in.bottom -= 1;
    RoundPath(p, in, rad);
    g.DrawPath(&pen, &p);
}
void Text(Gdiplus::Graphics& g, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt, int spacing = 0)
{
    HDC dc = g.GetHDC();
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HGDIOBJ old = SelectObject(dc, f);
    if (spacing) SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
    if (spacing) SetTextCharacterExtra(dc, 0);
    SelectObject(dc, old);
    g.ReleaseHDC(dc);
}
void Chevron(Gdiplus::Graphics& g, int cx, int cy, COLORREF c)
{
    Gdiplus::Pen pen(C(c), 1.6f * g_s);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float s = 4.0f * g_s;
    Gdiplus::PointF pts[3] = { { cx - s, cy - s / 2 }, { float(cx), cy + s / 2 }, { cx + s, cy - s / 2 } };
    g.DrawLines(&pen, pts, 3);
}

// ---------------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------------
HWND g_wnd, g_tip, g_name;
HBRUSH g_fieldBrush;
std::map<std::wstring, std::wstring> g_state, g_customSnapshot;
std::wstring g_loadedCustom, g_ini, g_root, g_status;
bool g_deleteArmed = false, g_nameFocus = false;

const Item* Find(const wchar_t* key)
{
    for (const Item& it : kItems)
        if (!wcscmp(it.key, key)) return &it;
    return nullptr;
}
std::wstring ChoiceLabel(const Item& it)
{
    const auto labels = Split(it.choices), values = Split(it.values);
    for (size_t i = 0; i < values.size(); i++)
        if (values[i] == g_state[it.key]) return labels[i];
    return labels[0];
}
bool Enabled(const Item& it)
{
    const std::wstring& up = g_state[L"upscaler"];
    if (!wcscmp(it.key, L"scale") || !wcscmp(it.key, L"sharpness")) return up != L"off";
    if (!wcscmp(it.key, L"cheap_upscale")) return up == L"fsr1";
    if (!wcscmp(it.key, L"fsr_version")) return up == L"fsr3";
    if (it.parent && !wcscmp(it.parent, L"cull_pixels")) return g_state[L"cull_pixels"] != L"-1";
    if (it.parent && !wcscmp(it.parent, L"freeze")) return g_state[L"freeze"] == L"1";
    if (it.parent && !wcscmp(it.parent, L"decimate")) return g_state[L"decimate"] != L"0";
    return true;
}
std::map<std::wstring, std::wstring> PresetState(int i)
{
    std::map<std::wstring, std::wstring> s;
    for (const Item& it : kItems)
        if (PresetOwns(it)) s[it.key] = it.def;
    for (const std::wstring& kv : Split(kPresets[i].set, L';'))
        if (const size_t eq = kv.find(L'='); eq != std::wstring::npos) s[kv.substr(0, eq)] = kv.substr(eq + 1);
    return s;
}
// The built-in preset the current state is, comparing the keys a preset owns (cutsOnly: only the Hyper cuts).
int MatchPreset(bool cutsOnly)
{
    for (int i = 0; i < kPresetCount; i++)
    {
        const auto s = PresetState(i);
        bool same = true;
        for (const Item& it : kItems)
            if (PresetOwns(it) && (!cutsOnly || IsCut(it)) && s.at(it.key) != g_state[it.key]) same = false;
        if (same) return i;
    }
    return -1;
}
std::wstring PresetLabel()
{
    if (!g_loadedCustom.empty())
        return g_state == g_customSnapshot ? g_loadedCustom : g_loadedCustom + L" (edited)";
    const int i = MatchPreset(false);
    return i >= 0 ? kPresets[i].name : L"Custom";
}

// The environment the game is started with: name, value (unset = remove the variable).
struct Var { std::wstring name, value; bool unset = false; };
std::vector<Var> BuildEnv()
{
    auto& s = g_state;
    std::vector<Var> e;
    auto set = [&](const wchar_t* n, const std::wstring& v) { e.push_back({ n, v }); };
    auto present = [&](const wchar_t* n, bool on) { e.push_back({ n, L"1", !on }); }; // switches tested for presence
    set(L"FM2_NO_LAUNCHER", L"1");
    set(L"REGPU_HYPER", L"off");
    set(L"REGPU_HYPER_SCENE", L"0");
    set(L"REGPU_HYPER_HELPERS", s[L"helpers"]);
    set(L"FM2_ANISOTROPY", L"16");
    set(L"REGPU_HYPER_ANISO", s[L"aniso"]);
    set(L"REGPU_HYPER_RESOLVE1010", s[L"resolve1010"]);
    set(L"REGPU_HYPER_RESOLVE_BY_SIZE", s[L"resolve_by_size"]);
    set(L"REGPU_HYPER_FREEZE", s[L"freeze"] == L"1" ? L"120" : L"0");
    set(L"REGPU_HYPER_FREEZE_REFRESH", s[L"freeze_refresh"]);
    set(L"REGPU_HYPER_CULL_PIXELS", s[L"cull_pixels"]);
    set(L"REGPU_HYPER_CULL_ALL", s[L"cull_all"]);
    present(L"REGPU_HYPER_CULL_PASSES", s[L"cull_passes"] == L"1");
    set(L"REGPU_HYPER_CULL_PROBE", s[L"cull_probe"]);
    set(L"REGPU_HYPER_CULL_HYST", s[L"cull_hyst"] == L"1" ? L"200" : L"100");
    set(L"REGPU_HYPER_CULL_PIN", s[L"cull_pin"]);
    set(L"REGPU_HYPER_DECIMATE", s[L"decimate"]);
    set(L"REGPU_HYPER_VCACHE", s[L"vcache"] == L"1" ? L"2" : L"0");
    set(L"REGPU_HYPER_NODISCARD", s[L"nodiscard"]);
    set(L"REGPU_HYPER_SHADER_OPT", s[L"shader_opt"] == L"1" ? L"1" : L"-1");
    set(L"REGPU_HYPER_PLAIN_MUL", s[L"plain_mul"] == L"1" ? L"5" : L"0");
    set(L"REGPU_HYPER_FETCHSPEC", s[L"fetchspec"]);
    set(L"REGPU_HYPER_TAILSPEC", s[L"tailspec"]);
    set(L"REGPU_HYPER_SWIZZLE_SEL", s[L"swizzle_sel"]);
    set(L"REGPU_HYPER_LEAN_LOD", s[L"lean_lod"]);
    set(L"REGPU_HYPER_LEAN_VFETCH", s[L"lean_vfetch"]);
    set(L"REGPU_HYPER_LEAN_FETCH", s[L"lean_fetch"]);
    present(L"REGPU_HYPER_A2M_TEST", s[L"a2m_test"] == L"1");
    const std::wstring& up = s[L"upscaler"];
    set(L"FM2_RESOLUTION", s[L"resolution"]);
    set(L"FM2_UPSCALER", up);
    present(L"FM2_TEMPORAL_UPSCALERS", up == L"fsr3");
    set(L"FM2_FSR_SCALE", up == L"fsr1" ? s[L"scale"] : L"100");
    e.push_back({ L"FM2_TEMPORAL_SCALE", s[L"scale"], up != L"fsr3" });
    e.push_back({ L"FM2_FSR_VERSION", s[L"fsr_version"], up != L"fsr3" || s[L"fsr_version"].empty() });
    set(L"REGPU_HYPER_UPSCALE", up == L"fsr1" ? s[L"cheap_upscale"] : L"0");
    set(L"FM2_SHARPNESS", s[L"sharpness"]);
    set(L"FM2_FULLSCREEN", s[L"fullscreen"]);
    set(L"FM2_VSYNC", s[L"vsync"]);
    set(L"FM2_MAX_FPS", s[L"max_fps"]);
    present(L"REGPU_FPS_OVERLAY", s[L"fps_overlay"] == L"1");
    present(L"FM2_FPS_OVERLAY", false);
    present(L"FM2_AUTODRIVE", s[L"autodrive"] == L"1");
    present(L"FM2_RACENOW_AI", false); // set by the race-now payload (ai=1) instead
    return e;
}

// Test Race's command line: our bot (drive=1) or the game's AI driver (ai=1), see Driver.
std::wstring RaceArgs()
{
    const std::wstring& d = g_state[L"autodrive"];
    return std::wstring(L" --racenow drive=") + (d == L"1" ? L"1" : L"0") + (d == L"ai" ? L",ai=1" : L"") + L",fps=0";
}
std::wstring RenderInfo()
{
    unsigned w = 1280, h = 720;
    swscanf(g_state[L"resolution"].c_str(), L"%ux%u", &w, &h);
    const std::wstring& up = g_state[L"upscaler"];
    const unsigned pct = up == L"off" ? 100u : unsigned(_wtoi(g_state[L"scale"].c_str()));
    wchar_t s[128];
    if (up == L"off")
        swprintf(s, 128, L"Draws and shows %u\u00d7%u", w, h);
    else
        swprintf(s, 128, L"Draws %u\u00d7%u  \u2192  shows %u\u00d7%u", w * pct / 100, h * pct / 100, w, h);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------------------------------
enum Act { ActNone, ActPreset, ActSave, ActDelete, ActCopy, ActStartGame, ActStartRace };
struct Row { int item; RECT r, ctl; std::vector<RECT> segs; };
struct Card { int index; RECT r; std::vector<int> items; };
struct Button { Act act; RECT r; };
std::vector<Row> g_rows;
std::vector<Card> g_cards;
std::vector<Button> g_buttons;
RECT g_nameBox, g_header, g_footer;
int g_width = 0, g_height = 0;

// Hover state
int g_hoverRow = -1, g_hoverSeg = -1, g_tipRow = -1;
Act g_hoverAct = ActNone, g_pressAct = ActNone;

void Layout()
{
    g_rows.clear();
    g_cards.clear();
    g_buttons.clear();
    const int margin = P(24), gap = P(16), colW = P(318), rowH = P(36), headerH = P(56), footerH = P(56);
    g_width = margin * 2 + colW * 4 + gap * 3;
    g_header = { 0, 0, g_width, headerH };

    // Header controls, right to left.
    int x = g_width - margin;
    const int ctlH = P(32), ctlY = (headerH - ctlH) / 2;
    auto take = [&](int w) { RECT r{ x - w, ctlY, x, ctlY + ctlH }; x -= w + P(8); return r; };
    g_buttons.push_back({ ActDelete, take(P(86)) });
    g_buttons.push_back({ ActSave, take(P(86)) });
    g_nameBox = take(P(200));
    x -= P(14);
    g_buttons.push_back({ ActPreset, take(P(230)) });


    // Cards in four columns.
    // Balanced columns (heights 510 / 474 / 356 / 320 at 100%), then each column's last card stretched to one bottom
    // edge so the grid ends in a straight line.
    const std::vector<std::vector<int>> columns = { { Rend, Disp }, { Surf, Expt }, { Geom }, { Shad } };
    const int top = headerH + margin;
    int bottom = top;
    for (size_t c = 0; c < columns.size(); c++)
    {
        const int left = margin + int(c) * (colW + gap);
        int y = top;
        for (int group : columns[c])
        {
            Card card{ group, { left, y, left + colW, 0 }, {} };
            int ry = y + P(58);
            for (int i = 0; i < kItemCount; i++)
            {
                const Item& it = kItems[i];
                if (it.group != group) continue;
                Row row{ i, { left + P(8), ry, left + colW - P(8), ry + rowH }, {}, {} };
                const int right = row.r.right - P(10), cy = (row.r.top + row.r.bottom) / 2;
                if (it.kind == Check)
                    row.ctl = { right - P(36), cy - P(10), right, cy + P(10) };
                else if (it.kind == Combo)
                    row.ctl = { right - P(138), cy - P(14), right, cy + P(14) };
                else
                {
                    const auto labels = Split(it.choices);
                    int segW = 0;
                    for (const auto& l : labels) segW = std::max(segW, TextWidth(g_f.bodyBold, l) + P(14));
                    segW = std::max(segW, P(32));
                    const int w = segW * int(labels.size()) + P(4);
                    row.ctl = { right - w, cy - P(14), right, cy + P(14) };
                    for (size_t k = 0; k < labels.size(); k++)
                        row.segs.push_back({ row.ctl.left + P(2) + int(k) * segW, row.ctl.top + P(2), row.ctl.left + P(2) + int(k + 1) * segW, row.ctl.bottom - P(2) });
                }
                card.items.push_back(int(g_rows.size()));
                g_rows.push_back(row);
                ry += rowH;
            }
            card.r.bottom = ry + P(12);
            y = card.r.bottom + gap;
            g_cards.push_back(card);
        }
        bottom = std::max(bottom, y - gap);
    }
    for (size_t c = 0; c < columns.size(); c++)
        for (int k = int(g_cards.size()) - 1; k >= 0; k--)
            if (g_cards[k].r.left == margin + int(c) * (colW + gap))
            {
                g_cards[k].r.bottom = bottom;
                break;
            }
    g_footer = { 0, bottom + margin, g_width, bottom + margin + footerH };
    g_height = g_footer.bottom;

    // Footer: Copy on the left; Self-driving and the two start buttons on the right.
    const int bh = P(34), fy = g_footer.top + (footerH - bh) / 2;
    g_buttons.push_back({ ActStartGame, { g_width - margin - P(158), fy, g_width - margin, fy + bh } });
    g_buttons.push_back({ ActStartRace, { g_width - margin - P(158) - P(10) - P(132), fy, g_width - margin - P(168), fy + bh } });
    g_buttons.push_back({ ActCopy, { margin, fy, margin + P(150), fy + bh } });
    for (int i = 0; i < kItemCount; i++)
        if (kItems[i].group == Foot)
        {
            const int right = g_width - margin - P(158) - P(10) - P(132) - P(28);
            const int cy = fy + bh / 2;
            Row row{ i, {}, {}, {} };
            int ctlW = P(36);
            if (kItems[i].kind == Radio)
            {
                const auto labels = Split(kItems[i].choices);
                int segW = P(32);
                for (const auto& l : labels) segW = std::max(segW, TextWidth(g_f.bodyBold, l) + P(18));
                ctlW = segW * int(labels.size()) + P(4);
                row.ctl = { right - ctlW, cy - P(14), right, cy + P(14) };
                for (size_t k = 0; k < labels.size(); k++)
                    row.segs.push_back({ row.ctl.left + P(2) + int(k) * segW, row.ctl.top + P(2), row.ctl.left + P(2) + int(k + 1) * segW, row.ctl.bottom - P(2) });
            }
            else
                row.ctl = { right - ctlW, cy - P(10), right, cy + P(10) };
            const int w = ctlW + P(22) + TextWidth(g_f.bodyBold, kItems[i].label);
            row.r = { right - w - P(10), fy, right, fy + bh };
            g_rows.push_back(row);
        }
    if (g_name)
    {
        const int h = TextHeight(g_f.body, L"Ag", 1000);
        SetWindowPos(g_name, nullptr, g_nameBox.left + P(12), (g_nameBox.top + g_nameBox.bottom - h) / 2, g_nameBox.right - g_nameBox.left - P(24), h, SWP_NOZORDER);
        SendMessageW(g_name, WM_SETFONT, WPARAM(g_f.body), TRUE);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------------------------
void DrawToggle(Gdiplus::Graphics& g, const RECT& c, bool on, bool enabled)
{
    const COLORREF track = on ? (enabled ? kAccent : kAccentDim) : kTrackOff;
    const float rad = float(c.bottom - c.top) / 2;
    Fill(g, c, track, rad);
    if (!on) Stroke(g, c, kFieldLine, rad);
    const int d = (c.bottom - c.top) - P(8);
    const int kx = on ? c.right - P(4) - d : c.left + P(4);
    Gdiplus::SolidBrush knob(C(on ? kInk : (enabled ? kKnobOff : Mix(kKnobOff, kCard, 0.5f))));
    g.FillEllipse(&knob, kx, c.top + P(4), d, d);
}

void DrawRow(Gdiplus::Graphics& g, int index, COLORREF surface)
{
    const Row& row = g_rows[index];
    const Item& it = kItems[row.item];
    const bool enabled = Enabled(it), hover = index == g_hoverRow;
    if (hover && it.group != Foot) Fill(g, row.r, kRowHover, 7.0f * g_s);
    const COLORREF dimBase = hover && it.group != Foot ? kRowHover : surface;
    auto dim = [&](COLORREF c) { return enabled ? c : Mix(c, dimBase, 0.55f); };
    int labelLeft = row.r.left + P(10);
    if (it.parent && it.group != Foot)
    {
        // A sub-setting: a thin guide line ties it to the setting above.
        Gdiplus::Pen pen(C(kLine), 1.5f * g_s);
        const int gx = row.r.left + P(16);
        g.DrawLine(&pen, gx, row.r.top - P(4), gx, (row.r.top + row.r.bottom) / 2);
        g.DrawLine(&pen, gx, (row.r.top + row.r.bottom) / 2, gx + P(7), (row.r.top + row.r.bottom) / 2);
        labelLeft += P(18);
    }
    RECT label{ labelLeft, row.r.top, row.ctl.left - P(8), row.r.bottom };
    Text(g, it.label, label, it.group == Foot ? g_f.bodyBold : g_f.body, dim(it.group == Foot ? kText : kText), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::wstring& v = g_state[it.key];
    if (it.kind == Check)
        DrawToggle(g, row.ctl, v == L"1", enabled);
    else if (it.kind == Combo)
    {
        const bool over = hover && enabled && g_hoverSeg == 0;
        Fill(g, row.ctl, over ? kFieldHover : kField, 7.0f * g_s);
        Stroke(g, row.ctl, over ? kAccentDim : kFieldLine, 7.0f * g_s);
        RECT t{ row.ctl.left + P(11), row.ctl.top, row.ctl.right - P(26), row.ctl.bottom };
        Text(g, ChoiceLabel(it), t, g_f.body, dim(kText), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Chevron(g, row.ctl.right - P(14), (row.ctl.top + row.ctl.bottom) / 2, dim(kText2));
    }
    else
    {
        Fill(g, row.ctl, kField, 8.0f * g_s);
        Stroke(g, row.ctl, kFieldLine, 8.0f * g_s);
        const auto labels = Split(it.choices), values = Split(it.values);
        for (size_t k = 0; k < row.segs.size(); k++)
        {
            const bool sel = values[k] == v;
            if (sel)
                Fill(g, row.segs[k], enabled ? kAccent : kAccentDim, 6.0f * g_s);
            else if (hover && enabled && g_hoverSeg == int(k))
                Fill(g, row.segs[k], kFieldHover, 6.0f * g_s);
            Text(g, labels[k], row.segs[k], sel ? g_f.bodyBold : g_f.minor, sel ? kInk : dim(kText2), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

const wchar_t* ActLabel(Act a)
{
    switch (a)
    {
    case ActSave: return L"Save";
    case ActDelete: return g_deleteArmed ? L"Confirm" : L"Delete";
    case ActCopy: return L"Copy launch line";
    case ActStartGame: return L"Start game";
    case ActStartRace: return L"Test Race";
    default: return L"";
    }
}

void DrawButton(Gdiplus::Graphics& g, const Button& b, COLORREF surface)
{
    const bool hover = g_hoverAct == b.act, press = g_pressAct == b.act && hover;
    const float rad = 8.0f * g_s;
    if (b.act == ActPreset)
    {
        Fill(g, b.r, hover ? kFieldHover : kField, rad);
        Stroke(g, b.r, hover ? kAccentDim : kFieldLine, rad);
        RECT cap{ b.r.left + P(12), b.r.top, b.r.left + P(70), b.r.bottom };
        Text(g, L"PRESET", cap, g_f.caps, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE, P(1));
        RECT t{ b.r.left + P(68), b.r.top, b.r.right - P(28), b.r.bottom };
        Text(g, PresetLabel(), t, g_f.bodyBold, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Chevron(g, b.r.right - P(15), (b.r.top + b.r.bottom) / 2, kText2);
        return;
    }
    const bool primary = b.act == ActStartGame;
    const bool disabled = b.act == ActDelete && g_loadedCustom.empty();
    const bool danger = b.act == ActDelete && g_deleteArmed;
    COLORREF fill = primary ? (press ? Mix(kAccent, kInk, 0.15f) : hover ? Mix(kAccent, RGB(255, 255, 255), 0.12f) : kAccent)
                            : (hover && !disabled ? kFieldHover : surface);
    if (danger) fill = Mix(kBad, kInk, 0.35f);
    Fill(g, b.r, fill, rad);
    if (!primary && !danger) Stroke(g, b.r, hover && !disabled ? kAccentDim : kFieldLine, rad);
    const COLORREF ink = primary ? kInk : danger ? kText : disabled ? kMuted : kText;
    Text(g, ActLabel(b.act), b.r, g_f.button, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PaintMain(HDC hdc)
{
    RECT cr;
    GetClientRect(g_wnd, &cr);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Fill(g, cr, kBg);

        // Header: the name and the preset controls.
        Fill(g, g_header, kChrome);
        Fill(g, { 0, g_header.bottom - 1, g_width, g_header.bottom }, kLine);
        const int margin = P(24);
        const int hyperW = TextWidth(g_f.title, L"HYPER") + P(4) * 5;
        RECT name{ margin, 0, margin + hyperW + P(4), g_header.bottom };
        Text(g, L"HYPER", name, g_f.title, kAccent, DT_LEFT | DT_VCENTER | DT_SINGLELINE, P(4));
        const int settingsW = TextWidth(g_f.titleLight, L"Settings");
        RECT name2{ name.right + P(6), 0, name.right + P(6) + settingsW + P(4), g_header.bottom };
        Text(g, L"Settings", name2, g_f.titleLight, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT sub{ name2.right + P(16), P(3), name2.right + P(420), g_header.bottom };
        Text(g, L"ReGPU low-spec mode  \u00b7  Forza Motorsport 2", sub, g_f.minor, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Fill(g, g_nameBox, kField, 8.0f * g_s);
        Stroke(g, g_nameBox, g_nameFocus ? kAccent : kFieldLine, 8.0f * g_s);
        for (const Button& b : g_buttons)
            if (b.act == ActPreset || b.act == ActSave || b.act == ActDelete) DrawButton(g, b, kChrome);

        // Cards.
        for (const Card& card : g_cards)
        {
            Fill(g, card.r, kCard, 12.0f * g_s);
            Stroke(g, card.r, kLine, 12.0f * g_s);
            RECT head{ card.r.left + P(18), card.r.top + P(14), card.r.right - P(18), card.r.top + P(32) };
            std::wstring caps = kCards[card.index];
            for (auto& ch : caps) ch = towupper(ch);
            Text(g, caps, head, g_f.caps, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE, P(1.5f));
            if (card.index == Surf || card.index == Geom || card.index == Shad || card.index == Expt)
            {
                int on = 0, all = 0;
                for (int r : card.items)
                {
                    const Item& it = kItems[g_rows[r].item];
                    if (it.parent) continue;
                    all++;
                    if (g_state[it.key] != it.def) on++;
                }
                wchar_t count[32];
                swprintf(count, 32, on ? L"%d of %d on" : L"stock", on, all);
                Text(g, count, head, g_f.minor, on ? kAccent : kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            RECT note{ card.r.left + P(18), card.r.top + P(32), card.r.right - P(18), card.r.top + P(52) };
            Text(g, card.index == Rend ? RenderInfo() : std::wstring(kCardNotes[card.index]), note, g_f.minor, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            for (int r : card.items)
                DrawRow(g, r, kCard);
        }

        // Footer.
        Fill(g, g_footer, kChrome);
        Fill(g, { 0, g_footer.top, g_width, g_footer.top + 1 }, kLine);
        for (const Button& b : g_buttons)
            if (b.act == ActCopy || b.act == ActStartGame || b.act == ActStartRace) DrawButton(g, b, kChrome);
        for (size_t i = 0; i < g_rows.size(); i++)
            if (kItems[g_rows[i].item].group == Foot) DrawRow(g, int(i), kChrome);
        if (!g_status.empty())
        {
            RECT st{ margin + P(166), g_footer.top, margin + P(560), g_footer.bottom };
            Text(g, g_status, st, g_f.minor, kText2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------------------------------------------------------------
// The hover card
// ---------------------------------------------------------------------------------------------------------------------
struct TipContent
{
    std::wstring title, desc, caption, note, current;
    std::vector<std::vector<std::wstring>> rows; // value, choice, laptop, intel
};
TipContent g_tipContent;
const int kTipW = 380;

TipContent TipFor(const Item& it)
{
    TipContent t;
    t.title = it.label;
    t.desc = it.tip;
    t.note = it.note ? it.note : L"";
    t.caption = it.caption ? it.caption : L"Alone vs stock  \u00b7  720p, parked";
    t.current = g_state[it.key];
    if (it.perf)
        for (const std::wstring& row : Split(it.perf, L';'))
            t.rows.push_back(Split(row.c_str()));
    if (!Enabled(it) && it.parent)
    {
        std::wstring needs;
        if (!wcscmp(it.key, L"cheap_upscale")) needs = L"Used only with FSR 1.";
        else if (!wcscmp(it.key, L"fsr_version")) needs = L"Used only with FSR 3.";
        else if (!wcscmp(it.parent, L"upscaler")) needs = L"Used only with FSR 1 or FSR 3.";
        else if (const Item* p = Find(it.parent)) needs = L"Needs \u201c" + std::wstring(p->label) + L"\u201d on.";
        t.note = (t.note.empty() ? L"" : t.note + L"\n\n") + needs;
    }
    return t;
}
TipContent TipForAct(Act a)
{
    TipContent t;
    switch (a)
    {
    case ActPreset:
        t.title = L"Presets";
        t.desc = L"Off, Lossless, Turbo and Potato set every cut and the upscaler at once; your own presets are listed "
                 L"below them. Output resolution, sharpening and the display options stay as they are.";
        break;
    case ActSave:
        t.title = L"Save as a custom preset";
        t.desc = L"Saves every setting under the name in the box. Saving with an existing name replaces it.";
        t.note = L"Stored in Documents\\fm2\\hyper_menu.ini.";
        break;
    case ActDelete:
        t.title = L"Delete this custom preset";
        t.desc = g_loadedCustom.empty() ? L"Pick one of your own presets first. Built-in presets can't be deleted."
                                        : L"Click twice to delete \u201c" + g_loadedCustom + L"\u201d. The settings on screen stay.";
        break;
    case ActCopy:
        t.title = L"Copy launch line";
        t.desc = L"Copies the exact environment variables and command for these settings, for a log or a script.";
        break;
    case ActStartGame:
        t.title = L"Start game";
        t.desc = L"Boots the game normally, through the intro and the menus, with these settings.";
        break;
    case ActStartRace:
        t.title = L"Test Race";
        t.desc = L"Boots straight into an Exhibition race on Nissan Speedway, skipping the menus, as the benchmarks do.";
        break;
    default:
        break;
    }
    return t;
}

SIZE TipSize(const TipContent& t)
{
    const int pad = P(18), inner = P(kTipW) - pad * 2;
    int h = pad + TextHeight(g_f.tipTitle, t.title, inner) + P(6) + TextHeight(g_f.tipBody, t.desc, inner);
    if (!t.rows.empty())
        h += P(16) + P(20) + P(20) + int(t.rows.size()) * P(26);
    if (!t.note.empty())
        h += P(12) + TextHeight(g_f.minor, t.note, inner);
    return { P(kTipW), h + pad };
}

void PaintTip(HDC hdc)
{
    RECT cr;
    GetClientRect(g_tip, &cr);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const TipContent& t = g_tipContent;
        Fill(g, cr, kTip);
        const int pad = P(18), inner = cr.right - pad * 2;
        int y = pad;
        const int th = TextHeight(g_f.tipTitle, t.title, inner);
        Text(g, t.title, { pad, y, pad + inner, y + th }, g_f.tipTitle, kText, DT_LEFT | DT_WORDBREAK);
        y += th + P(6);
        const int dh = TextHeight(g_f.tipBody, t.desc, inner);
        Text(g, t.desc, { pad, y, pad + inner, y + dh }, g_f.tipBody, kText2, DT_LEFT | DT_WORDBREAK);
        y += dh;
        if (!t.rows.empty())
        {
            y += P(16);
            const int colW = P(96), c2 = pad + inner - colW * 2;
            std::wstring caption = t.caption;
            for (auto& ch : caption) ch = towupper(ch);
            Text(g, caption, { pad, y, pad + inner, y + P(20) }, g_f.caps, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, P(1));
            y += P(20);
            Fill(g, { pad, y + P(19), pad + inner, y + P(20) }, kLine);
            Text(g, L"Setting", { pad, y, c2, y + P(20) }, g_f.minor, kText2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(g, L"Vega laptop", { c2, y, c2 + colW, y + P(20) }, g_f.minor, kText2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            Text(g, L"HD 630", { c2 + colW, y, c2 + colW * 2, y + P(20) }, g_f.minor, kText2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            y += P(20);
            for (const auto& row : t.rows)
            {
                if (row.size() < 4) continue;
                RECT rr{ pad - P(8), y + P(1), pad + inner + P(8), y + P(25) };
                const bool current = row[0] == t.current;
                if (current) Fill(g, rr, Mix(kAccent, kTip, 0.86f), 6.0f * g_s);
                Text(g, row[1], { pad, y, c2, y + P(26) }, current ? g_f.bodyBold : g_f.tipBody, current ? kAccent : kText,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                for (int k = 2; k < 4; k++)
                {
                    const std::wstring& cell = row[k];
                    const wchar_t tone = cell.empty() ? L'n' : cell[0];
                    const COLORREF c = tone == L'g' ? kGood : tone == L'b' ? kBad : kMuted;
                    Text(g, cell.substr(1), { c2 + colW * (k - 2), y, c2 + colW * (k - 1), y + P(26) }, tone == L'n' ? g_f.minor : g_f.bodyBold, c,
                         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
                y += P(26);
            }
        }
        if (!t.note.empty())
        {
            y += P(12);
            const int nh = TextHeight(g_f.minor, t.note, inner);
            Text(g, t.note, { pad, y, pad + inner, y + nh }, g_f.minor, kMuted, DT_LEFT | DT_WORDBREAK);
        }
    }
    BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void HideTip()
{
    KillTimer(g_wnd, 1);
    if (g_tip && IsWindowVisible(g_tip)) ShowWindow(g_tip, SW_HIDE);
    g_tipRow = -1;
}
// key: a row index (>= 0) or -(Act) for a button.
void ShowTip(int key)
{
    RECT anchor;
    if (key >= 0)
    {
        g_tipContent = TipFor(kItems[g_rows[key].item]);
        anchor = g_rows[key].r;
        for (const Card& c : g_cards)
            for (int r : c.items)
                if (r == key) { anchor.left = c.r.left; anchor.right = c.r.right; }
    }
    else
    {
        g_tipContent = TipForAct(Act(-key));
        if (g_tipContent.title.empty()) return;
        for (const Button& b : g_buttons)
            if (b.act == Act(-key)) anchor = b.r;
    }
    const SIZE sz = TipSize(g_tipContent);
    POINT tl{ anchor.left, anchor.top }, br{ anchor.right, anchor.bottom };
    ClientToScreen(g_wnd, &tl);
    ClientToScreen(g_wnd, &br);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    int x, y;
    if (key >= 0)
    {
        x = br.x + P(10);
        if (x + sz.cx > wa.right) x = tl.x - P(10) - sz.cx;
        y = tl.y - P(10);
    }
    else
    {
        x = std::min(tl.x, br.x - int(sz.cx));
        y = br.y + P(8);
        if (y + sz.cy > wa.bottom) y = tl.y - P(8) - sz.cy;
    }
    x = std::clamp(x, int(wa.left) + P(4), int(wa.right) - int(sz.cx) - P(4));
    y = std::clamp(y, int(wa.top) + P(4), int(wa.bottom) - int(sz.cy) - P(4));
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, sz.cx, sz.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_tip, nullptr, FALSE);
    g_tipRow = key;
}
int HoverKey() { return g_hoverRow >= 0 ? g_hoverRow : g_hoverAct != ActNone ? -int(g_hoverAct) : INT_MIN; }

// ---------------------------------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------------------------------
void SetStatus(const std::wstring& s)
{
    g_status = s;
    SetTimer(g_wnd, 2, 4000, nullptr);
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void SaveSection(const std::wstring& section)
{
    for (const Item& it : kItems)
        WritePrivateProfileStringW(section.c_str(), it.key, g_state[it.key].c_str(), g_ini.c_str());
}
bool LoadSection(const std::wstring& section)
{
    wchar_t buf[256];
    bool any = false;
    for (const Item& it : kItems)
        if (GetPrivateProfileStringW(section.c_str(), it.key, L"\x01", buf, 256, g_ini.c_str()) && buf[0] != 1)
        {
            g_state[it.key] = buf;
            any = true;
        }
    return any;
}
std::vector<std::wstring> CustomNames()
{
    std::vector<std::wstring> out;
    std::vector<wchar_t> names(16384);
    GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), g_ini.c_str());
    for (const wchar_t* n = names.data(); *n; n += wcslen(n) + 1)
        if (wcscmp(n, L"last") && wcscmp(n, L"window")) out.push_back(n);
    return out;
}
void ApplyPreset(int i)
{
    for (const auto& [k, v] : PresetState(i))
        g_state[k] = v;
    g_loadedCustom.clear();
    g_deleteArmed = false;
}
void Changed()
{
    if (g_tipRow != INT_MIN && g_tip && IsWindowVisible(g_tip) && g_tipRow >= 0)
    {
        g_tipContent = TipFor(kItems[g_rows[g_tipRow].item]);
        InvalidateRect(g_tip, nullptr, FALSE);
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}
std::wstring LaunchLine(bool inRace)
{
    std::wstring line;
    for (const Var& v : BuildEnv())
        if (!v.unset) line += v.name + L"=" + v.value + L" ";
    line += L"fm2.exe game";
    if (inRace) line += RaceArgs();
    return line;
}
void Copy()
{
    const std::wstring line = LaunchLine(true);
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (line.size() + 1) * sizeof(wchar_t)))
    {
        memcpy(GlobalLock(mem), line.c_str(), (line.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    SetStatus(L"Launch line copied to the clipboard.");
}
void Start(bool inRace)
{
    SaveSection(L"last");
    for (const Var& v : BuildEnv())
        SetEnvironmentVariableW(v.name.c_str(), v.unset ? nullptr : v.value.c_str());
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/"));
    std::wstring cmd = L"\"" + dir + L"\\fm2.exe\" game";
    if (inRace)
        cmd += RaceArgs();
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetStatus(inRace ? L"Starting the race\u2026" : L"Starting the game\u2026");
    }
    else
        SetStatus(L"Could not start fm2.exe: it must be in the same folder as hyper_menu.exe.");
}
void Save()
{
    wchar_t name[64] = {};
    GetWindowTextW(g_name, name, 64);
    std::wstring n = name;
    while (!n.empty() && n.back() == L' ') n.pop_back();
    for (wchar_t& ch : n)
        if (ch == L'[' || ch == L']' || ch == L'=') ch = L'-';
    if (n.empty() || n == L"last" || n == L"window")
    {
        SetStatus(L"Type a name for the preset first.");
        SetFocus(g_name);
        return;
    }
    SaveSection(n);
    g_loadedCustom = n;
    g_customSnapshot = g_state;
    SetWindowTextW(g_name, L"");
    SetStatus(L"Saved \u201c" + n + L"\u201d.");
}
void PresetMenu(const RECT& anchor)
{
    HMENU m = CreatePopupMenu();
    const int current = g_loadedCustom.empty() ? MatchPreset(false) : -1;
    for (int i = 0; i < kPresetCount; i++)
        AppendMenuW(m, MF_STRING | (i == current ? MF_CHECKED : 0), 1 + i, kPresets[i].name);
    const auto customs = CustomNames();
    if (!customs.empty())
    {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < customs.size(); i++)
            AppendMenuW(m, MF_STRING | (customs[i] == g_loadedCustom ? MF_CHECKED : 0), 100 + int(i), customs[i].c_str());
    }
    POINT pt{ anchor.left, anchor.bottom + P(4) };
    ClientToScreen(g_wnd, &pt);
    const int id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, g_wnd, nullptr);
    DestroyMenu(m);
    if (id >= 1 && id <= kPresetCount)
    {
        ApplyPreset(id - 1);
        SetStatus(std::wstring(L"Preset ") + kPresets[id - 1].name + L".");
    }
    else if (id >= 100 && id - 100 < int(customs.size()))
    {
        LoadSection(customs[id - 100]);
        g_loadedCustom = customs[id - 100];
        g_customSnapshot = g_state;
        g_deleteArmed = false;
    }
    Changed();
}
void ChoiceMenu(const Row& row)
{
    const Item& it = kItems[row.item];
    const auto labels = Split(it.choices), values = Split(it.values);
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < labels.size(); i++)
        AppendMenuW(m, MF_STRING | (values[i] == g_state[it.key] ? MF_CHECKED : 0), 1 + int(i), labels[i].c_str());
    POINT pt{ row.ctl.left, row.ctl.bottom + P(4) };
    ClientToScreen(g_wnd, &pt);
    const int id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, g_wnd, nullptr);
    DestroyMenu(m);
    if (id >= 1 && id <= int(values.size()))
    {
        g_state[it.key] = values[id - 1];
        Changed();
    }
}
void Click(int x, int y)
{
    const POINT pt{ x, y };
    for (const Button& b : g_buttons)
        if (PtInRect(&b.r, pt))
        {
            HideTip();
            switch (b.act)
            {
            case ActPreset: PresetMenu(b.r); break;
            case ActSave: Save(); break;
            case ActDelete:
                if (g_loadedCustom.empty())
                    SetStatus(L"Pick one of your own presets to delete it.");
                else if (!g_deleteArmed)
                {
                    g_deleteArmed = true;
                    SetTimer(g_wnd, 3, 3000, nullptr);
                }
                else
                {
                    WritePrivateProfileStringW(g_loadedCustom.c_str(), nullptr, nullptr, g_ini.c_str());
                    SetStatus(L"Deleted \u201c" + g_loadedCustom + L"\u201d.");
                    g_loadedCustom.clear();
                    g_deleteArmed = false;
                }
                break;
            case ActCopy: Copy(); break;
            case ActStartGame: Start(false); break;
            case ActStartRace: Start(true); break;
            default: break;
            }
            InvalidateRect(g_wnd, nullptr, FALSE);
            return;
        }
    for (const Row& row : g_rows)
    {
        if (!PtInRect(&row.r, pt)) continue;
        const Item& it = kItems[row.item];
        if (!Enabled(it)) return;
        if (it.kind == Check)
            g_state[it.key] = g_state[it.key] == L"1" ? L"0" : L"1";
        else if (it.kind == Combo && PtInRect(&row.ctl, pt))
        {
            HideTip();
            ChoiceMenu(row);
            return;
        }
        else if (it.kind == Radio)
        {
            const auto values = Split(it.values);
            for (size_t k = 0; k < row.segs.size(); k++)
                if (PtInRect(&row.segs[k], pt)) g_state[it.key] = values[k];
        }
        g_deleteArmed = false;
        Changed();
        return;
    }
}
void HoverAt(int x, int y)
{
    const POINT pt{ x, y };
    int row = -1, seg = -1;
    Act act = ActNone;
    for (const Button& b : g_buttons)
        if (PtInRect(&b.r, pt)) act = b.act;
    for (size_t i = 0; act == ActNone && i < g_rows.size(); i++)
        if (PtInRect(&g_rows[i].r, pt))
        {
            row = int(i);
            const Row& r = g_rows[i];
            if (kItems[r.item].kind == Combo && PtInRect(&r.ctl, pt)) seg = 0;
            for (size_t k = 0; k < r.segs.size(); k++)
                if (PtInRect(&r.segs[k], pt)) seg = int(k);
        }
    if (row != g_hoverRow || seg != g_hoverSeg || act != g_hoverAct)
    {
        const int before = HoverKey();
        g_hoverRow = row;
        g_hoverSeg = seg;
        g_hoverAct = act;
        InvalidateRect(g_wnd, nullptr, FALSE);
        const int now = HoverKey();
        if (now != before)
        {
            if (now == INT_MIN)
                HideTip();
            else if (g_tip && IsWindowVisible(g_tip))
                ShowTip(now); // already reading cards: follow the mouse at once
            else
                SetTimer(g_wnd, 1, 420, nullptr);
        }
    }
    const bool hand = act != ActNone || (row >= 0 && Enabled(kItems[g_rows[row].item]) &&
                                         (kItems[g_rows[row].item].kind == Check || seg >= 0));
    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
}

// ---------------------------------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------------------------------
void FitWindow()
{
    RECT r{ 0, 0, g_width, g_height };
    const UINT dpi = GetDpiForWindow(g_wnd);
    AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongW(g_wnd, GWL_STYLE)), FALSE, 0, dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK TipProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        PaintTip(dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT CALLBACK Proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        PaintMain(dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, wnd, 0 };
        TrackMouseEvent(&tme);
        HoverAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        // A leave that arrives after the mouse came back into the client area (fast moves): keep tracking instead.
        // Only the client area: over the title bar, re-arming posts another leave at once, an endless loop that
        // starved the window's repaints (it looked frozen until the mouse left the window, 16 Nov 2026).
        POINT cur;
        GetCursorPos(&cur);
        POINT local = cur;
        ScreenToClient(wnd, &local);
        RECT client;
        GetClientRect(wnd, &client);
        if (WindowFromPoint(cur) == wnd && PtInRect(&client, local))
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
    }
        g_hoverRow = g_hoverSeg = -1;
        g_hoverAct = ActNone;
        HideTip();
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(wnd);
        g_pressAct = g_hoverAct;
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        g_pressAct = ActNone;
        Click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE; // set in WM_MOUSEMOVE
        break;
    case WM_TIMER:
        KillTimer(wnd, wp);
        if (wp == 1 && HoverKey() != INT_MIN) ShowTip(HoverKey());
        if (wp == 2) g_status.clear();
        if (wp == 3) g_deleteArmed = false;
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (HWND(lp) == g_name && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS))
        {
            g_nameFocus = HIWORD(wp) == EN_SETFOCUS;
            InvalidateRect(wnd, &g_nameBox, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor(HDC(wp), kText);
        SetBkColor(HDC(wp), kField);
        return LRESULT(g_fieldBrush);
    case WM_DPICHANGED:
    {
        g_s = HIWORD(wp) / 96.0f;
        MakeFonts();
        Layout();
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(wnd, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        FitWindow();
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) HideTip();
        break;
    case WM_CLOSE:
        SaveSection(L"last");
        WritePrivateProfileStringW(L"window", L"custom", g_loadedCustom.c_str(), g_ini.c_str());
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Dark popup menus (the dropdowns): uxtheme's unnamed exports, as Explorer and Notepad++ use them. Ignored if absent.
void DarkMenus(HWND wnd)
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using AllowWindow = bool(WINAPI*)(HWND, bool);
    using Plain = void(WINAPI*)();
    if (auto f = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)))) f(2); // SetPreferredAppMode(ForceDark)
    if (auto f = reinterpret_cast<AllowWindow>(GetProcAddress(ux, MAKEINTRESOURCEA(133)))) f(wnd, true);
    if (auto f = reinterpret_cast<Plain>(GetProcAddress(ux, MAKEINTRESOURCEA(104)))) f(); // RefreshImmersiveColorPolicyState
    if (auto f = reinterpret_cast<Plain>(GetProcAddress(ux, MAKEINTRESOURCEA(136)))) f(); // FlushMenuThemes
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gtoken = 0;
    Gdiplus::GdiplusStartup(&gtoken, &gin, nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // The preset file: Documents\fm2 (the game's user folder). The game's working folder: the first folder at or above
    // this program that holds "game" (fm2.exe is started as "fm2.exe game" from there, as the scripts do).
    wchar_t docs[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
    CreateDirectoryW((std::wstring(docs) + L"\\fm2").c_str(), nullptr);
    g_ini = std::wstring(docs) + L"\\fm2\\hyper_menu.ini";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_root = exe;
    for (int up = 0; up < 5; up++)
    {
        const size_t slash = g_root.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        g_root.resize(slash);
        const DWORD a = GetFileAttributesW((g_root + L"\\game").c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) break;
    }

    if (FontExists(L"Segoe UI Variable Text")) g_textFace = L"Segoe UI Variable Text";
    if (FontExists(L"Segoe UI Variable Display")) g_displayFace = L"Segoe UI Variable Display";
    g_fieldBrush = CreateSolidBrush(kField);

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"HyperMenu";
    RegisterClassW(&wc);
    WNDCLASSW tc{};
    tc.lpfnWndProc = TipProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tc.lpszClassName = L"HyperTip";
    RegisterClassW(&tc);

    g_wnd = CreateWindowExW(0, L"HyperMenu", L"Hyper Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, inst, nullptr);
    g_s = GetDpiForWindow(g_wnd) / 96.0f;
    // Title bar in the window's colours (Windows 11; ignored elsewhere).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF caption = kChrome, captionText = kText;
    DwmSetWindowAttribute(g_wnd, 35, &caption, sizeof(caption));         // DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(g_wnd, 36, &captionText, sizeof(captionText)); // DWMWA_TEXT_COLOR
    DarkMenus(g_wnd);

    g_tip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"HyperTip", L"", WS_POPUP, 0, 0, 10, 10, g_wnd, nullptr, inst, nullptr);
    const int round = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g_tip, 33, &round, sizeof(round));
    COLORREF border = kFieldLine;
    DwmSetWindowAttribute(g_tip, 34, &border, sizeof(border));

    g_name = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_wnd, nullptr, inst, nullptr);
    SendMessageW(g_name, EM_SETCUEBANNER, TRUE, LPARAM(L"Name for a custom preset"));
    SendMessageW(g_name, EM_SETLIMITTEXT, 48, 0);

    MakeFonts();
    Layout();
    FitWindow();

    for (const Item& it : kItems)
        g_state[it.key] = it.def;
    if (LoadSection(L"last"))
    {
        wchar_t custom[128] = {};
        GetPrivateProfileStringW(L"window", L"custom", L"", custom, 128, g_ini.c_str());
        const auto names = CustomNames();
        if (*custom && std::find(names.begin(), names.end(), custom) != names.end())
        {
            const auto now = g_state;
            LoadSection(custom);
            g_customSnapshot = g_state;
            g_state = now;
            g_loadedCustom = custom;
        }
    }
    else
        ApplyPreset(PresetIndex(L"Potato"));

    // Centre on the screen it opens on.
    RECT wr;
    GetWindowRect(g_wnd, &wr);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(g_wnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                 mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - h) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (m.message == WM_KEYDOWN && m.hwnd == g_name && m.wParam == VK_RETURN)
        {
            Save();
            InvalidateRect(g_wnd, nullptr, FALSE);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Gdiplus::GdiplusShutdown(gtoken);
    return 0;
}
