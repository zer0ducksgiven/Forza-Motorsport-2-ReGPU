// Optional cheats, per profile: profiles\<name>\cheats.ini, written by the launcher's Profiles tab.
// They change the game's own state through its own functions, so the game saves the result in
// the (encrypted, signed) profile itself:
//   infinite_credits=1  credits read as at least kInfiniteCredits
//   give_credits=<n>    n more credits: shown at once, saved with the next credit change (a prize or
//                       a purchase writes what it read), then the file says give_credits=0
//   max_level=1         the driver levels (career and Arcade) read as kMaxLevel
//   arcade_all_cars=1   the developers' AddAllCarsCheat, once: every car (about 470) in the Arcade
//                       garage. The game freezes for about 20 s while it adds them.
// Addresses are the disc executable's (the title-update build translates them). Every profile value
// ("Main/Credits", "Main/Level") is read through the profile database's lookup (sub_8245FD08) and
// credits are written by SetCredits (sub_8222E3F0). AddAllCarsCheat (sub_8220FD20) runs on the
// App::Forza object, which Lua fetches through luaL_checkudata (sub_8254E1A0).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kernel/imports.h"
#include "kernel/runtime.h"
#include "log.h"
#include "memory.h"

namespace
{
constexpr uint32_t kInfiniteCredits = 99999999, kMaxCredits = 999999999, kMaxLevel = 50;

bool g_profileLoaded = false; // its values are being read (the Arcade cheat waits for that)

struct Cheats
{
    bool infiniteCredits = false, maxLevel = false, arcadeAllCars = false;
    uint32_t giveCredits = 0;
};

std::filesystem::path CheatsFile() { return ContentDir().parent_path() / "cheats.ini"; }

Cheats& Get()
{
    static Cheats c = [] {
        Cheats c;
        std::ifstream in(CheatsFile());
        for (std::string line; std::getline(in, line);)
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq);
            const unsigned long v = strtoul(line.c_str() + eq + 1, nullptr, 10);
            if (key == "infinite_credits")
                c.infiniteCredits = v != 0;
            else if (key == "give_credits")
                c.giveCredits = uint32_t(std::min<unsigned long>(v, kMaxCredits));
            else if (key == "max_level")
                c.maxLevel = v != 0;
            else if (key == "arcade_all_cars")
                c.arcadeAllCars = v != 0;
        }
        if (c.infiniteCredits || c.giveCredits || c.maxLevel || c.arcadeAllCars)
            Log("cheats: infinite credits %d, give %u, max level %d, all Arcade cars %d", int(c.infiniteCredits), c.giveCredits,
                int(c.maxLevel), int(c.arcadeAllCars));
        return c;
    }();
    return c;
}

// One-shot cheats are switched off in the file once done; its other lines are kept.
void SpendOnce(const char* key)
{
    const std::string prefix = std::string(key) + "=";
    std::vector<std::string> lines;
    {
        std::ifstream in(CheatsFile());
        for (std::string line; std::getline(in, line);)
            lines.push_back(line.rfind(prefix, 0) == 0 ? prefix + "0" : line);
    }
    std::ofstream out(CheatsFile(), std::ios::trunc);
    for (const auto& line : lines)
        out << line << "\n";
}
}

// Any cheat switched on for this profile (FM2 Live stays off then: kernel/runtime.cpp).
bool CheatsOn()
{
    const Cheats& c = Get();
    return c.infiniteCredits || c.giveCredits || c.maxLevel || c.arcadeAllCars;
}

PPC_FUNC_IMPL(__imp__sub_8222E3F0);
// SetCredits(profile, credits): the game writes credits it read with the gift added, so the gift is
// saved from here on.
PPC_FUNC(sub_8222E3F0)
{
    __imp__sub_8222E3F0(ctx, base);
    Cheats& c = Get();
    if (c.giveCredits)
    {
        Log("cheats: %u credits given (saved with the next credit change)", c.giveCredits);
        c.giveCredits = 0;
        SpendOnce("give_credits");
    }
}

PPC_FUNC_IMPL(__imp__sub_8254E1A0);
PPC_FUNC_IMPL(__imp__sub_8220FD20);
// luaL_checkudata(L, index, type): the first App::Forza with a profile loaded gets the cheat.
PPC_FUNC(sub_8254E1A0)
{
    const uint32_t type = ctx.r5.u32;
    __imp__sub_8254E1A0(ctx, base);
    static bool done = false;
    if (done || !g_profileLoaded || !ctx.r3.u32 || !Get().arcadeAllCars || !type || strcmp(GuestPtr<const char>(type), "App::Forza") != 0)
        return;
    const uint32_t forza = LoadBE32(ctx.r3.u32);
    if (!forza || !LoadBE32(forza + 176)) // no profile yet (the cheat itself checks the same)
        return;
    done = true;
    const uint64_t result = ctx.r3.u64;
    ctx.r3.u64 = forza;
    __imp__sub_8220FD20(ctx, base); // AddAllCarsCheat
    ctx.r3.u64 = result;
    Get().arcadeAllCars = false;
    SpendOnce("arcade_all_cars");
    Log("cheats: every car added to the Arcade garage");
}

namespace
{
// The store's key: a vtable, a hash, then the value's name within its section ("Credits" for
// Main/Credits, "Level" for Main/Level and Arcade/Level) in a fixed 32-character buffer.
bool KeyIs(uint32_t key, const char* name) { return strncmp(GuestPtr<const char>(key + 8), name, 32) == 0; }
}

PPC_FUNC_IMPL(__imp__sub_8245FD08);
// The profile database's lookup: (result, database, key) -> result, an integer at +8. Every read of
// a profile value goes through it (the menus' ProfileManager.Main.Credits as well as GetCredits).
PPC_FUNC(sub_8245FD08)
{
    const uint32_t key = ctx.r5.u32, result = ctx.r3.u32;
    __imp__sub_8245FD08(ctx, base);
    if (!g_profileLoaded && key && KeyIs(key, "Credits"))
        g_profileLoaded = true;
    Cheats& c = Get();
    if (!key || (!c.infiniteCredits && !c.giveCredits && !c.maxLevel))
        return;
    const uint32_t value = LoadBE32(result + 8);
    if (KeyIs(key, "Credits"))
    {
        uint32_t shown = value;
        if (c.giveCredits)
            shown = uint32_t(std::min<uint64_t>(uint64_t(value) + c.giveCredits, kMaxCredits));
        if (c.infiniteCredits)
            shown = std::max(shown, kInfiniteCredits);
        StoreBE32(result + 8, shown);
    }
    else if (c.maxLevel && KeyIs(key, "Level"))
        StoreBE32(result + 8, std::max(value, kMaxLevel));
}
