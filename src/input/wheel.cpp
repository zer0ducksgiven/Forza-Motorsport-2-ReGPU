// Racing wheels through SDL3. One thread owns SDL: every 2 ms it reads the wheel and its pedals
// (player 1 as an Xbox 360 wheel: steering on the left stick's X, the pedals on the triggers) and
// plays the game's force feedback (input/wheel_effects.h) on the wheel's DirectInput effects.
//
// For direct drive wheels, which show every step of a force: the game sets its steering force
// once or twice a frame, and the thread eases towards each new value (kSmoothSeconds) instead of
// jumping to it. Forces fade in when they start, fade out while the player is not driving (menus,
// pauses and replays: hooks/wheel_driving.cpp), the game's window is not in front or the game has
// stopped updating them (a hang), and stop for good on exit (WheelStop).
//
// Settings (settings.ini, from the launcher; FM2_WHEEL_STRENGTH, _SPRING, _REVERSE and _ROTATION
// override them): the strength, the spring and damper on top of it, the steering force reversed,
// and the rotation the wheel is set to (steering is scaled to the 360 wheel's 270 degrees from it).
// Environment only:
//   FM2_WHEEL_GAS=p0, FM2_WHEEL_BRAKE=p1  the pedal axes: w (the wheel) or p (the pedals) and the
//                             axis number. By default the pedals' first two axes, or the wheel's
//                             axes after steering when the pedals plug into the wheel.
//   FM2_NO_WHEEL=1            SDL not started at all
// The log names every device, axis and button as they are first moved, to map a new wheel.
#include "input/wheel.h"

#include <SDL3/SDL.h>
#include <windows.h>
#include <Xinput.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "settings.h"
#include "kernel/runtime.h"

namespace
{
using namespace std::chrono;

constexpr float kSmoothSeconds = 0.010f; // the steering force eases to each new value over about this
constexpr float kFadeInSeconds = 0.5f, kFadeOutSeconds = 0.15f;
constexpr auto kStaleAfter = milliseconds(250); // no steering force update for this long: fade out
constexpr auto kDrivingOffAfter = milliseconds(50); // "not driving" this long before forces fade
// The game's steering force turns the wheel right when positive; an SDL level on the steering
// axis pulls left when positive (FH1's wheel_input_driver.cpp).
constexpr float kConstantToSdl = -1.0f;

// Vendors whose devices are wheels or pedals when SDL doesn't say (DirectInput often doesn't).
bool WheelVendor(uint16_t vid)
{
    switch (vid)
    {
    case 0x044F: // Thrustmaster
    case 0x046D: // Logitech
    case 0x0EB7: // Fanatec
    case 0x346E: // Moza
    case 0x3670: // Simagic
    case 0x16D0: // Simucube
    case 0x2433: // Asetek
    case 0x1DD2: // Leo Bodnar (pedal and button boxes)
        return true;
    default:
        return false;
    }
}

struct Device
{
    SDL_Joystick* joystick = nullptr;
    SDL_JoystickID id = 0;
    std::string name;
    uint16_t vid = 0, pid = 0;
    int axes = 0, buttons = 0, hats = 0;
    std::vector<int16_t> rest, logged; // each axis at rest (from SDL) and as last logged
    std::vector<bool> down;
};

// A pedal: an axis on the wheel or the pedals, pressed away from where it rests.
struct Axis
{
    char device = 0; // 'w' or 'p'
    int index = -1;
};

struct Driver
{
    std::mutex mutex; // everything below and every SDL call
    std::vector<Device> devices;
    int wheel = -1, pedals = -1; // into devices
    SDL_Haptic* haptic = nullptr;
    uint32_t features = 0;
    WheelInput input;
    bool stopped = false;

    // The game's force feedback and what each block's SDL effect was last given.
    wheel::State game;
    int fx[wheel::kMaxBlocks];
    SDL_HapticEffect sent[wheel::kMaxBlocks];
    bool running[wheel::kMaxBlocks] = {};
    float steering = 0;              // the eased steering force, -1 to 1
    steady_clock::time_point updated; // the game's last steering force
    float fade = 0;
    bool driving = false; // from the game: off in menus, pauses and replays
    steady_clock::time_point stoppedDriving; // when it last went off

    // From the settings when the thread starts.
    float strength = 0.5f, spring = 0.5f, reverse = 1, steerScale = 1;
};
Driver g_wheel;
std::atomic<bool> g_started{ false }, g_present{ false }, g_ff{ false };

Axis ParseAxis(const char* env)
{
    const char* v = getenv(env);
    if (!v || (v[0] != 'w' && v[0] != 'p'))
        return {};
    return { v[0], atoi(v + 1) };
}

const Device* Of(Driver& d, char which)
{
    const int i = which == 'w' ? d.wheel : d.pedals;
    return i >= 0 ? &d.devices[i] : nullptr;
}

// 0-255: how far the pedal is pressed from its rest.
uint8_t Pedal(Driver& d, Axis a)
{
    const Device* dev = Of(d, a.device);
    if (!dev || a.index < 0 || a.index >= dev->axes)
        return 0;
    const int v = SDL_GetJoystickAxis(dev->joystick, a.index), rest = dev->rest[a.index];
    return uint8_t(std::clamp(std::abs(v - rest) * 255 / 65535, 0, 255));
}

// The pedal axes: from FM2_WHEEL_GAS and FM2_WHEEL_BRAKE, else the first two axes resting at one
// end of their range (a released pedal; unused axes rest at 0) on the pedals, or on the wheel
// after steering when the pedals plug into it (a T598: axes 1, 5 and 6).
void PedalAxes(Driver& d, Axis& gas, Axis& brake)
{
    gas = ParseAxis("FM2_WHEEL_GAS");
    brake = ParseAxis("FM2_WHEEL_BRAKE");
    const char which = d.pedals >= 0 ? 'p' : 'w';
    std::vector<int> resting;
    if (const Device* dev = Of(d, which))
        for (int i = which == 'w' ? 1 : 0; i < dev->axes; i++)
            if (std::abs(dev->rest[i]) > 30000)
                resting.push_back(i);
    // Wheels whose pedals are known: vendor, product, gas axis, brake axis (on the wheel).
    static const struct { uint16_t vid, pid; int gas, brake; } known[] = {
        { 0x044F, 0xB696, 5, 1 }, // Thrustmaster T598 in PC mode ("Advanced Mode Racer")
    };
    if (which == 'w')
        for (const auto& k : known)
            if (d.devices[d.wheel].vid == k.vid && d.devices[d.wheel].pid == k.pid)
            {
                if (gas.index < 0)
                    gas = { 'w', k.gas };
                if (brake.index < 0)
                    brake = { 'w', k.brake };
            }
    if (gas.index < 0)
        gas = { which, resting.size() > 0 ? resting[0] : 1 };
    if (brake.index < 0)
        brake = { which, resting.size() > 1 ? resting[1] : 2 };
}

void LogMoves(Device& dev)
{
    static int lines = 0;
    for (int i = 0; i < dev.axes && lines < 300; i++)
    {
        const int v = SDL_GetJoystickAxis(dev.joystick, i);
        if (std::abs(v - dev.logged[i]) > 8000)
        {
            Log("wheel: %s axis %d = %d (rest %d)", dev.name.c_str(), i, v, dev.rest[i]);
            dev.logged[i] = int16_t(v);
            lines++;
        }
    }
    for (int i = 0; i < dev.buttons && lines < 300; i++)
    {
        const bool down = SDL_GetJoystickButton(dev.joystick, i);
        if (down != dev.down[i])
        {
            dev.down[i] = down;
            if (down)
            {
                Log("wheel: %s button %d", dev.name.c_str(), i);
                lines++;
            }
        }
    }
}

void ReadInput(Driver& d)
{
    for (Device& dev : d.devices)
        LogMoves(dev);
    WheelInput in;
    if (d.wheel >= 0)
    {
        const Device& w = d.devices[d.wheel];
        in.present = true;
        const float steer = SDL_GetJoystickAxis(w.joystick, 0) / 32767.0f * d.steerScale;
        in.steer = int16_t(std::lround(std::clamp(steer, -1.0f, 1.0f) * 32767.0f));
        Axis gas, brake;
        PedalAxes(d, gas, brake);
        in.gas = Pedal(d, gas);
        in.brake = Pedal(d, brake);
        // Known wheels' buttons (SDL button, pad button); the bumpers are the 360 wheel's shift
        // down (LB) and up (RB).
        struct Button
        {
            int sdl;
            uint16_t pad;
        };
        // T598 in PC mode, a PlayStation layout: Cross, Circle, Square, Triangle as A, B, X, Y,
        // the paddles and L2/R2 shift, Share and Options as Back and Start, L3/R3 as the stick clicks.
        static const Button t598[] = { { 5, XINPUT_GAMEPAD_A }, { 4, XINPUT_GAMEPAD_B }, { 3, XINPUT_GAMEPAD_X },
            { 2, XINPUT_GAMEPAD_Y }, { 0, XINPUT_GAMEPAD_LEFT_SHOULDER }, { 1, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { 9, XINPUT_GAMEPAD_LEFT_SHOULDER }, { 8, XINPUT_GAMEPAD_RIGHT_SHOULDER }, { 6, XINPUT_GAMEPAD_BACK },
            { 7, XINPUT_GAMEPAD_START }, { 21, XINPUT_GAMEPAD_LEFT_THUMB }, { 22, XINPUT_GAMEPAD_RIGHT_THUMB } };
        // Others, the usual wheel order (FH1's generic layout): face buttons, the right and left
        // paddle, then Back and Start.
        static const Button generic[] = { { 0, XINPUT_GAMEPAD_A }, { 1, XINPUT_GAMEPAD_B }, { 2, XINPUT_GAMEPAD_X },
            { 3, XINPUT_GAMEPAD_Y }, { 4, XINPUT_GAMEPAD_RIGHT_SHOULDER }, { 5, XINPUT_GAMEPAD_LEFT_SHOULDER },
            { 6, XINPUT_GAMEPAD_BACK }, { 7, XINPUT_GAMEPAD_START } };
        const bool isT598 = w.vid == 0x044F && w.pid == 0xB696;
        for (const Button& b : isT598 ? std::span<const Button>(t598) : std::span<const Button>(generic))
            if (b.sdl < w.buttons && SDL_GetJoystickButton(w.joystick, b.sdl))
                in.buttons |= b.pad;
        if (w.hats)
        {
            const Uint8 hat = SDL_GetJoystickHat(w.joystick, 0);
            in.buttons |= (hat & SDL_HAT_UP ? XINPUT_GAMEPAD_DPAD_UP : 0) | (hat & SDL_HAT_DOWN ? XINPUT_GAMEPAD_DPAD_DOWN : 0) |
                (hat & SDL_HAT_LEFT ? XINPUT_GAMEPAD_DPAD_LEFT : 0) | (hat & SDL_HAT_RIGHT ? XINPUT_GAMEPAD_DPAD_RIGHT : 0);
        }
    }
    d.input = in;
}

void CloseHaptic(Driver& d)
{
    if (!d.haptic)
        return;
    SDL_StopHapticEffects(d.haptic);
    SDL_CloseHaptic(d.haptic);
    d.haptic = nullptr;
    d.features = 0;
    std::fill(std::begin(d.fx), std::end(d.fx), -1);
    std::fill(std::begin(d.running), std::end(d.running), false);
    g_ff = false;
}

void OpenHaptic(Driver& d)
{
    const Device& w = d.devices[d.wheel];
    if (!SDL_IsJoystickHaptic(w.joystick) || !(d.haptic = SDL_OpenHapticFromJoystick(w.joystick)))
    {
        Log("wheel: %s has no force feedback SDL can drive (%s)", w.name.c_str(), SDL_GetError());
        return;
    }
    d.features = SDL_GetHapticFeatures(d.haptic);
    std::fill(std::begin(d.fx), std::end(d.fx), -1);
    // The game centres the wheel itself (its spring); the wheel's own centring would fight it.
    if (d.features & SDL_HAPTIC_AUTOCENTER)
        SDL_SetHapticAutocenter(d.haptic, 0);
    if (d.features & SDL_HAPTIC_GAIN)
        SDL_SetHapticGain(d.haptic, 100);
    Log("wheel: force feedback:%s%s%s%s%s%s%s, %d effects at once", d.features & SDL_HAPTIC_CONSTANT ? " constant" : "",
        d.features & SDL_HAPTIC_SPRING ? " spring" : "", d.features & SDL_HAPTIC_DAMPER ? " damper" : "",
        d.features & SDL_HAPTIC_INERTIA ? " inertia" : "", d.features & SDL_HAPTIC_FRICTION ? " friction" : "",
        d.features & SDL_HAPTIC_GAIN ? " gain" : "", d.features & SDL_HAPTIC_AUTOCENTER ? " autocentre" : "",
        SDL_GetMaxHapticEffectsPlaying(d.haptic));
    Log("wheel: strength %.0f%%, spring and damper %.0f%% of that%s, steering x%.2f", d.strength * 100, d.spring * 100,
        d.reverse < 0 ? ", steering force reversed" : "", d.steerScale);
    g_ff = true;
}

// For the launcher's Racing wheel panel: the wheel the game found (name, vendor:product, whether it
// has force feedback), or an empty file for none.
void RecordWheel(const Device* w, bool ff)
{
    if (FILE* f = _wfopen((UserDir() / "wheel.txt").c_str(), L"w"))
    {
        if (w)
            fprintf(f, "%s\n%04X:%04X\n%s\n", w->name.c_str(), w->vid, w->pid, ff ? "force feedback" : "no force feedback");
        fclose(f);
    }
}

void Added(Driver& d, SDL_JoystickID id)
{
    const uint16_t vid = SDL_GetJoystickVendorForID(id), pid = SDL_GetJoystickProductForID(id);
    const SDL_JoystickType type = SDL_GetJoystickTypeForID(id);
    const char* name = SDL_GetJoystickNameForID(id);
    const bool wheelType = type == SDL_JOYSTICK_TYPE_WHEEL, pedalType = type == SDL_JOYSTICK_TYPE_THROTTLE;
    if (!wheelType && !pedalType && !WheelVendor(vid))
        return;
    Device dev;
    if (!(dev.joystick = SDL_OpenJoystick(id)))
        return;
    dev.id = id;
    dev.name = name ? name : "?";
    dev.vid = vid, dev.pid = pid;
    dev.axes = SDL_GetNumJoystickAxes(dev.joystick);
    dev.buttons = SDL_GetNumJoystickButtons(dev.joystick);
    dev.hats = SDL_GetNumJoystickHats(dev.joystick);
    dev.rest.assign(std::max(dev.axes, 0), 0);
    for (int i = 0; i < dev.axes; i++)
        if (!SDL_GetJoystickAxisInitialState(dev.joystick, i, &dev.rest[i]))
            dev.rest[i] = SDL_GetJoystickAxis(dev.joystick, i);
    dev.logged = dev.rest;
    dev.down.assign(std::max(dev.buttons, 0), false);
    const bool haptic = SDL_IsJoystickHaptic(dev.joystick);
    std::string rests;
    for (int16_t r : dev.rest)
        rests += " " + std::to_string(r);
    Log("wheel: found %s (%04X:%04X, type %d): %d axes (at rest:%s), %d buttons, %d hats%s", dev.name.c_str(), vid, pid, int(type),
        dev.axes, rests.c_str(), dev.buttons, dev.hats, haptic ? ", force feedback" : "");
    d.devices.push_back(dev);
    const int index = int(d.devices.size()) - 1;
    // The wheel: the first with force feedback or SDL's wheel type; any other is the pedals.
    if (d.wheel < 0 && (haptic || wheelType))
    {
        d.wheel = index;
        OpenHaptic(d);
    }
    else if (d.pedals < 0)
        d.pedals = index;
    if (d.wheel >= 0)
    {
        Axis gas, brake;
        PedalAxes(d, gas, brake);
        Log("wheel: steering %s axis 0, gas %c%d, brake %c%d", d.devices[d.wheel].name.c_str(), gas.device, gas.index, brake.device,
            brake.index);
        RecordWheel(&d.devices[d.wheel], d.haptic != nullptr);
    }
    g_present = d.wheel >= 0;
}

void Removed(Driver& d, SDL_JoystickID id)
{
    for (size_t i = 0; i < d.devices.size(); i++)
        if (d.devices[i].id == id)
        {
            Log("wheel: %s disconnected", d.devices[i].name.c_str());
            if (int(i) == d.wheel)
                CloseHaptic(d);
            SDL_CloseJoystick(d.devices[i].joystick);
            d.devices.erase(d.devices.begin() + i);
            auto fix = [&](int& which) { which = which == int(i) ? -1 : which > int(i) ? which - 1 : which; };
            fix(d.wheel);
            fix(d.pedals);
            break;
        }
    g_present = d.wheel >= 0;
    d.input.present = g_present;
}

bool Focused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// One of the game's blocks onto its SDL effect: created, updated, started or stopped.
void Apply(Driver& d, uint32_t block, float dt)
{
    const uint32_t i = block - 1;
    SDL_HapticEffect fx;
    memset(&fx, 0, sizeof(fx)); // compared whole with the last one sent
    bool play = false;
    if (d.game.Defined(block))
    {
        const wheel::Effect& e = d.game.Get(block);
        const bool centring = e.type == wheel::kSpring || e.type == wheel::kDamper;
        const float scale = wheel::Scale(e, d.game.DeviceGain(), d.strength * (centring ? d.spring : 1.0f)) * d.fade;
        Uint16 condition = 0;
        switch (e.type)
        {
        case wheel::kSpring: condition = SDL_HAPTIC_SPRING; break;
        case wheel::kDamper: condition = SDL_HAPTIC_DAMPER; break;
        case wheel::kInertia: condition = SDL_HAPTIC_INERTIA; break;
        case wheel::kFriction: condition = SDL_HAPTIC_FRICTION; break;
        default: break;
        }
        if (e.type == wheel::kConstant && (d.features & SDL_HAPTIC_CONSTANT))
        {
            // Eased towards the game's value: a step every frame is felt on a direct drive wheel.
            const float target = d.game.Active(block) ? wheel::ConstantLevel(e, scale) : 0.0f;
            d.steering += (target - d.steering) * std::min(1.0f, dt / kSmoothSeconds);
            fx.type = SDL_HAPTIC_CONSTANT;
            fx.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
            fx.constant.length = SDL_HAPTIC_INFINITY;
            fx.constant.level = Sint16(std::lround(kConstantToSdl * d.reverse * d.steering * 32767.0f));
            play = d.game.Active(block);
            // Not resent for the last few units of easing.
            if (d.fx[i] >= 0 && std::abs(fx.constant.level - d.sent[i].constant.level) < 32 && fx.constant.level != 0)
                fx.constant.level = d.sent[i].constant.level;
        }
        else if (condition && (d.features & condition))
        {
            const wheel::Condition c = wheel::ConditionOf(e, scale);
            fx.type = condition;
            fx.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
            fx.condition.length = SDL_HAPTIC_INFINITY;
            fx.condition.right_sat[0] = Uint16(std::lround(c.rightSat * 65535.0f));
            fx.condition.left_sat[0] = Uint16(std::lround(c.leftSat * 65535.0f));
            fx.condition.right_coeff[0] = Sint16(std::lround(c.rightCoeff * 32767.0f));
            fx.condition.left_coeff[0] = Sint16(std::lround(c.leftCoeff * 32767.0f));
            fx.condition.deadband[0] = Uint16(std::lround(c.deadband * 65535.0f));
            fx.condition.center[0] = Sint16(std::lround(c.center * 32767.0f));
            play = d.game.Active(block) && wheel::Felt(c);
        }
    }
    int& id = d.fx[i];
    if (!play)
    {
        if (id >= 0 && d.running[i])
        {
            SDL_StopHapticEffect(d.haptic, id);
            d.running[i] = false;
        }
        return;
    }
    if (id >= 0 && d.sent[i].type != fx.type)
    {
        SDL_DestroyHapticEffect(d.haptic, id);
        id = -1;
        d.running[i] = false;
    }
    if (id < 0)
    {
        if ((id = SDL_CreateHapticEffect(d.haptic, &fx)) < 0)
        {
            static int warned = 0;
            if (warned++ < 10)
                Log("wheel: the game's effect block %u (SDL type %u) was refused: %s", block, fx.type, SDL_GetError());
            return;
        }
        d.sent[i] = fx;
    }
    else if (memcmp(&fx, &d.sent[i], sizeof(fx)) != 0 && SDL_UpdateHapticEffect(d.haptic, id, &fx))
        d.sent[i] = fx;
    if (!d.running[i])
        d.running[i] = SDL_RunHapticEffect(d.haptic, id, SDL_HAPTIC_INFINITY);
}

void Thread()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Wheels through DirectInput, where their force feedback is; controllers stay with XInput.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    Driver& d = g_wheel;
    {
        const Settings& st = GetSettings();
        std::lock_guard lock(d.mutex);
        d.strength = st.wheelStrength / 100.0f;
        d.spring = st.wheelSpring / 100.0f;
        d.reverse = st.wheelReverse ? -1.0f : 1.0f;
        // The 360 wheel turns 270 degrees: a wheel set to 1080 needs a quarter of its turn for full lock.
        d.steerScale = st.wheelRotation ? st.wheelRotation / 270.0f : 1.0f;
    }
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC))
    {
        Log("wheel: SDL did not start: %s", SDL_GetError());
        return;
    }
    Log("wheel: SDL %d.%d.%d, looking for wheels", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    auto last = steady_clock::now();
    for (bool first = true;; first = false)
    {
        const auto now = steady_clock::now();
        const float dt = duration<float>(now - last).count();
        last = now;
        {
            std::lock_guard lock(d.mutex);
            if (d.stopped)
                return;
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_EVENT_JOYSTICK_ADDED)
                    Added(d, ev.jdevice.which);
                else if (ev.type == SDL_EVENT_JOYSTICK_REMOVED)
                    Removed(d, ev.jdevice.which);
            // The devices already plugged in come as events on the first pass.
            if (first && d.wheel < 0)
            {
                Log("wheel: no racing wheel connected");
                RecordWheel(nullptr, false);
            }
            ReadInput(d);
            if (d.haptic)
            {
                // The game also turns driving off and back on within a fraction of a millisecond every
                // frame for a few seconds after a race loads; only an off that lasts counts.
                const bool driving = d.driving || now - d.stoppedDriving < kDrivingOffAfter;
                const bool live = driving && Focused() && now - d.updated < kStaleAfter;
                d.fade = live ? std::min(1.0f, d.fade + dt / kFadeInSeconds) : std::max(0.0f, d.fade - dt / kFadeOutSeconds);
                for (uint32_t b = 1; b <= wheel::kMaxBlocks; b++)
                    Apply(d, b, dt);
            }
        }
        std::this_thread::sleep_until(now + milliseconds(2));
    }
}
}

void WheelStart()
{
    if (getenv("FM2_NO_WHEEL") || g_started.exchange(true))
        return;
    std::fill(std::begin(g_wheel.fx), std::end(g_wheel.fx), -1);
    std::thread(Thread).detach();
}

void WheelStop()
{
    if (!g_started)
        return;
    // try_lock: a crash on the wheel thread itself holds the mutex.
    std::unique_lock lock(g_wheel.mutex, std::try_to_lock);
    g_wheel.stopped = true;
    if (lock.owns_lock())
        CloseHaptic(g_wheel);
    else if (g_wheel.haptic)
        SDL_StopHapticEffects(g_wheel.haptic);
}

WheelInput WheelRead()
{
    if (!g_present)
        return {};
    std::lock_guard lock(g_wheel.mutex);
    return g_wheel.input;
}

bool WheelHasForceFeedback()
{
    return g_ff;
}

void WheelFfDefine(const wheel::Effect& e)
{
    std::lock_guard lock(g_wheel.mutex);
    g_wheel.game.Define(e);
    if (e.type == wheel::kConstant)
        g_wheel.updated = steady_clock::now();
}

void WheelFfOperate(uint32_t block, uint32_t op)
{
    std::lock_guard lock(g_wheel.mutex);
    g_wheel.game.Operate(block, op);
}

void WheelFfControl(uint32_t command)
{
    std::lock_guard lock(g_wheel.mutex);
    g_wheel.game.Control(command);
}

void WheelSetDriving(bool driving)
{
    std::lock_guard lock(g_wheel.mutex);
    if (g_wheel.driving && !driving)
        g_wheel.stoppedDriving = steady_clock::now();
    g_wheel.driving = driving;
}

void WheelFfDeviceGain(uint32_t gain)
{
    std::lock_guard lock(g_wheel.mutex);
    g_wheel.game.SetDeviceGain(gain);
}
