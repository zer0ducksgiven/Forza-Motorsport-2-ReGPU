#pragma once
// The Xbox 360 wheel's force feedback as FM2 drives it through the kernel's XInputdFF* calls:
// effect blocks, their state and how strongly each plays, without SDL (input/wheel.cpp plays
// them). From the Forza Horizon recomp (github.com/Dyotson/ForzaHorizon1-ReImagined, rexglue-sdk
// patch 0101, include/rex/input/wheel/wheel_ff.h, BSD 3-Clause), the same engine; FM2 sends the
// same five blocks (kernel/wheel_ff.cpp).
//
// An effect is 29 big-endian words: block (1-based), type, a word the game sets to 4, gain
// (0-255), seven fixed words, then the type's parameters from word 11. Types in the HID PID order:
// 1 constant, 8 spring, 9 damper, 10 inertia, 11 friction. The constant force's word 11 is the
// steering force, -127 to 127, positive turning the wheel right; a condition's words 11-16 are
// its centre, the coefficients on the positive and negative side (-127 to 127, positive pulls
// back to the centre), the saturations on each side and the dead band (0-255).
#include <algorithm>
#include <cstdint>

namespace wheel
{
constexpr uint32_t kConstant = 1, kSpring = 8, kDamper = 9, kInertia = 10, kFriction = 11;
constexpr uint32_t kEffectWords = 29; // 0x74 bytes
constexpr uint32_t kMaxBlocks = 16;   // what GetDeviceInfo reports; FM2 uses 1-5
constexpr uint32_t kOpStop = 3;       // EffectOperation; the game starts with 2
// DeviceControl, in the HID PID order. The game sends 3 before it defines its effects.
constexpr uint32_t kEnableActuators = 1, kDisableActuators = 2, kStopAll = 3, kReset = 4, kPause = 5, kContinue = 6;

struct Effect
{
    uint32_t block = 0, type = 0, gain = 0;
    int32_t magnitude = 0; // constant: word 11
    int32_t center = 0;    // conditions: words 11-16
    int32_t posCoeff = 0, negCoeff = 0, posSat = 0, negSat = 0, deadband = 0;
};

// words: the effect's 29 words, already in host order.
inline Effect Decode(const uint32_t* w)
{
    Effect e;
    e.block = w[0];
    e.type = w[1];
    e.gain = w[3];
    e.magnitude = e.center = int32_t(w[11]);
    e.posCoeff = int32_t(w[12]);
    e.negCoeff = int32_t(w[13]);
    e.posSat = int32_t(w[14]);
    e.negSat = int32_t(w[15]);
    e.deadband = int32_t(w[16]);
    return e;
}

// What the game has asked of the device: its effect blocks and whether each plays.
class State
{
public:
    bool Define(const Effect& e)
    {
        if (e.block < 1 || e.block > kMaxBlocks)
            return false;
        fx_[e.block - 1] = e;
        defined_[e.block - 1] = true;
        return true;
    }
    bool Operate(uint32_t block, uint32_t op)
    {
        if (block < 1 || block > kMaxBlocks || !defined_[block - 1])
            return false;
        playing_[block - 1] = op != kOpStop;
        return true;
    }
    void Control(uint32_t command)
    {
        switch (command)
        {
        case kEnableActuators: actuators_ = true; break;
        case kDisableActuators: actuators_ = false; break;
        case kStopAll: std::fill(playing_, playing_ + kMaxBlocks, false); break;
        case kReset:
            std::fill(playing_, playing_ + kMaxBlocks, false);
            std::fill(defined_, defined_ + kMaxBlocks, false);
            paused_ = false;
            actuators_ = true;
            break;
        case kPause: paused_ = true; break;
        case kContinue: paused_ = false; break;
        default: break;
        }
    }
    void SetDeviceGain(uint32_t gain) { deviceGain_ = std::min<uint32_t>(gain, 255); }
    bool Defined(uint32_t block) const { return block >= 1 && block <= kMaxBlocks && defined_[block - 1]; }
    const Effect& Get(uint32_t block) const { return fx_[block - 1]; }
    // The block's effect should be felt now.
    bool Active(uint32_t block) const { return Defined(block) && playing_[block - 1] && actuators_ && !paused_; }
    uint32_t DeviceGain() const { return deviceGain_; }

private:
    Effect fx_[kMaxBlocks]{};
    bool defined_[kMaxBlocks]{};
    bool playing_[kMaxBlocks]{};
    bool actuators_ = true, paused_ = false;
    uint32_t deviceGain_ = 255;
};

// How strongly an effect plays, 0 to 1: its gain, the device gain and the player's strength.
inline float Scale(const Effect& e, uint32_t deviceGain, float strength)
{
    const float g = std::min<uint32_t>(e.gain, 255) / 255.0f * std::min<uint32_t>(deviceGain, 255) / 255.0f;
    return std::clamp(g * strength, 0.0f, 1.0f);
}

// The steering force, -1 to 1, positive turning the wheel right.
inline float ConstantLevel(const Effect& e, float scale)
{
    return std::clamp(e.magnitude / 127.0f, -1.0f, 1.0f) * scale;
}

// A condition on the steering axis as fractions of the full range.
struct Condition
{
    float rightCoeff = 0, leftCoeff = 0, rightSat = 0, leftSat = 0, deadband = 0, center = 0;
};
inline Condition ConditionOf(const Effect& e, float scale)
{
    Condition c;
    c.rightCoeff = std::clamp(e.posCoeff / 127.0f, -1.0f, 1.0f) * scale;
    c.leftCoeff = std::clamp(e.negCoeff / 127.0f, -1.0f, 1.0f) * scale;
    c.rightSat = std::clamp(e.posSat / 255.0f, 0.0f, 1.0f) * scale;
    c.leftSat = std::clamp(e.negSat / 255.0f, 0.0f, 1.0f) * scale;
    c.deadband = std::clamp(e.deadband / 255.0f, 0.0f, 1.0f);
    c.center = std::clamp(e.center / 127.0f, -1.0f, 1.0f);
    return c;
}
// True when the condition can push at all (the game's inertia and friction are all zero).
inline bool Felt(const Condition& c)
{
    return (c.rightCoeff != 0 && c.rightSat > 0) || (c.leftCoeff != 0 && c.leftSat > 0);
}
}
