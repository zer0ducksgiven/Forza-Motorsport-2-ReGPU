// Random Events (the career mod in tools/modkit/career): counts finished career races per profile, shows the counts
// in the career screens (RandomEventsLuaSource) and gives a random car every fifth Random Events race.
//
// The count: CCareerCircuitRaceCoordinator::UpdateRaceResults runs once a career race is over and writes the
// result (INSERT INTO <profile>Career_RaceResults VALUES (race, place), or UPDATE ... SET Place when it's better).
// Its coordinator holds the event's ID at +72. Every call counts one career race; a race of a Random Events event
// (IDs 900-903) also counts towards the prize car. Kept in profiles\<name>\random_events.ini:
//   races=<career races>  random_races=<Random Events races>  rotation=<the set the mod was built for>
//   prize=<car id> prize_price=<its base price> prize_name=<name>   (a prize car won, not yet in the garage)
// The mod's events rotate every 2 races (the launcher rebuilds them at launch from races / 2).
//
// The prize: a car drawn from the mod's RandomEventsPrizes.txt (every car, written by the generator). The game has
// no "add a car" for this, so the car is bought: as the career main menu's script loads (FM2_PRIZE_CHUNK, default
// "C_MYC_MainMenu.lua"), Lobby.activePlayer.garage:BuyCar(<id>, 1), the dealership's own call, runs in the game's
// Lua state first (hooks/lua.cpp RunBefore) with the credits raised for it and put back after, so it costs nothing;
// the profile is saved then (or the car would be lost if the game closed before its next save). The purchase also
// makes it the current car.
// FM2_LUA_NAMES=1 logs every chunk's name (to find another script to use). Only while the mod is installed.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "kernel/imports.h"
#include "kernel/runtime.h"
#include "live_switch.h"
#include "log.h"

namespace
{
constexpr uint32_t kFirstEvent = 900, kLastEvent = 903, kRotateEvery = 2, kPrizeEvery = 5;

struct State
{
    uint32_t races = 0, randomRaces = 0, rotation = 0, prize = 0, prizePrice = 0;
    std::string prizeName;
    std::filesystem::path loadedFrom; // the profile's file it was read from (the profile signs in after boot)
} g_state;
std::mutex g_mutex;

std::filesystem::path StateFile() { return ContentDir().parent_path() / "random_events.ini"; }

// The mod's DLCZips folder, or empty when it isn't installed.
const std::filesystem::path& ModFolder()
{
    static const std::filesystem::path folder = [] {
        std::error_code ec;
        for (const auto& pack : std::filesystem::directory_iterator(DlcDir(), ec))
        {
            const std::filesystem::path zips = DlcPackageRoot(pack.path()) / "Media" / "DLCZips";
            for (const auto& z : std::filesystem::directory_iterator(zips, ec))
                if (z.path().filename().string().rfind("RandomEvents_pri_", 0) == 0)
                    return zips;
        }
        return std::filesystem::path();
    }();
    return folder;
}

bool ModInstalled() { return !ModFolder().empty(); }

void Load()
{
    const std::filesystem::path file = StateFile();
    if (file == g_state.loadedFrom)
        return;
    g_state = State{};
    g_state.loadedFrom = file;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const uint32_t v = uint32_t(strtoul(value.c_str(), nullptr, 10));
        if (key == "races") g_state.races = v;
        else if (key == "random_races") g_state.randomRaces = v;
        else if (key == "rotation") g_state.rotation = v;
        else if (key == "prize") g_state.prize = v;
        else if (key == "prize_price") g_state.prizePrice = v;
        else if (key == "prize_name") g_state.prizeName = value;
    }
}

void Save()
{
    std::ofstream out(StateFile(), std::ios::trunc);
    out << "races=" << g_state.races << "\nrandom_races=" << g_state.randomRaces << "\nrotation=" << g_state.rotation << "\n";
    if (g_state.prize)
        out << "prize=" << g_state.prize << "\nprize_price=" << g_state.prizePrice << "\nprize_name=" << g_state.prizeName << "\n";
}

// A car from the prize list, any of them.
void DrawPrize()
{
    std::ifstream in(ModFolder() / "RandomEventsPrizes.txt");
    std::vector<std::string> cars;
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            cars.push_back(line);
    if (cars.empty())
    {
        Log("random events: no prize list (RandomEventsPrizes.txt) in the mod");
        return;
    }
    std::mt19937 rng{ std::random_device{}() };
    const std::string& line = cars[rng() % cars.size()];
    unsigned id = 0, price = 0, year = 0;
    int used = 0;
    if (sscanf(line.c_str(), "%u %u %u %n", &id, &price, &year, &used) < 3)
        return;
    g_state.prize = id;
    g_state.prizePrice = price;
    g_state.prizeName = std::to_string(year) + " " + (line.c_str() + used);
    Log("random events: prize car won: %s (car %u, %u CR)", g_state.prizeName.c_str(), id, price);
}
}

// For hooks/lua.cpp, as a script loads: Lua to run before it, in the same state (the prize car's purchase), or empty.
std::string RandomEventsLuaBefore(const char* name, bool source, const char* text, uint32_t size)
{
    (void)text;
    (void)size;
    static LiveSwitch logNames("FM2_LUA_NAMES");
    if (logNames)
        Log("lua: loading %s (%s)", name, source ? "source" : "compiled");
    if (!ModInstalled())
        return {};
    static const std::string target = getenv("FM2_PRIZE_CHUNK") ? getenv("FM2_PRIZE_CHUNK") : "C_MYC_MainMenu.lua";
    static bool told = false;
    if (logNames && !told)
    {
        told = true;
        Log("random events: the prize code runs before the script named like \"%s\"", target.c_str());
    }
    if (!strstr(name, target.c_str()))
        return {};
    std::lock_guard lock(g_mutex);
    Load();
    if (logNames)
        Log("random events: %s (%s) is the prize script; prize %u", name, source ? "source" : "compiled", g_state.prize);
    if (!g_state.prize)
        return {};
    char lua[512];
    // The credits are raised so the purchase can't fail, then put back as they were, so the car costs nothing; then
    // the profile is saved, as the dealership saves after a purchase (P_SaveGame.xml).
    snprintf(lua, sizeof(lua),
             "pcall(function() local p = ProfileManager.Main local c = p.Credits p.Credits = c + 10000000 "
             "pcall(function() Lobby.activePlayer.garage:BuyCar(%u, 1) end) p.Credits = c "
             "ProfileManager.activePlayerProfile:Save() end)", g_state.prize);
    Log("random events: prize car %s (car %u) goes into the garage before %s", g_state.prizeName.c_str(), g_state.prize, name);
    g_state.prize = 0;
    g_state.prizePrice = 0;
    g_state.prizeName.clear();
    Save();
    return lua;
}

// The counts, in the game's own screens (Flows/CareerEventSelect.xml, rewritten as its scripts load: they load
// again each time the screens open, so the counts are current):
//  - Race Type: Random Events is listed first, and its description (raceTypeQuery) says when the events change and when the
//    prize car comes, or which car was won;
//  - Event Select: "First Place Prize" (PrizeQuery) shows only a prize car (one per event in Rewards_CareerEvents),
//    so a Random Events event, which pays credits, showed an empty box: it falls back to the event's CashPrize
//    ("12,000 CR"; the generator's prizes are whole thousands) and the races to the prize car.
// Text the queries return in place of a string reference is shown as it is.
namespace
{
constexpr uint32_t kCategory = 10; // CareerEventTypes.id (tools/modkit/career/random_events.py, CATEGORY_ID)

std::string Plural(uint32_t n, const char* what)
{
    return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
}

// A string for SQL ('' for ').
std::string Quoted(const std::string& text)
{
    std::string out = "'";
    for (char c : text)
        out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

bool Replace(std::string& text, std::string_view from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at == std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    return true;
}
}

std::string RandomEventsLuaSource(const char* text, uint32_t size)
{
    static const std::string_view kTypes = "SELECT id, Name, Description, BadgePath";
    static const std::string_view kTypesOrder = "FROM CareerEventTypes ORDER BY id";
    static const std::string_view kPrize =
        "SELECT ModelShort AS ModelName FROM Rewards_CareerEvents INNER JOIN Data_Car ON Id=CarId WHERE EventId=[Events.Id]";
    if (!ModInstalled())
        return {};
    std::string out(text, size);
    if (out.find(kTypes) == std::string::npos && out.find(kPrize) == std::string::npos)
        return {};
    std::lock_guard lock(g_mutex);
    Load();
    const uint32_t toRotate = kRotateEvery - g_state.races % kRotateEvery;
    const uint32_t toPrize = kPrizeEvery - g_state.randomRaces % kPrizeEvery;
    std::string status = g_state.prize
        ? "Prize car won: the " + g_state.prizeName + "! It goes into your garage from the Career menu."
        : "Four new events every two career races, a random car every five Random Events races. New events in " +
              Plural(toRotate, "race") + ", prize car in " + Plural(toPrize, "race") + ".";
    const std::string prize = g_state.prize ? " + prize car won" : ", prize car in " + Plural(toPrize, "race");
    bool changed = Replace(out, kTypes,
                           "SELECT id, Name, CASE WHEN id=" + std::to_string(kCategory) + " THEN " + Quoted(status) +
                               " ELSE Description END AS Description, BadgePath");
    // Random Events first in the list
    changed |= Replace(out, kTypesOrder, "FROM CareerEventTypes ORDER BY id<>" + std::to_string(kCategory) + ", id");
    changed |= Replace(out, kPrize,
                       "SELECT COALESCE((SELECT ModelShort FROM Rewards_CareerEvents INNER JOIN Data_Car ON Data_Car.Id=CarId "
                       "WHERE EventId=e.Id), (e.CashPrize / 1000) || ',000 CR' || " + Quoted(prize) +
                           ") AS ModelName FROM Events e WHERE e.Id=[Events.Id]");
    if (changed)
        Log("random events: the career screens show the counts (%s)", status.c_str());
    return changed ? out : std::string();
}

#if FM2_TITLE_UPDATE
// Pasted together so tools/xexremap.py leaves these title update addresses be (as in quit_button.cpp).
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

PPC_FUNC_IMPL(TU_IMP(82348078));

// CCareerCircuitRaceCoordinator::UpdateRaceResults(this, args)
PPC_FUNC(TU_SUB(82348078))
{
    const uint32_t coordinator = ctx.r3.u32;
    const uint32_t eventId = __builtin_bswap32(*GuestPtr<uint32_t>(coordinator + 72));
    TU_IMP(82348078)(ctx, base);
    if (!ModInstalled())
        return;
    std::lock_guard lock(g_mutex);
    Load();
    g_state.races++;
    const bool random = eventId >= kFirstEvent && eventId <= kLastEvent;
    if (random)
        g_state.randomRaces++;
    if (random && g_state.randomRaces % kPrizeEvery == 0 && !g_state.prize)
        DrawPrize();
    Save();
    Log("random events: career race finished (event %u%s): %u races, %u Random Events races", eventId, random ? ", Random Events" : "",
        g_state.races, g_state.randomRaces);
}
#endif
