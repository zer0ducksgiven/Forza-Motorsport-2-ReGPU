// XAM: users, profiles, system UI, content packages, input, notifications.
// The port runs headless: one signed-in local profile, every system dialog
// answered with its default.
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

// When the boot's shader warm-up ended (renderer.cpp): input recordings and replays count from it.
std::atomic<int64_t> g_inputEpochNs{ 0 };

#include "gpu/renderer.h"
#include "input/wheel.h"
#include "kernel/sessions.h"
#include "kernel/wheel_ff.h"
#include "debug.h"
#include "kernel/imports.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "live_switch.h"
#include "loader.h"
#include "render/presenter.h"
#include "settings.h"
#include "zip.h"

#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

namespace
{
// The profile's offline XUID (0xE000...). The saves are signed for their owner, so it is kept in
// the profile folder (profiles\<name>\xuid): a copied, renamed or imported profile stays valid.
// A profile without the file gets the one derived from its name (what every profile had before).
uint64_t Xuid()
{
    static const uint64_t xuid = [] {
        const auto file = ContentDir().parent_path() / "xuid";
        if (FILE* f = _wfopen(file.c_str(), L"r"))
        {
            unsigned long long v = 0;
            const bool ok = fscanf(f, "%llx", &v) == 1 && (v >> 60) == 0xE;
            fclose(f);
            if (ok)
                return uint64_t(v);
        }
        uint64_t h = 0xCBF29CE484222325ull;
        for (char c : GetSettings().profile)
            h = (h ^ uint8_t(c)) * 0x100000001B3ull;
        const uint64_t derived = 0xE000000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
        if (FILE* f = _wfopen(file.c_str(), L"w"))
        {
            fprintf(f, "%016llX\n", (unsigned long long)derived);
            fclose(f);
        }
        return derived;
    }();
    return xuid;
}

constexpr uint32_t kContentDataSize = 0x134;

// A DLC package's car zips (Media\DLCZips\<name>.zip) carry their database patches (media\db\patch\*.slt,
// *.sql: the cars' Data_Car rows and so on), and the game lists that directory through a folder of the
// zip's name (<name>\media\db\patch\), not inside the zip. On the console the package's files are laid out
// that way; unpacked here from the zip alone the folder was missing, so no DLC car was ever added to the
// database (official packs and backported "mod" cars alike). The patch files are written there once (again
// if the zip changes); everything else is still read from the zip.
// A DLC package's root: the folder that holds Media. Packages extracted with other tools sometimes
// sit one folder deeper (<pack>\<folder>\Media\...); the game then saw an empty pack (no cars or
// tracks) and its database patches were never found.
}

std::filesystem::path DlcPackageRoot(const std::filesystem::path& package)
{
    std::error_code ec;
    if (std::filesystem::is_directory(package / "Media", ec))
        return package;
    std::filesystem::path only;
    int dirs = 0;
    for (const auto& e : std::filesystem::directory_iterator(package, ec))
        if (e.is_directory(ec))
        {
            dirs++;
            only = e.path();
        }
    if (dirs == 1 && std::filesystem::is_directory(only / "Media", ec))
        return only;
    return package;
}

namespace
{

void UnpackDlcPatches(const std::filesystem::path& packageFolder)
{
    const std::filesystem::path package = DlcPackageRoot(packageFolder);
    {
        // What the pack holds, for bug reports: its car zips and whether Media was found.
        std::error_code ec;
        int zipsFound = 0;
        for (const auto& z : std::filesystem::directory_iterator(package / "Media" / "DLCZips", ec))
            zipsFound += z.path().extension() == ".zip";
        Log("content: DLC %s: %s%s, %d car zip(s)", packageFolder.filename().string().c_str(),
            std::filesystem::is_directory(package / "Media", ec) ? "Media found" : "NO Media folder (not installed the way the game reads it)",
            package == packageFolder ? "" : " (one folder deeper)", zipsFound);
    }
    std::error_code ec;
    const std::filesystem::path zips = package / "Media" / "DLCZips";
    for (const auto& z : std::filesystem::directory_iterator(zips, ec))
    {
        if (!z.is_regular_file(ec) || z.path().extension() != ".zip")
            continue;
        const std::filesystem::path folder = z.path().parent_path() / z.path().stem();
        const std::filesystem::path stamp = folder / ".fm2-patches";
        const std::string want = std::to_string(z.file_size(ec)) + " " + std::to_string(z.last_write_time(ec).time_since_epoch().count());
        {
            std::ifstream in(stamp);
            std::string have;
            std::getline(in, have);
            if (have == want)
                continue;
        }
        std::ifstream in(z.path(), std::ios::binary);
        const std::vector<uint8_t> zip{ std::istreambuf_iterator<char>(in), {} };
        int written = 0;
        ForEachZipMember(zip, "media/db/patch/", [&](const std::string& name, const std::vector<uint8_t>& data) {
            std::string lower = name;
            for (char& c : lower)
                c = char(tolower(uint8_t(c)));
            if (lower.rfind("media/db/patch/", 0) != 0 || lower.back() == '/')
                return;
            const std::filesystem::path out = folder / std::filesystem::path(name).make_preferred();
            std::filesystem::create_directories(out.parent_path(), ec);
            std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            written++;
        });
        if (written)
        {
            std::ofstream(stamp) << want << "\n";
            Log("content: %s: %d database patch file(s) unpacked", z.path().filename().string().c_str(), written);
        }
    }
}

// XCONTENT_DATA: deviceId, contentType, displayName[128] (UTF-16 BE), fileName[42].
struct ContentItem
{
    uint32_t type = 0;
    std::u16string displayName;
    std::string fileName;
};

std::filesystem::path ContentTypeDir(uint32_t type)
{
    char t[16];
    snprintf(t, sizeof(t), "%08X", type);
    return ContentDir() / t;
}

// Display names live beside the packages (<fileName>.title, UTF-16 LE), outside what the game sees.
std::filesystem::path TitlePath(uint32_t type, const std::string& fileName) { return ContentTypeDir(type) / (fileName + ".title"); }

struct ListenerObject : KernelObject
{
    ListenerObject() : KernelObject(Kind::Other) {}
    uint64_t mask = 0;
    std::deque<std::pair<uint32_t, uint32_t>> queue; // id, param
};

struct EnumeratorObject : KernelObject
{
    EnumeratorObject() : KernelObject(Kind::Other) {}
    std::vector<ContentItem> items;
    size_t next = 0;
};

std::mutex g_listenerMutex;
}

// Online (FM2-online-scope.md), step 1: FM2_LIVE=1 shows the game a profile signed in to Xbox
// Live and a network that is up, and logs every online request it then makes (XMsg messages,
// sockets), to map what it needs. The requests themselves still fail.
bool LiveTrace()
{
    static const bool on = getenv("FM2_LIVE") != nullptr;
    return on;
}

namespace
{
// The first bytes of a guest buffer, as hex.
std::string HexBytes(uint32_t address, uint32_t size)
{
    std::string s;
    char b[4];
    for (uint32_t i = 0; address && i < size && i < 64; i++)
    {
        snprintf(b, sizeof(b), "%02X", *GuestPtr<uint8_t>(address + i));
        s += b;
        if (i % 4 == 3)
            s += ' ';
    }
    return s;
}

// Each distinct (app, message) of an XMsg call, the first few times.
void LogXMsg(const char* call, uint32_t app, uint32_t message, uint32_t buffer, uint32_t size, uint32_t lr)
{
    static std::mutex mutex;
    static std::map<uint64_t, int> seen;
    std::lock_guard lock(mutex);
    if (seen[uint64_t(app) << 32 | message]++ >= 3)
        return;
    Log("live: %s app %X message %08X, %u bytes at %08X (lr %08X): %s", call, app, message, size, buffer, lr,
        HexBytes(buffer, size).c_str());
}
}

uint64_t OnlineLocalXuid(uint32_t userIndex)
{
    return userIndex == 0 ? Xuid() : 0;
}

// Users ------------------------------------------------------------------------

GUEST_EXPORT(XamUserGetSigninState)
{
    // 1 signed in locally, 2 signed in to Xbox Live (FM2_LIVE)
    Return(ctx, ctx.r3.u32 == 0 ? (LiveTrace() ? 2 : 1) : 0);
}

GUEST_EXPORT(XamUserGetXUID)
{
    // (user, type, xuid*)
    if (ctx.r3.u32 != 0)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    StoreBE64(ctx.r5.u32, Xuid());
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserGetName)
{
    // (user, buffer, size)
    if (ctx.r3.u32 != 0)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    const char* name = GetSettings().profile.c_str();
    const uint32_t size = ctx.r5.u32;
    if (size)
    {
        strncpy(GuestPtr<char>(ctx.r4.u32), name, size);
        GuestPtr<char>(ctx.r4.u32)[size - 1] = 0;
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserCheckPrivilege)
{
    // (user, mask, result*)
    StoreIfNonNull(ctx.r5.u32, 1);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserAreUsersFriends)
{
    Return(ctx, X_ERROR_NO_SUCH_USER);
}

GUEST_EXPORT(XamUserReadProfileSettings)
{
    // (titleId, user, xuidCount, xuids, settingCount, settingIds*, bufferSize*, buffer, overlapped)
    // Answer every setting as "not set": the title then uses its defaults.
    const uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, sizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    const uint32_t overlapped = StackArg(ctx, 8);
    // XUSER_READ_PROFILE_SETTING_RESULT { u32 count, u32 settings* } then
    // XUSER_PROFILE_SETTING { u32 source, u32 user, u64 xuid? ..., u32 id, XUSER_DATA (0x10) } of 0x28 bytes.
    const uint32_t needed = 8 + settingCount * 0x28;
    if (!buffer || LoadBE32(sizePtr) < needed)
    {
        StoreBE32(sizePtr, needed);
        Return(ctx, 122); // ERROR_INSUFFICIENT_BUFFER
        return;
    }
    memset(GuestPtr(buffer), 0, needed);
    StoreBE32(buffer, settingCount);
    StoreBE32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        const uint32_t s = buffer + 8 + i * 0x28;
        StoreBE32(s + 0x00, 0); // source: not set (default)
        StoreBE32(s + 0x04, ctx.r4.u32);
        StoreBE32(s + 0x10, LoadBE32(settingIds + i * 4));
    }
    if (overlapped)
    {
        CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
        Return(ctx, X_ERROR_IO_PENDING);
        return;
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserCreateStatsEnumerator)
{
    Return(ctx, X_ERROR_FUNCTION_FAILED);
}

// System ------------------------------------------------------------------------

GUEST_EXPORT(XGetAVPack) { Return(ctx, 6); }        // HDMI
GUEST_EXPORT(XGetGameRegion) { Return(ctx, 0xFFFF); }
GUEST_EXPORT(XGetLanguage) { Return(ctx, GetSettings().language); } // settings.ini language
GUEST_EXPORT(XamGetSystemVersion) { Return(ctx, 0x20004B07); }
GUEST_EXPORT(XNetLogonGetTitleID) { Return(ctx, g_title.titleId); }

GUEST_EXPORT(XamGetExecutionId)
{
    StoreIfNonNull(ctx.r3.u32, g_title.executionId);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XamAlloc)
{
    // (flags, size, out*)
    const uint32_t address = GuestAlloc(Heap::Virtual, ctx.r4.u32);
    StoreIfNonNull(ctx.r5.u32, address);
    Return(ctx, address ? X_ERROR_SUCCESS : 8);
}

GUEST_EXPORT(XamFree)
{
    GuestFree(ctx.r3.u32);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamLoaderGetLaunchDataSize)
{
    StoreIfNonNull(ctx.r3.u32, 0);
    Return(ctx, X_ERROR_NOT_FOUND);
}

GUEST_EXPORT(XamLoaderGetLaunchData)
{
    Return(ctx, X_ERROR_NOT_FOUND);
}

GUEST_EXPORT(XamLoaderSetLaunchData)
{
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamLoaderLaunchTitle)
{
    Log("XamLoaderLaunchTitle(\"%s\"): exiting", GuestString(ctx.r3.u32).c_str());
    WheelStop();
    std::_Exit(0);
}

GUEST_EXPORT(XamLoaderTerminateTitle)
{
    Log("XamLoaderTerminateTitle: exiting");
    WheelStop();
    std::_Exit(0);
}

// Notifications -----------------------------------------------------------------

namespace
{
std::vector<std::weak_ptr<ListenerObject>> g_listeners;
}

GUEST_EXPORT(XamNotifyCreateListener)
{
    // (areas mask (u64), unknown)
    auto listener = std::make_shared<ListenerObject>();
    listener->mask = ctx.r3.u64;
    // Startup state as a fresh console reports it.
    listener->queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED: user 0
    listener->queue.push_back({ 0x00000009, 0 }); // XN_SYS_UI: hidden
    {
        std::lock_guard lock(g_listenerMutex);
        g_listeners.push_back(listener);
    }
    Return(ctx, CreateHandle(listener));
}

GUEST_EXPORT(XNotifyGetNext)
{
    // (handle, matchId, id*, param*)
    auto object = GetHandle(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, 0);
        return;
    }
    auto& listener = static_cast<ListenerObject&>(*object);
    std::lock_guard lock(g_listenerMutex);
    const uint32_t match = ctx.r4.u32;
    for (auto it = listener.queue.begin(); it != listener.queue.end(); ++it)
    {
        if (match && it->first != match)
            continue;
        StoreIfNonNull(ctx.r5.u32, it->first);
        StoreIfNonNull(ctx.r6.u32, it->second);
        listener.queue.erase(it);
        Return(ctx, 1);
        return;
    }
    Return(ctx, 0);
}

// System UI: answered at once ------------------------------------------------------

namespace
{
void FinishUi(PPCContext& ctx, uint32_t overlapped, uint32_t result = X_ERROR_SUCCESS)
{
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result);
        Return(ctx, X_ERROR_IO_PENDING);
    }
    else
        Return(ctx, result);
}
}

GUEST_EXPORT(XamShowSigninUI)
{
    // The sign-in guide opens and closes at once, with the profile signed in.
    std::lock_guard lock(g_listenerMutex);
    for (auto& weak : g_listeners)
        if (auto l = weak.lock())
        {
            l->queue.push_back({ 0x00000009, 1 }); // XN_SYS_UI: shown
            l->queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED: user 0
            l->queue.push_back({ 0x00000009, 0 }); // XN_SYS_UI: hidden
        }
    Return(ctx, X_ERROR_SUCCESS);
}
GUEST_EXPORT(XamShowFriendsUI) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowGamerCardUIForXUID) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowDirtyDiscErrorUI)
{
    Fatal("the title reported a disc read error (XamShowDirtyDiscErrorUI): check the game files");
}

GUEST_EXPORT(XamShowDeviceSelectorUI)
{
    // (user, contentType, flags, totalRequested (u64, one register), deviceId*, overlapped)
    StoreIfNonNull(ctx.r7.u32, 1); // the hard drive
    FinishUi(ctx, ctx.r8.u32);
}

GUEST_EXPORT(XamShowMessageBoxUI)
{
    // (user, title, text, buttonCount, buttons, focus, flags, result* (r10), overlapped (stack))
    auto wide = [](uint32_t p) {
        std::string out;
        for (uint32_t i = 0; p && i < 512; i++)
        {
            const uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(p)[i]);
            if (!c)
                break;
            out += c < 0x80 ? char(c) : '?';
        }
        return out;
    };
    std::string text = wide(ctx.r4.u32) + " | " + wide(ctx.r5.u32);
    for (uint32_t b = 0; b < ctx.r6.u32 && b < 4; b++)
        text += " [" + wide(LoadBE32(ctx.r7.u32 + b * 4)) + "]";
    Log("message box: %s", text.c_str());
    StoreIfNonNull(ctx.r10.u32, 0); // result: first button
    FinishUi(ctx, StackArg(ctx, 8));
}

GUEST_EXPORT(XamShowMessageBoxUIEx)
{
    StoreIfNonNull(StackArg(ctx, 8), 0);
    FinishUi(ctx, StackArg(ctx, 9));
}

GUEST_EXPORT(XamShowKeyboardUI)
{
    // (user, flags, default text, title, description, result buffer, max length, overlapped)
    // A text box over the game window stands in for the console's keyboard (GitHub #13: handing back
    // the default, empty for a new tune's name, made the game ask again forever).
    const uint32_t def = ctx.r5.u32, out = ctx.r8.u32, max = ctx.r9.u32;
    auto read = [](uint32_t p) {
        std::wstring s;
        for (uint32_t i = 0; p && i < 512; i++)
        {
            const uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(p)[i]);
            if (!c)
                break;
            s += wchar_t(c);
        }
        return s;
    };
    std::wstring text = read(def);
    const bool ok = PresenterAskText(read(ctx.r6.u32), read(ctx.r7.u32), text, max ? max - 1 : 0);
    Log("keyboard: %s", ok ? "entered text" : "cancelled");
    if (out && max)
    {
        uint32_t i = 0;
        for (; i < text.size() && i + 1 < max; i++)
            GuestPtr<uint16_t>(out)[i] = __builtin_bswap16(uint16_t(text[i]));
        GuestPtr<uint16_t>(out)[i] = 0;
    }
    FinishUi(ctx, ctx.r10.u32, ok ? X_ERROR_SUCCESS : X_ERROR_CANCELLED);
}

// Input ----------------------------------------------------------------------------------

// Guest users 0-3 are host XInput pads 0-3. User 0 also takes the keyboard
// (merged with pad 0 if one is connected), so the game is playable without
// a controller:
//   W / S        throttle / brake (right / left trigger)
//   A / D        steer (left stick)
//   arrow keys   d-pad (menus)      I/J/K/L   right stick (camera)
//   Enter        A (select)         Backspace B (back)
//   Space        X (handbrake)      Y         Y
//   Q / E        left / right shoulder (gear down / up)
//   Tab          Back (view)        Escape    Start (pause)
// FM2_AUTOSTART taps START + A every 3 s for unattended test runs.
namespace
{
struct PadState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

bool Key(int vk)
{
    return GetAsyncKeyState(vk) & 0x8000;
}

void AddKeyboardNow(PadState& s);

// The keys are read at most once a millisecond: the game asks for player 1 several times a frame,
// and each read is about twenty GetAsyncKeyState calls (about 1% of its main thread).
void AddKeyboard(PadState& s)
{
    static std::mutex mutex;
    static Clock::time_point at{};
    static PadState keys{};
    {
        std::lock_guard lock(mutex);
        if (Clock::now() - at > std::chrono::milliseconds(1))
        {
            keys = {};
            AddKeyboardNow(keys);
            at = Clock::now();
        }
    }
    s.buttons |= keys.buttons;
    if (keys.rightTrigger)
        s.rightTrigger = keys.rightTrigger;
    if (keys.leftTrigger)
        s.leftTrigger = keys.leftTrigger;
    if (keys.lx)
        s.lx = keys.lx;
    if (keys.rx)
        s.rx = keys.rx;
    if (keys.ry)
        s.ry = keys.ry;
}

void AddKeyboardNow(PadState& s)
{
    if (!PresenterHasFocus())
        return;
    struct Map
    {
        int vk;
        uint16_t button;
    };
    static const Map buttons[] = {
        { VK_UP, XINPUT_GAMEPAD_DPAD_UP }, { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN }, { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
        { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT }, { VK_ESCAPE, XINPUT_GAMEPAD_START }, { VK_TAB, XINPUT_GAMEPAD_BACK },
        { 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER }, { 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER }, { VK_RETURN, XINPUT_GAMEPAD_A },
        { VK_BACK, XINPUT_GAMEPAD_B }, { VK_SPACE, XINPUT_GAMEPAD_X }, { 'Y', XINPUT_GAMEPAD_Y },
    };
    for (const Map& m : buttons)
        if (Key(m.vk))
            s.buttons |= m.button;
    if (Key('W'))
        s.rightTrigger = 255;
    if (Key('S'))
        s.leftTrigger = 255;
    if (Key('A') != Key('D'))
        s.lx = Key('A') ? -32767 : 32767;
    if (Key('J') != Key('L'))
        s.rx = Key('J') ? -32767 : 32767;
    if (Key('I') != Key('K'))
        s.ry = Key('K') ? -32767 : 32767;
}

int g_xinputPads = 0; // connected XInput pads (PadSlot): SDL's pads (GamepadRead) are the players after them

DWORD PadSlot(uint32_t user)
{
    // Players are the connected pads in slot order: player 1 is the first connected pad whatever its
    // XInput slot. Only player 1 is signed in, and a pad left in slot 2 (it connected while another
    // pad or a virtual one held slot 1) otherwise played signed out: no profile, no saves. The slots
    // are looked up again every second (polling empty slots is slow).
    static std::mutex slotMutex;
    static int slotOf[XUSER_MAX_COUNT] = { 0, 1, 2, 3 };
    static Clock::time_point slotsChecked{};
    {
        std::lock_guard lock(slotMutex);
        const auto now = Clock::now();
        if (now - slotsChecked > std::chrono::seconds(1))
        {
            slotsChecked = now;
            int n = 0;
            XINPUT_STATE probe{};
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; slot++)
                if (XInputGetState(slot, &probe) == ERROR_SUCCESS)
                    slotOf[n++] = int(slot);
            g_xinputPads = n;
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT && n < int(XUSER_MAX_COUNT); slot++)
                if (std::find(slotOf, slotOf + n, int(slot)) == slotOf + n)
                    slotOf[n++] = int(slot);
        }
    }
    std::lock_guard lock(slotMutex);
    return DWORD(slotOf[user < XUSER_MAX_COUNT ? user : 0]);
}

// XInputGetState for this user's slot. Only slots the once-a-second probe found connected are asked
// (an empty slot is the slow call: the game asks for all four users several times a frame, and
// that was 2-3% of its main thread), and a slot's state is reused for a millisecond.
bool CachedXInputState(uint32_t user, XINPUT_STATE& x)
{
    const DWORD slot = PadSlot(user);
    if (int(user) >= g_xinputPads)
        return false;
    struct Cached { Clock::time_point at{}; XINPUT_STATE state{}; bool ok = false; };
    static Cached cache[XUSER_MAX_COUNT];
    static std::mutex cacheMutex;
    std::lock_guard lock(cacheMutex);
    Cached& c = cache[slot];
    const auto now = Clock::now();
    if (now - c.at > std::chrono::milliseconds(1))
    {
        c.ok = XInputGetState(slot, &c.state) == ERROR_SUCCESS;
        c.at = now;
    }
    x = c.state;
    return c.ok;
}

// Returns false if nothing is connected for this user.
bool ReadPad(uint32_t user, PadState& s)
{
    bool connected = false;
    XINPUT_STATE x{};
    if (user < XUSER_MAX_COUNT && CachedXInputState(user, x))
    {
        connected = true;
        s.buttons = x.Gamepad.wButtons; // same bit layout as the 360
        s.leftTrigger = x.Gamepad.bLeftTrigger;
        s.rightTrigger = x.Gamepad.bRightTrigger;
        s.lx = x.Gamepad.sThumbLX;
        s.ly = x.Gamepad.sThumbLY;
        s.rx = x.Gamepad.sThumbRX;
        s.ry = x.Gamepad.sThumbRY;
        if (s.buttons || s.leftTrigger > 64 || s.rightTrigger > 64 || std::abs(s.lx) > 16000 || std::abs(s.ly) > 16000)
        {
            NoteXInputPadUsed();
            PresenterControllerUsed();
        }
    }
    // A DualSense, DualShock 4 or other pad XInput doesn't see (input/wheel.cpp).
    else if (user < XUSER_MAX_COUNT && int(user) >= g_xinputPads)
        if (const GamepadInput g = GamepadRead(user - uint32_t(g_xinputPads)); g.present)
        {
            connected = true;
            s.buttons = g.buttons;
            s.leftTrigger = g.leftTrigger;
            s.rightTrigger = g.rightTrigger;
            s.lx = g.lx;
            s.ly = g.ly;
            s.rx = g.rx;
            s.ry = g.ry;
            if (s.buttons || s.leftTrigger > 64 || s.rightTrigger > 64 || std::abs(s.lx) > 16000 || std::abs(s.ly) > 16000)
                PresenterControllerUsed();
        }
    if (user == 0)
    {
        connected = true;
        AddKeyboard(s);
        // A racing wheel (input/wheel.cpp): its steering replaces the left stick's X, its pedals
        // and buttons add to the pad's.
        if (const WheelInput w = WheelRead(); w.present)
        {
            s.lx = w.steer;
            s.leftTrigger = std::max(s.leftTrigger, w.brake);
            s.rightTrigger = std::max(s.rightTrigger, w.gas);
            s.buttons |= w.buttons;
        }
        static LiveSwitch autoStart("FM2_AUTOSTART");
        static const auto start = Clock::now();
        // FM2_INPUT_SCRIPT="7:START,10:A,14:RIGHT,...": press each button for
        // 150 ms at that many seconds after boot (repro runs).
        static const std::vector<std::pair<double, uint16_t>> script = [] {
            std::vector<std::pair<double, uint16_t>> v;
            static const std::pair<const char*, uint16_t> names[] = { { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B },
                { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y }, { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK },
                { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT },
                { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER } };
            const char* s = getenv("FM2_INPUT_SCRIPT");
            while (s && *s)
            {
                char* end;
                const double t = strtod(s, &end);
                if (*end != ':')
                    break;
                const char* name = end + 1;
                size_t len = strcspn(name, ",");
                if (len == 2 && strncmp(name, "RT", 2) == 0) // "45:RT": right trigger held from 45 s on
                    v.emplace_back(t, 0xFFFF);
                for (auto& [n, b] : names)
                    if (strlen(n) == len && strncmp(n, name, len) == 0)
                        v.emplace_back(t, b);
                s = name + len + (name[len] == ',' ? 1 : 0);
            }
            return v;
        }();
        // FM2_TOUR=<steps>: hands-off route through the menus and races (shader tour).
        // Steps (comma separated): a button name (A, B, X, Y, START, BACK, UP, DOWN, LEFT,
        // RIGHT, LB, RB), "WAIT:<seconds>", "IDLE" (wait until the renderer has no compile
        // running; at least 2 s), "RT:<seconds>" (hold the right trigger), "STEER:<seconds>" (throttle,
        // steering right then left), "RACE:<seconds>"
        // (the benchmark: the right trigger held, A tapped every 2 s for the first 10 s in case a slow
        // PC was still loading when the race start was pressed, measured from 12 s), "QUIT" (save
        // caches and exit). Each button press lasts at least 150 ms and 3 reads, then a 600 ms gap.
        {
            static const char* tourEnv = getenv("FM2_TOUR");
            struct Tour
            {
                std::vector<std::string> steps;
                size_t index = 0;
                Clock::time_point stepStart{};
                bool started = false;
                int downPolls = 0, upPolls = 0; // reads of the current button press
                double releasedAt = 0;
                std::string Step() const { return index < steps.size() ? steps[index] : std::string(); }
            };
            static Tour tour = [&] {
                Tour t;
                for (const char* p = tourEnv ? tourEnv : ""; p && *p;)
                {
                    const size_t len = strcspn(p, ",");
                    if (len)
                        t.steps.emplace_back(p, len);
                    p += len + (p[len] == ',' ? 1 : 0);
                }
                return t;
            }();
            static std::mutex tourMutex;
            if (!tour.steps.empty())
            {
                std::lock_guard lock(tourMutex);
                const auto now = Clock::now();
                if (!tour.started)
                {
                    tour.started = true;
                    tour.stepStart = now;
                }
                static const std::pair<const char*, uint16_t> names[] = { { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B },
                    { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y }, { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK },
                    { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT },
                    { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER } };
                const double elapsed = std::chrono::duration<double>(now - tour.stepStart).count();
                const std::string step = tour.Step();
                bool advance = false;
                if (step.empty() || step == "QUIT")
                {
                    if (step == "QUIT")
                    {
                        static const char* benchmark = getenv("FM2_BENCHMARK"); // report path
                        if (benchmark)
                        {
                            BenchmarkRecord(false);
                            BenchmarkWriteReport(benchmark);
                        }
                        RendererSaveCaches();
                        Log("tour: finished, caches saved");
                        WheelStop();
                        fflush(nullptr);
                        ExitProcess(0);
                    }
                }
                else if (step.rfind("WAIT:", 0) == 0)
                    advance = elapsed >= atof(step.c_str() + 5);
                else if (step == "IDLE")
                {
                    advance = elapsed >= 2.0 && RendererCompileIdle();
                }
                else if (step.rfind("RACE:", 0) == 0)
                {
                    s.rightTrigger = 255;
                    if (elapsed < 10.0 && fmod(elapsed, 2.0) < 0.2)
                        s.buttons |= XINPUT_GAMEPAD_A;
                    advance = elapsed >= atof(step.c_str() + 5);
                    BenchmarkRecord(!advance && elapsed >= 12.0);
                }
                else if (step.rfind("RT:", 0) == 0)
                {
                    s.rightTrigger = 255;
                    advance = elapsed >= atof(step.c_str() + 3);
                    // The benchmark measures the race, from 3 s after the throttle is held.
                    BenchmarkRecord(!advance && elapsed >= 3.0);
                }
                else if (step.rfind("LS:", 0) == 0 || step.rfind("RS:", 0) == 0)
                {
                    // "LS:2" / "RS:2": left stick up / right stick right, held that long (moves Photo Mode's camera)
                    (step[0] == 'L' ? s.ly : s.rx) = 24000;
                    advance = elapsed >= atof(step.c_str() + 3);
                }
                else if (step.rfind("STEER:", 0) == 0)
                {
                    // "STEER:5": throttle held and steering right, then left, half the time each
                    // (the wheel's steering force in a race)
                    const double length = atof(step.c_str() + 6);
                    s.rightTrigger = 255;
                    s.lx = elapsed < length / 2 ? 20000 : -20000;
                    advance = elapsed >= length;
                }
                else
                {
                    // Held until the game has read it down a few times, then released until it has
                    // read it up a few times: a slow PC can stall past a fixed 150 ms and miss it.
                    if (tour.downPolls < 3 || elapsed < 0.15)
                    {
                        for (auto& [n, b] : names)
                            if (step == n)
                                s.buttons |= b;
                        tour.downPolls++;
                        tour.releasedAt = elapsed;
                    }
                    else
                        tour.upPolls++;
                    advance = tour.upPolls >= 3 && elapsed - tour.releasedAt >= 0.6;
                }
                if (advance)
                {
                    tour.index++;
                    tour.stepStart = now;
                    tour.downPolls = tour.upPolls = 0;
                    Log("tour: step %zu/%zu %s", tour.index, tour.steps.size(), tour.Step().c_str());
                }
            }
        }
        if (!script.empty())
        {
            const double now = std::chrono::duration<double>(Clock::now() - start).count();
            for (auto& [t, b] : script)
                if (b == 0xFFFF)
                {
                    if (now >= t)
                        s.rightTrigger = 255;
                }
                else if (now >= t && now < t + 0.15)
                    s.buttons |= b;
        }
        if (autoStart)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            if (ms % 3000 > 2800)
                s.buttons |= XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A;
        }
        // FM2_RECORD_INPUT=<file>: write player 1's input (pad and keyboard) as it changes, with
        // the time since the boot's shader warm-up ended (g_inputEpochNs; nothing before it). FM2_REPLAY_INPUT=<file> plays such a recording back instead of the
        // live input (then holds its last state; FM2_REPLAY_EXIT=<seconds> quits that long after
        // its end), so a route to a rendering bug can be repeated without a person.
        if (const int64_t epoch = g_inputEpochNs.load(); epoch)
        {
            const double now = (std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() - epoch) / 1e9;
            static FILE* record = getenv("FM2_RECORD_INPUT") ? fopen(getenv("FM2_RECORD_INPUT"), "w") : nullptr;
            struct Sample
            {
                double t;
                PadState p;
            };
            static const std::vector<Sample> replay = [] {
                std::vector<Sample> v;
                if (FILE* f = getenv("FM2_REPLAY_INPUT") ? fopen(getenv("FM2_REPLAY_INPUT"), "r") : nullptr)
                {
                    Sample x;
                    int b, lt, rt, lx, ly, rx, ry;
                    while (fscanf(f, "%lf %x %d %d %d %d %d %d", &x.t, &b, &lt, &rt, &lx, &ly, &rx, &ry) == 8)
                    {
                        x.p.buttons = uint16_t(b);
                        x.p.leftTrigger = uint8_t(lt);
                        x.p.rightTrigger = uint8_t(rt);
                        x.p.lx = int16_t(lx), x.p.ly = int16_t(ly), x.p.rx = int16_t(rx), x.p.ry = int16_t(ry);
                        v.push_back(x);
                    }
                    fclose(f);
                    Log("input: replaying %zu changes over %.1f s", v.size(), v.empty() ? 0.0 : v.back().t);
                }
                return v;
            }();
            if (!replay.empty())
            {
                size_t i = 0;
                while (i + 1 < replay.size() && replay[i + 1].t <= now)
                    i++;
                s = now >= replay[0].t ? replay[i].p : PadState{};
                static const double exitAfter = getenv("FM2_REPLAY_EXIT") ? atof(getenv("FM2_REPLAY_EXIT")) : -1;
                if (exitAfter >= 0 && now > replay.back().t + exitAfter)
                {
                    Log("input: replay finished");
                    RendererSaveCaches();
                    fflush(nullptr);
                    ExitProcess(0);
                }
            }
            else if (record)
            {
                static PadState last{};
                static bool first = true;
                if (first || memcmp(&last, &s, sizeof(s)) != 0)
                {
                    fprintf(record, "%.3f %04X %u %u %d %d %d %d\n", now, s.buttons, s.leftTrigger, s.rightTrigger, s.lx, s.ly, s.rx, s.ry);
                    fflush(record);
                    last = s;
                    first = false;
                }
            }
        }
    }
    // The debug server's input (debug.cpp) on top of everything else.
    if (DebugPad d; user == 0 && DebugPadState(d))
    {
        connected = true;
        s.buttons |= d.buttons;
        if (d.triggers)
            s.leftTrigger = d.leftTrigger, s.rightTrigger = d.rightTrigger;
        if (d.sticks)
            s.lx = d.lx, s.ly = d.ly, s.rx = d.rx, s.ry = d.ry;
    }
    return connected;
}
}

GUEST_EXPORT(XamInputGetCapabilities)
{
    // (user, flags, XINPUT_CAPABILITIES*)
    PadState s;
    if (!ReadPad(ctx.r3.u32, s))
    {
        Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    const uint32_t caps = ctx.r5.u32;
    memset(GuestPtr(caps), 0, 0x14);
    *GuestPtr<uint8_t>(caps + 0) = 1; // XINPUT_DEVTYPE_GAMEPAD
    *GuestPtr<uint8_t>(caps + 1) = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
    if (ctx.r3.u32 == 0 && (WheelFakeEnabled() || WheelRead().present))
    {
        // A racing wheel (or FM2_FAKE_WHEEL) as an Xbox 360 wheel, with force feedback when it
        // has some: the game then starts its XInputdFF* effects (kernel/wheel_ff.cpp).
        *GuestPtr<uint8_t>(caps + 1) = 2; // XINPUT_DEVSUBTYPE_WHEEL
        if (WheelFakeEnabled() || WheelHasForceFeedback())
            *GuestPtr<uint8_t>(caps + 3) = 1; // flags (big-endian): XINPUT_CAPS_FFB_SUPPORTED
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamInputGetState)
{
    // (user, flags, XINPUT_STATE*): packet, buttons, triggers, sticks (big-endian).
    PadState s;
    if (!ReadPad(ctx.r3.u32, s))
    {
        Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    // FM2_LOOK_BACK_AT=<seconds>: from then on the right stick is held down (the chase camera
    // looks back at the car's front), over a replay too: for captures of the bonnet.
    if (static const double lookBack = getenv("FM2_LOOK_BACK_AT") ? atof(getenv("FM2_LOOK_BACK_AT")) : 0; lookBack > 0)
    {
        static const auto start = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= lookBack)
            s.ry = -32767;
    }
    static std::atomic<uint32_t> packet{ 0 };
    const uint32_t state = ctx.r5.u32;
    StoreBE32(state, ++packet);
    *GuestPtr<uint16_t>(state + 4) = __builtin_bswap16(s.buttons);
    *GuestPtr<uint8_t>(state + 6) = s.leftTrigger;
    *GuestPtr<uint8_t>(state + 7) = s.rightTrigger;
    *GuestPtr<uint16_t>(state + 8) = __builtin_bswap16(uint16_t(s.lx));
    *GuestPtr<uint16_t>(state + 10) = __builtin_bswap16(uint16_t(s.ly));
    *GuestPtr<uint16_t>(state + 12) = __builtin_bswap16(uint16_t(s.rx));
    *GuestPtr<uint16_t>(state + 14) = __builtin_bswap16(uint16_t(s.ry));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamInputSetState)
{
    // (user, flags, XINPUT_VIBRATION*): left and right motor speeds, big-endian.
    const uint32_t user = ctx.r3.u32;
    PadState s;
    if (!ReadPad(user, s))
    {
        Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (ctx.r5.u32 && user < XUSER_MAX_COUNT)
    {
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = __builtin_bswap16(*GuestPtr<uint16_t>(ctx.r5.u32));
        v.wRightMotorSpeed = __builtin_bswap16(*GuestPtr<uint16_t>(ctx.r5.u32 + 2));
        if (int(user) < g_xinputPads)
            XInputSetState(PadSlot(user), &v);
        else
            GamepadRumble(user - uint32_t(g_xinputPads), v.wLeftMotorSpeed, v.wRightMotorSpeed);
    }
    Return(ctx, X_ERROR_SUCCESS);
}

// Content packages (saves) --------------------------------------------------------------

GUEST_EXPORT(XamContentCreateEx)
{
    // (user, rootName, XCONTENT_DATA*, flags, disposition*, license*, cacheSize, contentSize (u64), overlapped)
    const std::string root = GuestString(ctx.r4.u32);
    const uint32_t data = ctx.r5.u32;
    // XCONTENT_DATA: deviceId, contentType, displayName[128] (wide), fileName[42].
    const std::string fileName(GuestPtr<char>(data + 8 + 256), strnlen(GuestPtr<char>(data + 8 + 256), 42));
    const uint32_t contentType = LoadBE32(data + 4);
    std::filesystem::path dir = ContentTypeDir(contentType) / fileName;
    std::error_code ec;
    // A downloadable package installed in the shared folder: mounted read-only from there.
    const bool shared = contentType == 2 && std::filesystem::is_directory(DlcDir() / fileName, ec);
    if (shared)
        dir = DlcPackageRoot(DlcDir() / fileName);
    const bool existed = shared || std::filesystem::exists(dir, ec);
    const uint32_t flags = ctx.r6.u32;
    const uint32_t mode = flags & 0xF; // 1 create new, 2 create always, 3 open, 4 open always
    if (!existed && mode == 3)
    {
        FinishUi(ctx, StackArg(ctx, 8), X_ERROR_FILE_NOT_FOUND);
        return;
    }
    if (!shared)
        std::filesystem::create_directories(dir, ec);
    if (!existed)
    {
        std::u16string title;
        for (uint32_t i = 0; i < 128; i++)
        {
            const uint16_t c = __builtin_bswap16(*GuestPtr<uint16_t>(data + 8 + i * 2));
            if (!c)
                break;
            title += char16_t(c);
        }
        std::ofstream(TitlePath(contentType, fileName), std::ios::binary).write(reinterpret_cast<const char*>(title.data()), title.size() * 2);
    }
    const std::string device = shared ? "\\Device\\Dlc\\" + fileName
                                      : "\\Device\\Content\\" + dir.parent_path().filename().string() + "\\" + fileName;
    VfsAddLink(root + ":", device);
    Log("content: %s: -> %s", root.c_str(), dir.string().c_str());
    StoreIfNonNull(ctx.r7.u32, existed ? 2 : 1); // XCONTENT_OPENED_EXISTING : XCONTENT_CREATED_NEW
    // The license mask of a downloadable package: titles skip content that is not licensed. All bits
    // are set: the header's mask (<package>.license, tools/stfs.py) depends on how and where the
    // package was bought, and FM2 hid ten of the December Car Pack's eleven cars from a copy whose
    // header said 00000001 instead of 00000003 (report #7).
    StoreIfNonNull(ctx.r8.u32, shared ? 0xFFFFFFFFu : 0u);
    FinishUi(ctx, StackArg(ctx, 8));
}

GUEST_EXPORT(XamContentClose)
{
    VfsRemoveLink(GuestString(ctx.r3.u32) + ":");
    FinishUi(ctx, ctx.r4.u32);
}

GUEST_EXPORT(XamContentFlush) { FinishUi(ctx, ctx.r4.u32); }
GUEST_EXPORT(XamContentDelete)
{
    // (user, XCONTENT_DATA*, overlapped)
    const uint32_t data = ctx.r4.u32;
    const std::string fileName(GuestPtr<char>(data + 8 + 256), strnlen(GuestPtr<char>(data + 8 + 256), 42));
    std::error_code ec;
    if (!fileName.empty())
    {
        const uint32_t type = LoadBE32(data + 4);
        std::filesystem::remove_all(ContentTypeDir(type) / fileName, ec);
        std::filesystem::remove(TitlePath(type, fileName), ec);
        Log("content: deleted %08X/%s", type, fileName.c_str());
    }
    FinishUi(ctx, ctx.r5.u32);
}

GUEST_EXPORT(XamContentGetCreator)
{
    // (user, data*, isCreator*, xuid*, overlapped)
    StoreIfNonNull(ctx.r5.u32, 1);
    if (ctx.r6.u32)
        StoreBE64(ctx.r6.u32, Xuid());
    FinishUi(ctx, ctx.r7.u32);
}

GUEST_EXPORT(XamContentGetDeviceState)
{
    FinishUi(ctx, ctx.r4.u32);
}

GUEST_EXPORT(XamContentGetDeviceData)
{
    // (deviceId, XDEVICE_DATA*)
    const uint32_t data = ctx.r4.u32;
    memset(GuestPtr(data), 0, 0x50);
    StoreBE32(data + 0, ctx.r3.u32);
    StoreBE32(data + 4, 1);                 // device type: HDD
    StoreBE64(data + 8, 20ull << 30);        // total bytes
    StoreBE64(data + 16, 16ull << 30);       // free bytes
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamContentCreateEnumerator)
{
    // (user, deviceId, contentType, flags, itemCount, bufferSize*, handle*): lists the
    // profile's packages of that type (all types for 0).
    auto e = std::make_shared<EnumeratorObject>();
    const uint32_t wanted = ctx.r5.u32;
    std::error_code ec;
    for (const auto& typeDir : std::filesystem::directory_iterator(ContentDir(), ec))
    {
        uint32_t type = 0;
        if (!typeDir.is_directory() || sscanf(typeDir.path().filename().string().c_str(), "%x", &type) != 1 || (wanted && type != wanted))
            continue;
        for (const auto& package : std::filesystem::directory_iterator(typeDir.path(), ec))
        {
            const std::string fileName = package.path().filename().string();
            if (!package.is_directory() || fileName.size() > 42)
                continue;
            ContentItem item{ type, {}, fileName };
            std::ifstream in(TitlePath(type, fileName), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), {});
            item.displayName.assign(reinterpret_cast<const char16_t*>(bytes.data()), bytes.size() / 2);
            if (item.displayName.empty())
                item.displayName.assign(fileName.begin(), fileName.end());
            e->items.push_back(std::move(item));
        }
    }
    // Downloadable content is shared by all profiles: <user dir>\dlc\<package>. Not with a mod that has the
    // DLC cars itself: its database lists them already, and each enabled pack would add them again.
    if ((!wanted || wanted == 2) && !VfsModsCarryDlc())
        for (const auto& package : std::filesystem::directory_iterator(DlcDir(), ec))
        {
            const std::string fileName = package.path().filename().string();
            if (!package.is_directory() || fileName.size() > 42)
                continue;
            UnpackDlcPatches(package.path());
            ContentItem item{ 2, {}, fileName };
            std::ifstream in(DlcDir() / (fileName + ".title"), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), {});
            item.displayName.assign(reinterpret_cast<const char16_t*>(bytes.data()), bytes.size() / 2);
            if (item.displayName.empty())
                item.displayName.assign(fileName.begin(), fileName.end());
            e->items.push_back(std::move(item));
        }
    Log("content: enumerating %zu packages of type %08X", e->items.size(), wanted);
    StoreIfNonNull(ctx.r8.u32, std::max(ctx.r7.u32, 1u) * kContentDataSize);
    StoreIfNonNull(ctx.r9.u32, CreateHandle(e));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamCreateEnumeratorHandle)
{
    StoreIfNonNull(StackArg(ctx, 8), CreateHandle(std::make_shared<EnumeratorObject>()));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamEnumerate)
{
    // (handle, flags, buffer, size, itemsReturned*, overlapped)
    auto e = std::dynamic_pointer_cast<EnumeratorObject>(GetHandle(ctx.r3.u32));
    uint32_t count = 0;
    if (e)
    {
        const uint32_t buffer = ctx.r5.u32, capacity = ctx.r6.u32 / kContentDataSize;
        for (; count < capacity && e->next < e->items.size(); count++, e->next++)
        {
            const ContentItem& item = e->items[e->next];
            const uint32_t d = buffer + count * kContentDataSize;
            memset(GuestPtr(d), 0, kContentDataSize);
            StoreBE32(d + 0, 1); // the hard drive
            StoreBE32(d + 4, item.type);
            for (size_t i = 0; i < item.displayName.size() && i < 127; i++)
                *GuestPtr<uint16_t>(d + 8 + uint32_t(i) * 2) = __builtin_bswap16(uint16_t(item.displayName[i]));
            memcpy(GuestPtr(d + 8 + 256), item.fileName.data(), item.fileName.size());
        }
    }
    StoreIfNonNull(ctx.r7.u32, count);
    FinishUi(ctx, ctx.r8.u32, count ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES);
}

// Tasks ----------------------------------------------------------------------------

GUEST_EXPORT(XamTaskSchedule)
{
    // (callback, message, unknown, handle*)
    auto t = CreateGuestThread(ctx.r3.u32, ctx.r4.u32, 0, 0x10000, false, "xam task");
    StoreIfNonNull(ctx.r6.u32, t->handle);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XamTaskShouldExit) { Return(ctx, 0); }
GUEST_EXPORT(XamTaskCloseHandle) { CloseHandle(ctx.r3.u32); Return(ctx, X_STATUS_SUCCESS); }

// Live, sessions, voice: offline ------------------------------------------------------

// XMsg to system apps. 0xFA is XMP, the music player: report it idle
// (no custom soundtrack) at full volume. Arguments arrive as a struct of
// big-endian dwords at r5.
GUEST_EXPORT(XMsgInProcessCall)
{
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, args = ctx.r5.u32;
    if (LiveTrace())
        LogXMsg("XMsgInProcessCall", app, message, args, 16, ctx.lr);
    if (app == 0xFC)
    {
        // XLiveBase: the title's profile manager starts with a request here (00058004) and
        // gives up if it fails: no sign-in tracking, so "No Gamer Profiles" and no saves.
        LOG_ONCE("XLiveBase message %08X answered with success", message);
        Return(ctx, 0);
        return;
    }
    if (app != 0xFA)
    {
        LOG_ONCE("XMsgInProcessCall to app %X (message %08X) not supported", app, message);
        Return(ctx, 0x80004005); // E_FAIL
        return;
    }
    switch (message)
    {
    case 0x00070009: // get status (client, status*)
        StoreIfNonNull(LoadBE32(args + 4), 0);
        break;
    case 0x0007000B: // get volume (client, float*)
    {
        const float one = 1.0f;
        uint32_t bits;
        memcpy(&bits, &one, 4);
        StoreIfNonNull(LoadBE32(args + 4), bits);
        break;
    }
    case 0x0007001B: // get dashboard init state (client, state*)
        StoreIfNonNull(LoadBE32(args + 4), 0);
        break;
    default:
        LOG_ONCE("XMP message %08X answered with success", message);
        break;
    }
    Return(ctx, 0);
}
namespace
{
// An XMsg request: the XGI app's (0xFB) session and stats messages under FM2_LIVE
// (kernel/sessions.cpp); everything else fails as before.
void XMsgRequest(PPCContext& ctx, const char* call)
{
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32, buffer = ctx.r6.u32, size = ctx.r7.u32;
    if (!LiveTrace())
    {
        FinishUi(ctx, overlapped, X_ERROR_FUNCTION_FAILED);
        return;
    }
    LogXMsg(call, app, message, buffer, size, ctx.lr);
    uint32_t result = X_ERROR_FUNCTION_FAILED;
    if (app == 0xFB && OnlineXgiMessage(message, buffer, size, result))
    {
        FinishUi(ctx, overlapped, result);
        return;
    }
    FinishUi(ctx, overlapped, X_ERROR_FUNCTION_FAILED);
}
}
// (app, message, overlapped, buffer, size)
GUEST_EXPORT(XMsgStartIORequest) { XMsgRequest(ctx, "XMsgStartIORequest"); }
GUEST_EXPORT(XMsgStartIORequestEx) { XMsgRequest(ctx, "XMsgStartIORequestEx"); }
GUEST_EXPORT(XMsgCancelIORequest) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamSessionCreateHandle)
{
    // (handle*)
    StoreIfNonNull(ctx.r3.u32, LiveTrace() ? OnlineSessionCreateHandle() : CreateHandle(std::make_shared<EnumeratorObject>()));
    Return(ctx, X_ERROR_SUCCESS);
}
GUEST_EXPORT(XamSessionRefObjByHandle)
{
    // (handle, X_KSESSION**)
    const uint32_t obj = LiveTrace() ? OnlineSessionObject(ctx.r3.u32) : 0;
    StoreIfNonNull(ctx.r4.u32, obj);
    Return(ctx, obj ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED);
}
GUEST_EXPORT(XamVoiceCreate) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }
GUEST_EXPORT(XamVoiceClose) { Return(ctx, 0); }
GUEST_EXPORT(XamVoiceHeadsetPresent) { Return(ctx, 0); }
GUEST_EXPORT(XamVoiceSubmitPacket) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }

// Keys and signing: report every signature as valid ---------------------------------------

GUEST_EXPORT(XeKeysConsolePrivateKeySign)
{
    memset(GuestPtr(ctx.r4.u32), 0, 0x228);
    Return(ctx, 1);
}

GUEST_EXPORT(XeKeysConsoleSignatureVerification)
{
    StoreIfNonNull(ctx.r5.u32, 0);
    Return(ctx, 1);
}

// Network: no link. Every call fails cleanly ------------------------------------------

GUEST_EXPORT(NetDll_XNetStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetCleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSACleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAGetLastError) { Return(ctx, 10050); } // WSAENETDOWN
// FM2_LIVE: the link is up and online (XNET_ETHERNET_LINK_ACTIVE | 100 Mbps | full duplex), with
// an address (Ethernet, static, gateway, DNS, online): this PC (kernel/sessions.cpp).
GUEST_EXPORT(NetDll_XNetGetEthernetLinkStatus) { Return(ctx, LiveTrace() ? 0x1 | 0x2 | 0x8 : 0); }
GUEST_EXPORT(NetDll_XNetGetTitleXnAddr)
{
    // (caller, xnaddr*)
    if (!LiveTrace())
    {
        Return(ctx, 1); // XNET_GET_XNADDR_NONE
        return;
    }
    OnlineSelfXnAddr(ctx.r4.u32);
    Return(ctx, 0x2 | 0x4 | 0x20 | 0x40 | 0x80);
}
GUEST_EXPORT(NetDll_XNetGetConnectStatus) { Return(ctx, LiveTrace() ? 2 : 3); } // connected / lost
GUEST_EXPORT(NetDll_socket)
{
    // (caller, af, type, protocol)
    if (LiveTrace())
    {
        static std::atomic<int> logged{ 0 };
        if (logged++ < 20)
            Log("live: socket(af %u, type %u, protocol %u) (lr %08X): refused", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    }
    Return(ctx, 0xFFFFFFFF);
}
GUEST_EXPORT(NetDll_closesocket) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetCreateKey)
{
    // (caller, XNKID*, XNKEY*): a random system link key pair (an id with the top byte 0)
    for (uint32_t i = 0; i < 8; i++)
        *GuestPtr<uint8_t>(ctx.r4.u32 + i) = i == 0 ? 0 : uint8_t(rand());
    for (uint32_t i = 0; i < 16; i++)
        *GuestPtr<uint8_t>(ctx.r5.u32 + i) = uint8_t(rand());
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_XNetRandom)
{
    // (caller, buffer, length)
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        GuestPtr<uint8_t>(ctx.r4.u32)[i] = uint8_t(rand());
    Return(ctx, 0);
}
