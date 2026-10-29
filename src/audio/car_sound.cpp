// The car sound mix's rows on the game's Audio options screens (the options menu's and each race pause's):
// Exhaust and Turbo, under the game's own sliders, with the launcher's car sound mix on. The rows come from
// the screens' query (GameOptions, category 9) with two more added. The slider behaviour
// (AudioSliderBehavior.lua) reads ProfileManager.activePlayerProfile[token] (0..1) and moves it with
// :AudioIncrement(token) / :AudioDecrement(token); while the screen is open, ProfileManager is a stand-in
// that answers for the two tokens (0..1 is 0..400%) and passes the rest to the real one, and the next flow
// script to load puts the real one back. The Lua asks for the mix with "return 0 --[[carmix: get]]" and
// sends it back with the note "--carmix: set <exhaust> <turbo>" (settings.ini, and the car tuning zips as
// the next car loads: kernel/zip_overlay.cpp; the turbo's samples at once: CarSoundStreamGain).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "kernel/runtime.h"
#include "log.h"
#include "settings.h"

namespace
{
constexpr std::string_view kQuery = "SELECT * FROM GameOptionCategoryMap, GameOptions WHERE GroupId=4 AND CatId=9 AND OptionId=Id";
// GameOptionCategoryMap's six columns, then GameOptions' (Id, DisplayName, Token, isProperty, xblValue, Query,
// NumMin, NumMax), as the game's rows have them
constexpr std::string_view kRows = " UNION ALL SELECT 4, 9, 9001, 1, 1, 0, 9001, 'Exhaust', 'FM2ExhaustSounds', 1, -1, '', 0, 0"
                                   " UNION ALL SELECT 4, 9, 9002, 1, 1, 0, 9002, 'Turbo', 'FM2TurboSounds', 1, -1, '', 0, 0";
constexpr std::string_view kGet = "return 0 --[[carmix: get]]";
constexpr std::string_view kSet = "--carmix: set ";

// After the screen's query: ProfileManager's stand-in.
constexpr const char kStandIn[] = R"lua(
do
    local mix = loadstring("return 0 --[[carmix: get]]")()
    if type(mix) == "table" and not FM2CarMix then
        local real = ProfileManager
        local values = { FM2ExhaustSounds = mix[1] / 400, FM2TurboSounds = mix[2] / 400 }
        local steps = { FM2ExhaustSounds = 1 / 16, FM2TurboSounds = 1 / 16 } -- 25%
        local function send()
            loadstring(string.format("--carmix: set %d %d", math.floor(values.FM2ExhaustSounds * 400 + 0.5), math.floor(values.FM2TurboSounds * 400 + 0.5)))
        end
        -- a method of the real object, called with the real one in place of the stand-in
        local function pass(object, standIn, v)
            if type(v) == "function" then
                return function(self, ...)
                    if self == standIn then self = object end
                    return v(self, ...)
                end
            end
            return v
        end
        local profile = {}
        setmetatable(profile, {
            __index = function(t, k)
                if values[k] then return values[k] end
                local p = real.activePlayerProfile
                if k == "AudioIncrement" or k == "AudioDecrement" or k == "ProfileOptionIncrement" or k == "ProfileOptionDecrement" then
                    local up = string.find(k, "Increment") ~= nil
                    return function(self, token, ...)
                        if values[token] then
                            values[token] = math.max(0, math.min(1, values[token] + (up and steps[token] or -steps[token])))
                            send()
                            return
                        end
                        return p[k](p, token, ...)
                    end
                end
                if k == "SetDefaultProfileOptionsByCategory" then
                    return function(self, ...)
                        values.FM2ExhaustSounds, values.FM2TurboSounds = 0.25, 0.25
                        send()
                        return p[k](p, ...)
                    end
                end
                return pass(p, t, p[k])
            end,
            __newindex = function(t, k, v)
                if values[k] then
                    values[k] = v
                    send()
                else
                    real.activePlayerProfile[k] = v
                end
            end,
        })
        FM2CarMix = { real = real }
        ProfileManager = setmetatable({}, {
            __index = function(t, k)
                if k == "activePlayerProfile" then return profile end
                return pass(real, t, real[k])
            end,
            __newindex = function(t, k, v) real[k] = v end,
        })
    end
end
)lua";

// Before any other flow script: the real ProfileManager back.
constexpr std::string_view kRestore = "if FM2CarMix then ProfileManager = FM2CarMix.real; FM2CarMix = nil end\n";
}

// The turbo slider's gain for an XMA stream (audio/xma.cpp, as a stream starts), by its first packet's hash:
// the turbo's and supercharger's samples (Media\Audio\Cars\Turbos\Turbos.fsb, SuperChargers\SuperChargers.fsb,
// blow-off valves included) are made louder as they are decoded. Their level in the cars' tuning and the sound
// banks' curves can't go above full volume (most turbos are at 0.3, the blow-offs lower), so the samples are.
float CarSoundStreamGain(uint64_t firstPacketHash)
{
    static const std::map<uint64_t, std::string> samples = [] {
        std::map<uint64_t, std::string> m;
        for (const char* bank : { "Media/Audio/Cars/Turbos/Turbos.fsb", "Media/Audio/Cars/SuperChargers/SuperChargers.fsb" })
        {
            std::ifstream in(GameDir() / bank, std::ios::binary);
            const std::vector<uint8_t> d{ std::istreambuf_iterator<char>(in), {} };
            auto u32 = [&](size_t at) { return at + 4 <= d.size() ? uint32_t(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24) : 0u; };
            // FSB3: "FSB3", the sample count, the headers' size, the data's size, version, mode; then each sample's
            // header (its size first, its name, its compressed length at +36), then the samples one after another
            if (d.size() < 24 || memcmp(d.data(), "FSB3", 4) != 0)
            {
                Log("car sound mix: %s not read", bank);
                continue;
            }
            size_t header = 24, data = 24 + u32(8);
            for (uint32_t i = 0; i < u32(4) && header + 40 <= d.size(); i++)
            {
                const size_t size = d[header] | d[header + 1] << 8, length = u32(header + 36);
                if (data + 2048 <= d.size())
                {
                    uint64_t h = 0xCBF29CE484222325ull; // as xma.cpp's StreamHash
                    for (size_t b = 0; b < 2048; b++)
                        h = (h ^ d[data + b]) * 0x100000001B3ull;
                    m[h] = std::string(reinterpret_cast<const char*>(&d[header + 2]), strnlen(reinterpret_cast<const char*>(&d[header + 2]), 30));
                }
                header += size;
                data += length;
            }
        }
        Log("car sound mix: %zu turbo and supercharger samples", m.size());
        return m;
    }();
    const Settings& s = GetSettings();
    if (!s.carSoundMix || s.turboVolume == 100)
        return 1.0f;
    const auto it = samples.find(firstPacketHash);
    if (it == samples.end())
        return 1.0f;
    static std::set<uint64_t> logged;
    static std::mutex mutex;
    if (std::lock_guard lock(mutex); logged.insert(firstPacketHash).second)
        Log("car sound mix: %s at %u%%", it->second.c_str(), s.turboVolume);
    return std::min(s.turboVolume, 400u) / 100.0f;
}

// "--carmix: set <exhaust> <turbo>" (percent): true if it was one.
bool CarSoundLuaNote(const char* text, uint32_t size)
{
    const std::string_view note(text, size);
    if (note.rfind(kSet, 0) != 0)
        return false;
    unsigned exhaust = 100, turbo = 100;
    if (sscanf(std::string(note.substr(kSet.size())).c_str(), "%u %u", &exhaust, &turbo) == 2)
    {
        Settings& s = GetSettings();
        s.exhaustVolume = std::min(exhaust, 400u);
        s.turboVolume = std::min(turbo, 400u);
        SaveSettings();
        Log("car sound mix: exhaust %u%%, turbo %u%% (from the next car loaded)", s.exhaustVolume, s.turboVolume);
    }
    return true;
}

// For hooks/lua.cpp, as a flow's Lua loads: the audio screens' rows and stand-in, the answer to "get", the real
// ProfileManager put back before other scripts; or empty when unchanged.
std::string CarSoundLuaSource(const char* text, uint32_t size)
{
    const std::string_view in(text, size);
    if (in.rfind(kGet, 0) == 0)
    {
        const Settings& s = GetSettings();
        return s.carSoundMix ? "return { " + std::to_string(s.exhaustVolume) + ", " + std::to_string(s.turboVolume) + " }" : "return nil";
    }
    if (in.rfind("--", 0) == 0 || in.rfind("return 0 --[[", 0) == 0 || in.rfind("return '' --[[", 0) == 0 ||
        in.rfind("return {} --[[", 0) == 0 || in.rfind("false --[[", 0) == 0)
        return {}; // the mods' notes and questions
    const size_t at = in.find(kQuery);
    if (at == in.npos)
        return in.find("FM2CarMix") == in.npos && in.find("ProfileManager") != in.npos ? std::string(kRestore) + std::string(in) : std::string();
    if (!GetSettings().carSoundMix)
        return {};
    std::string out(in);
    out.insert(at + kQuery.size(), kRows);
    // the stand-in after the statement the query is in (its line's end)
    const size_t line = out.find('\n', at);
    out.insert(line == out.npos ? out.size() : line + 1, kStandIn);
    return out;
}
