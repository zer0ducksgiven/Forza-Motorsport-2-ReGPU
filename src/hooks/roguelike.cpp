// Roguelike Career (FM2-roguelike-scope.md): a career run on a fresh profile, built on Random Events'
// machinery. The mod package (tools/modkit/career/random_events.py --mode roguelike) carries the events and, in
// its Media\DLCZips\overlay folder, the career flows it changes (roguelike_ui.py; merged into UI.zip by
// kernel/zip_overlay.cpp while the package is switched on). Here: the run's state, the flows' Lua markers, and
// the car reward screen showing the run's cars.
//
// The run is kept in profiles\<name>\roguelike.ini:
//   run=1   this profile's career was started as a run (the first car bought under the mod)
// A profile whose career wasn't is turned away from Career while the mod is on (CareerMenu.xml).
//
// The car reward screen (Flows/CarReveal.xml, scene C_WIN_CarReveal) asks the native RewardReveal object which car
// to show; it keeps the race's unlocked cars as a vector of car IDs (this+12..+16). A flow queues the run's cars with
// "--roguelike: reveal <car>" (a win's car, then the car its part is for), and the hooks below show each once:
// CheckForCarUnlocks ("not empty") says yes while any wait, GetCurrentCarUnlock (which takes the first) returns the
// next. Otherwise both are the game's own.
#include <algorithm>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
std::mutex g_mutex;
std::mutex g_revealMutex;
std::deque<uint32_t> g_reveal; // the cars the reward screen shows next, in order (a win's car, then the part's car)

std::filesystem::path StateFile() { return ContentDir().parent_path() / "roguelike.ini"; }

// The package's DLCZips folder while it's switched on (its database zip is in an enabled DLC folder), or empty.
const std::filesystem::path& PackFolder()
{
    static const std::filesystem::path folder = [] {
        std::error_code ec;
        for (const auto& pack : std::filesystem::directory_iterator(DlcDir(), ec))
            if (const auto zips = DlcPackageRoot(pack.path()) / "Media" / "DLCZips"; std::filesystem::exists(zips / "Roguelike_pri_73.zip", ec))
                return zips;
        return std::filesystem::path();
    }();
    return folder;
}

bool Installed() { return !PackFolder().empty(); }

struct State
{
    bool run = false;
    uint32_t races = 0;   // career races finished in the run
    uint32_t given = 0;   // races whose car has been given
    uint32_t pending = 0; // the car drawn for the race being rewarded, until it's given
    uint32_t set = 0;     // the events' set: a new one once all four are raced
    std::map<uint32_t, uint32_t> strikes; // garage car -> its races in a row 4th or worse (the second takes the car)
    uint32_t level = 0;   // the run's level, 0 .. kTopLevel: one up a podium, the events harder with it
    std::string picks;    // the set's four garage cars ("a,b,c,d"), so its events stay put as cars are won
};

State Load()
{
    State s;
    std::ifstream in(StateFile());
    for (std::string line; std::getline(in, line);)
        if (line == "run=1")
            s.run = true;
        else if (line.rfind("races=", 0) == 0)
            s.races = uint32_t(strtoul(line.c_str() + 6, nullptr, 10));
        else if (line.rfind("given=", 0) == 0)
            s.given = uint32_t(strtoul(line.c_str() + 6, nullptr, 10));
        else if (line.rfind("pending=", 0) == 0)
            s.pending = uint32_t(strtoul(line.c_str() + 8, nullptr, 10));
        else if (line.rfind("set=", 0) == 0)
            s.set = uint32_t(strtoul(line.c_str() + 4, nullptr, 10));
        else if (line.rfind("level=", 0) == 0)
            s.level = uint32_t(strtoul(line.c_str() + 6, nullptr, 10));
        else if (line.rfind("strikes=", 0) == 0) // "gid:n,gid:n"
            for (const char* p = line.c_str() + 8; *p;)
            {
                char* end = nullptr;
                const uint32_t gid = uint32_t(strtoul(p, &end, 10));
                if (*end != ':')
                    break;
                const uint32_t n = uint32_t(strtoul(end + 1, &end, 10));
                if (n)
                    s.strikes[gid] = n;
                p = *end == ',' ? end + 1 : end;
            }
        else if (line.rfind("picks=", 0) == 0)
            s.picks = line.substr(6);
    return s;
}

void Save(const State& s)
{
    std::string strikes;
    for (const auto& [gid, n] : s.strikes)
        strikes += (strikes.empty() ? "" : ",") + std::to_string(gid) + ":" + std::to_string(n);
    std::ofstream(StateFile(), std::ios::trunc) << (s.run ? "run=1\n" : "") << "races=" << s.races << "\ngiven=" << s.given
                                                 << "\npending=" << s.pending << "\nset=" << s.set << "\nstrikes=" << strikes
                                                 << "\nlevel=" << s.level
                                                 << "\npicks=" << s.picks << "\n";
}

bool RunStarted() { return Load().run; }

struct Car
{
    uint32_t id, cls, type; // Data_Car.Id, ClassID (0 D .. 4 S, 5 U, 6-9 race classes R4-R1), CarTypeID (3 race car)
};

// Every car (the generator's RoguelikeCars.txt: "<id> <class> <type>").
const std::vector<Car>& Cars()
{
    static const std::vector<Car> cars = [] {
        std::vector<Car> out;
        std::ifstream in(PackFolder() / "RoguelikeCars.txt");
        for (std::string line; std::getline(in, line);)
            if (Car c{}; sscanf(line.c_str(), "%u %u %u", &c.id, &c.cls, &c.type) == 3)
                out.push_back(c);
        return out;
    }();
    return cars;
}

std::mt19937& Rng()
{
    static std::mt19937 rng{ std::random_device{}() };
    return rng;
}

// A random road car of this class (0 when the list has none).
uint32_t DrawCar(uint32_t cls)
{
    std::vector<uint32_t> pool;
    for (const Car& c : Cars())
        if (c.cls == cls && c.type != 3)
            pool.push_back(c.id);
    return pool.empty() ? 0 : pool[Rng()() % pool.size()];
}

// A win's chance of a car (one in this many; every win earns a part, CarReveal.xml)
constexpr uint32_t kCarChance = 5;
// The run's top level (State::level): its events the hardest
constexpr uint32_t kTopLevel = 30;

// A won race's car (FM2-roguelike-scope.md, decisions 1 and 2; any other place earns none): around the class raced
// (as upgraded), 60% the same, 20% one up, 20% one down, from D to S; the Extreme event (913) is a race car one time
// in 20.
uint32_t DrawNextCar(uint32_t eventId, uint32_t place, uint32_t raced)
{
    if (eventId == 913 && place == 1 && Rng()() % 20 == 0)
    {
        std::vector<uint32_t> pool;
        for (const Car& c : Cars())
            if (c.type == 3)
                pool.push_back(c.id);
        if (!pool.empty())
        {
            Log("roguelike: Extreme won: a race car");
            return pool[Rng()() % pool.size()];
        }
    }
    int cls = int(std::min<uint32_t>(raced, 4));
    if (const uint32_t r = Rng()() % 10; r >= 8)
        cls--;
    else if (r >= 6)
        cls++;
    return DrawCar(uint32_t(std::clamp(cls, 0, 4)));
}

bool Replace(std::string& text, std::string_view from, std::string_view to);

// The generator's English names (RoguelikeNames.txt: "<kind>\t<id>\t<name>") as a Lua table:
// { make = { [id] = "name", ... }, country = ..., track = ..., cylinders = ..., car = ... }.
// The generator's English names (RoguelikeNames.txt: "<kind>\t<id>\t<name>"), by kind and id.
const std::map<std::string, std::map<uint32_t, std::string>>& Names()
{
    static const auto names = [] {
        std::map<std::string, std::map<uint32_t, std::string>> out;
        std::ifstream in(PackFolder() / "RoguelikeNames.txt");
        for (std::string line; std::getline(in, line);)
        {
            const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b != std::string::npos)
                out[line.substr(0, a)][uint32_t(strtoul(line.c_str() + a + 1, nullptr, 10))] = line.substr(b + 1);
        }
        return out;
    }();
    return names;
}

std::string LuaString(const std::string& text)
{
    std::string out = "\"";
    for (char c : text)
        out += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    return out + "\"";
}

// The names as a Lua table: { make = { [id] = "name", ... }, country = ..., track = ..., cylinders = ..., car = ... }.
const std::string& NamesLua()
{
    static const std::string lua = [] {
        std::string out = "{";
        for (const char* kind : { "make", "country", "track", "cylinders", "car" })
        {
            out += std::string(kind) + "={";
            if (const auto it = Names().find(kind); it != Names().end())
                for (const auto& [id, name] : it->second)
                    out += "[" + std::to_string(id) + "]=" + LuaString(name) + ",";
            out += "},";
        }
        return out + "}";
    }();
    return lua;
}

// "<make> <model>" for a car (CarReveal.xml's part message), or "".
std::string CarName(uint32_t make, uint32_t car)
{
    auto find = [](const char* kind, uint32_t id) {
        const auto k = Names().find(kind);
        if (k == Names().end())
            return std::string();
        const auto n = k->second.find(id);
        return n == k->second.end() ? std::string() : n->second;
    };
    const std::string m = find("make", make), c = find("car", car);
    return m.empty() ? c : c.empty() ? m : m + " " + c;
}

// Lua run as Event Select loads (before its events query): the run's four events built from the garage
// (FM2-roguelike-scope.md, "Events built from the garage"). Four cars are picked: the newest always, the rest spread
// across the classes owned. Each event takes its car's class (as upgraded) and a theme the car passes with at least
// seven AI cars of that class (else "Class X Open"), a random track, the tier's distance in laps, an AI field from
// those cars, and no pay. Rules other than the last are joined with 'And' (the game's query has no joiner
// otherwise). Every race brings a new set: the seed is the set's number, and its four cars are kept (picks) until
// then, so selling a car doesn't change the events.
std::string EventBuilder(uint32_t seed, const std::string& picks, uint32_t level)
{
    static const char* kLua = R"LUA(
local ok, err = pcall(function()
if tonumber(UI.Storage.CareerEventType) ~= 11 then return end
local NAMES = @NAMES@
local s = @SEED@
local PICKS = { @PICKS@ } -- the set's four garage cars, once chosen
local TEST = @TEST@
local LEVEL, TOP = @LEVEL@, @TOP@ -- the run's level, and its top
local function rnd(n) s = math.mod(s * 16807, 2147483647) return math.floor(s / 2147483647 * n) + 1 end
local function shuffle(t) for i = table.getn(t), 2, -1 do local j = rnd(i) t[i], t[j] = t[j], t[i] end end
local function q(sql) return DB:execute(sql):toTable() or {} end
local function x(sql) DB:execute(sql) end
local function sq(text) return "'" .. (string.gsub(text, "'", "''")) .. "'" end
-- name, distance (m), levels above the run's (its difficulty: the run's level plus these, of TOP + 15), laps at least,
-- at most (a lap over 10 km, the Nordschleife, is a race already: only the most applies)
local TIERS = { { "Easy", 10000, 0, 1, 3 }, { "Medium", 15000, 5, 1, 5 }, { "Hard", 20000, 10, 5, 7 }, { "Extreme", 25000, 15, 5, 7 } }
local function round(v) return math.floor(v + 0.5) end
local function lerp(a, b, t) return a + (b - a) * t end
local CLASS = { [0] = "D", "C", "B", "A", "S", "U", "R4", "R3", "R2", "R1" }
local DRIVE = { "FWD", "RWD", "AWD" }
local REGION = { "Asian", "European", "North American" }
local ICON = { classics = 101, modern = 34, fwd = 4, rwd = 6, awd = 5, turbo = 26, free = 30, light = 11 }
local REGION_ICON = { 2, 1, 3 }
local COUNTRY_ICON = { Japan = 82, Germany = 78, Italy = 48, ["United Kingdom"] = 76, England = 76, USA = 42, ["United States"] = 42 }
local MAKE_ICON = { Acura = 13, Volkswagen = 15, Toyota = 16, Nissan = 18, Audi = 19, Porsche = 20, Chevrolet = 21, Dodge = 22,
                    Lamborghini = 23, Ferrari = 24, Ford = 61, Mazda = 62, Lotus = 65, Shelby = 66, TVR = 69, McLaren = 72, Honda = 477 }
local CYL_ICON = { ["4"] = 25, ["5"] = 31, ["6"] = 27, ["8"] = 29 }
local CARS = "SELECT c.Id AS id, c.Year AS year, c.MakeID AS make, c.CurbWeight AS weight, c.CarTypeID AS type, " ..
             "m.CountryID AS country, n.RegionID AS region, e.DriveTypeID AS drive, e.AspirationID_Stock AS asp, e.CylinderID AS cyl, " ..
             "sg.PerformanceIndex AS pi " ..
             "FROM Data_Car c JOIN StockGarage sg ON sg.Id = c.Id JOIN List_CarMake m ON m.ID = c.MakeID JOIN List_Country n ON n.CountryID = m.CountryID " ..
             "JOIN List_UpgradeEngine u ON u.Ordinal = c.Id AND u.IsStock = 1 JOIN Data_Engine e ON e.EngineID = u.EngineID"

-- the cars the events are built on
local garage = q("SELECT g.Id AS gid, g.ClassID AS cls, g.PerformanceIndex AS gpi, x.* FROM " .. Lobby.activePlayer.DB.Garage ..
                 " g JOIN (" .. CARS .. ") x ON x.id = g.CarId ORDER BY g.Id")
local n = table.getn(garage)
if n == 0 then return end
-- the set's cars as chosen when the set began, while they're all still owned (else a new choice)
local byGid, picks = {}, {}
for _, c in ipairs(garage) do byGid[c.gid] = c end
for _, gid in ipairs(PICKS) do if byGid[gid] then table.insert(picks, byGid[gid]) end end
if table.getn(picks) ~= 4 then
    local used, others = { [garage[n].cls] = true }, {}
    picks = { garage[n] }
    for i = 1, n - 1 do others[i] = garage[i] end
    shuffle(others)
    for _, c in ipairs(others) do
        if table.getn(picks) < 4 and not used[c.cls] then table.insert(picks, c) used[c.cls] = true end
    end
    for _, c in ipairs(others) do
        local dup = false
        for _, p in ipairs(picks) do if p == c then dup = true end end
        if table.getn(picks) < 4 and not dup then table.insert(picks, c) end
    end
    while table.getn(picks) < 4 do table.insert(picks, picks[rnd(table.getn(picks))]) end
    shuffle(picks)
    loadstring("--roguelike: picks " .. picks[1].gid .. "," .. picks[2].gid .. "," .. picks[3].gid .. "," .. picks[4].gid)
end
-- earlier sets' results are cleared (the event IDs are the same each set)
local results, raceResults = Lobby.activePlayer.DB.EventResults, Lobby.activePlayer.DB.RaceResults
for id = 910, 913 do
    x("DELETE FROM " .. results .. " WHERE EventId = " .. id)
    x("DELETE FROM " .. raceResults .. " WHERE RaceId IN (SELECT Id FROM Races WHERE EventId = " .. id .. ")")
end

-- the themes a car passes: { title, rule field, function, value, Data_Car condition, picture }
local function themes(c)
    local t = {}
    local make, country, cyl = NAMES.make[c.make], NAMES.country[c.country], NAMES.cylinders[c.cyl]
    if make then table.insert(t, { make .. " Cup", 6, "=", c.make, "c.MakeID = " .. c.make, MAKE_ICON[make] or REGION_ICON[c.region] or 1 }) end
    if country then table.insert(t, { country .. " Showdown", 4, "=", c.country, "m.CountryID = " .. c.country,
                                       COUNTRY_ICON[country] or REGION_ICON[c.region] or 1 }) end
    if REGION[c.region] then table.insert(t, { REGION[c.region] .. " Open", 3, "=", c.region, "n.RegionID = " .. c.region, REGION_ICON[c.region] }) end
    if DRIVE[c.drive] then table.insert(t, { DRIVE[c.drive] .. " Shootout", 14, "=", c.drive, "e.DriveTypeID = " .. c.drive,
                                             ICON[string.lower(DRIVE[c.drive])] }) end
    if c.asp == 2 then table.insert(t, { "Turbo Trophy", 15, "=", 2, "e.AspirationID_Stock = 2", ICON.turbo })
    elseif c.asp == 1 then table.insert(t, { "Free-Breathing Trophy", 15, "=", 1, "e.AspirationID_Stock = 1", ICON.free }) end
    if cyl then table.insert(t, { cyl .. " Challenge", 8, "=", c.cyl, "e.CylinderID = " .. c.cyl,
                                  CYL_ICON[string.sub(cyl, 1, 1)] or ICON.modern }) end
    if c.year < 1995 then
        local y = c.year + 5 - math.mod(c.year, 5)
        table.insert(t, { "Pre-" .. y .. " Classics", 1, "<", y, "c.Year < " .. y, ICON.classics })
    else
        local y = c.year - 1 - math.mod(c.year - 1, 5)
        table.insert(t, { "Post-" .. y .. " Machines", 1, ">", y, "c.Year > " .. y, ICON.modern })
    end
    local w = math.floor(c.weight) + 1
    table.insert(t, { "Lightweight Invitational", 11, "<", w, "c.CurbWeight < " .. w, ICON.light })
    return t
end

local tracks = q("SELECT id, MediaName AS media, Length AS len FROM Tracks WHERE MediaName <> 'DebugTrack' AND MediaName <> 'TuningTrack' ORDER BY id")
local circuits, seen = {}, {}
for _, t in ipairs(tracks) do if not seen[t.media] then seen[t.media] = true table.insert(circuits, t.media) end end

for slot = 1, 4 do
    local c, tier, id = picks[slot], TIERS[slot], 909 + slot
    local race = c.type == 3
    local base = "c.ClassID = " .. c.cls .. " AND c.CarTypeID " .. (race and "= 3" or "<> 3")
    local cand, theme, pool = themes(c)
    shuffle(cand)
    for _, th in ipairs(cand) do
        local p = q(CARS .. " WHERE " .. base .. " AND " .. th[5])
        if table.getn(p) >= 7 then theme, pool = th, p break end
    end
    if not theme then
        theme, pool = { "Class " .. CLASS[c.cls] .. " Open", nil, nil, nil, "1 = 1", ICON.modern }, q(CARS .. " WHERE " .. base)
    end
    if table.getn(pool) == 0 then pool = { c } end
    -- entry rules: the class, race cars or not, the theme
    local rules = { { 10, "=", c.cls }, { 18, race and "=" or "<>", 3 } }
    if theme[2] then table.insert(rules, { theme[2], theme[3], theme[4] }) end
    x("DELETE FROM EventRestrictions WHERE Event_ID = " .. id)
    for j, r in ipairs(rules) do
        x("INSERT INTO EventRestrictions (Id, Event_ID, FieldAttributeID, Func, Value, AndOrIndicator) VALUES (" ..
          (29100 + slot * 10 + j) .. ", " .. id .. ", " .. r[1] .. ", '" .. r[2] .. "', '" .. r[3] .. "', '" ..
          (j < table.getn(rules) and "And" or "") .. "')")
    end
    -- the track (a circuit, then one of its layouts) and laps for the tier's distance
    local media, layouts = circuits[rnd(table.getn(circuits))], {}
    for _, t in ipairs(tracks) do if t.media == media then table.insert(layouts, t) end end
    local track = layouts[rnd(table.getn(layouts))]
    local laps = math.min(math.max(track.len > 10000 and 1 or tier[4], math.floor(tier[2] / track.len + 0.5)), tier[5])
    if TEST then -- FM2_ROGUELIKE_TEST: one lap of the shortest layout
        for _, t in ipairs(tracks) do if t.len < track.len then track = t end end
        laps = 1
    end
    x("UPDATE Races SET TrackId = " .. track.id .. ", NumLaps = " .. laps .. " WHERE EventId = " .. id)
    -- the event's difficulty, 0 .. 1: the run's level and the tier's
    local d = math.min(1, (LEVEL + tier[3]) / (TOP + 15))
    -- the AI field: seven of the theme's cars in stock PI order, against the PI of the car the event is built on (as
    -- upgraded): from the slowest seven (0) through the seven around it (0.5) to the fastest seven (1)
    table.sort(pool, function(a, b) return a.pi < b.pi end)
    local np, at = table.getn(pool), 1
    for i, p in ipairs(pool) do if p.pi <= (c.gpi or c.pi) then at = i end end
    local from = d < 0.5 and lerp(1, at - 3, d * 2) or lerp(at - 3, np - 6, d * 2 - 1)
    from = math.max(1, math.min(round(from), np - 6))
    local field = {}
    for i = from, math.min(np, from + 6) do table.insert(field, pool[i]) end
    shuffle(field)
    -- AI skill on Intermediate (AISkills: 1 best .. 20; Novice 4 easier, Hardcore 4 harder), the generator's
    -- torque-boosted skills (+100: 5% more torque, +200: 10%), AI aggression (AITemperaments: 1 most .. 10)
    local skill, boost, aggro = round(lerp(20, 1, d)), d >= 0.8 and 200 or d >= 0.45 and 100 or 0, round(lerp(9, 1, d))
    local skills = { math.min(20, skill + 4) + boost, skill + boost, math.max(1, skill - 4) + boost } -- Novice, Intermediate/Hard, Hardcore
    local slots = q("SELECT ID FROM EventParticipants WHERE EventID = " .. id .. " ORDER BY ID")
    for i, sl in ipairs(slots) do
        x("UPDATE EventParticipants SET Ordinal = " .. field[math.mod(i - 1, table.getn(field)) + 1].id ..
          ", Easy_AISkill_Id = " .. skills[1] .. ", Medium_AISkill_Id = " .. skills[2] .. ", Hard_AISkill_Id = " .. skills[3] ..
          ", Easy_AITemperament_id = " .. aggro .. ", Medium_AITemperament_id = " .. aggro .. ", Hard_AITemperament_id = " .. aggro ..
          " WHERE ID = " .. sl.ID)
    end
    -- the name and description, no pay
    local desc = (NAMES.track[track.id] or media) .. ". Class " .. CLASS[c.cls] .. ", " .. laps .. " lap" .. (laps > 1 and "s" or "") ..
                 ". Built around your " .. c.year .. " " .. (NAMES.make[c.make] or "") .. " " .. (NAMES.car[c.id] or "") .. "."
    x("UPDATE Events SET Name = " .. sq(tier[1] .. ": " .. theme[1]) .. ", Description = " .. sq(desc) .. ", EventIcon = " .. theme[6] ..
      ", CashPrize = 0, EventOrder = " .. slot .. " WHERE Id = " .. id)
    x("UPDATE EventScoring SET Credits = 0 WHERE Event_ID = " .. id)
end
loadstring("--roguelike: events built")
end)
if not ok then loadstring("--roguelike: events failed: " .. tostring(err)) end
)LUA";
    std::string lua = kLua;
    Replace(lua, "@NAMES@", NamesLua());
    Replace(lua, "@SEED@", std::to_string(seed % 2147483646 + 1));
    Replace(lua, "@PICKS@", picks);
    static LiveSwitch test("FM2_ROGUELIKE_TEST"); // every event one lap of the shortest layout, for testing
    Replace(lua, "@TEST@", test ? "true" : "false");
    Replace(lua, "@LEVEL@", std::to_string(level));
    Replace(lua, "@TOP@", std::to_string(kTopLevel));
    return lua;
}

bool Replace(std::string& text, std::string_view from, std::string_view to)
{
    const size_t at = text.find(from);
    if (at == std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    return true;
}
}

// Rogue Mode is switched on (FM2 Live stays off then: kernel/runtime.cpp).
bool RogueModeOn() { return Installed(); }

// For hooks/random_events.cpp, as a career race's result is written: one more race in the run.
void RoguelikeRaceFinished(uint32_t eventId)
{
    if (!Installed())
        return;
    std::lock_guard lock(g_mutex);
    State s = Load();
    if (!s.run)
        return;
    s.races++;
    s.set++; // every race brings a new set of events
    s.picks.clear();
    Save(s);
    Log("roguelike: race %u finished (event %u); set %u next", s.races, eventId, s.set);
}

// For hooks/lua.cpp, as Lua text loads unrun: the flows' notes (roguelike_ui.py). True if it was one.
bool RoguelikeLuaNote(const char* text, uint32_t size)
{
    static constexpr std::string_view kNote = "--roguelike: ";
    if (!Installed() || size < kNote.size() || std::string_view(text, kNote.size()) != kNote)
        return false;
    const std::string note(text + kNote.size(), size - kNote.size());
    unsigned car = 0;
    if (note == "run started")
    {
        std::lock_guard lock(g_mutex);
        Save(State{ true, 0, 0, 0 });
        Log("roguelike: run started (%s)", StateFile().string().c_str());
    }
    else if (note.rfind("picks ", 0) == 0) // the set's four cars, chosen as its events were first built
    {
        std::lock_guard lock(g_mutex);
        State s = Load();
        s.picks = note.substr(6);
        Save(s);
        Log("roguelike: set %u is built on garage cars %s", s.set, s.picks.c_str());
    }
    else if (sscanf(note.c_str(), "got %u", &car) == 1)
    {
        std::lock_guard lock(g_mutex);
        State s = Load();
        s.given = s.races;
        s.pending = 0;
        Save(s);
        Log("roguelike: car %u given for race %u", car, s.races);
    }
    else if (note.rfind("part ", 0) == 0)
        Log("roguelike: a win's part: PartEnum/part %s", note.c_str() + 5);
    else if (note == "part installed")
        Log("roguelike: the win's part is installed");
    else if (note == "events built")
        Log("roguelike: Event Select's events built from the garage");
    else if (note.rfind("events failed", 0) == 0)
        Log("roguelike: Event Select's events weren't built: %s", note.c_str());
    else if (sscanf(note.c_str(), "reveal %u", &car) == 1 && car)
    {
        std::lock_guard lock(g_revealMutex);
        g_reveal.push_back(car);
        Log("roguelike: the car reward screen shows car %u (%zu waiting)", car, g_reveal.size());
    }
    else if (sscanf(note.c_str(), "car lost %u", &car) == 1)
        Log("roguelike: two strikes: garage car %u is lost", car);
    else if (sscanf(note.c_str(), "car gone %u", &car) == 1) // sold: its strikes go with it, should its garage id come back
    {
        std::lock_guard lock(g_mutex);
        State s = Load();
        if (s.strikes.erase(car))
            Save(s);
    }
    else
        Log("roguelike: unknown note \"%s\"", note.c_str());
    return true;
}

// For hooks/lua.cpp, as compiled Lua loads (the UI's behaviors): the career menus' footer (C_MYC_CareerBar.lua) shows
// the car's name with its strikes after it, as the title bar's crosses (the first red with one; the garage's Lvl is
// the car's strikes, roguelike_ui.py's SYNC_STRIKES), or empty when unchanged. The footer's query is a string constant,
// swapped whole: a Lua 5.1 chunk's string is its size_t length (the NUL counted) and its bytes, with nothing pointing
// into the chunk. The name comes from RogueShortNames (Forza.xml's main menu makes it), as plain text: ModelShort is a
// string reference ("_&..."), not known to resolve with text after it; a car without a name there keeps the game's.
std::string RoguelikeLuaBytecode(const char* data, uint32_t size)
{
    static constexpr std::string_view kBarQuery = "SELECT ModelShort AS ModelName, Powertrains.SmallIconPath AS "
                                                  "PowertrainIconSmall, Garage.ClassID, Garage.PerformanceIndex\n\t\tFROM ";
    // (the font put back after: {font} otherwise stays for the texts drawn after it)
    static constexpr std::string_view kMarks = "{font=''Bullet , 38''}{colour=''70 70 70 255''}X X{font_pop}";
    static constexpr std::string_view kStruck =
        "{font=''Bullet , 38''}{colour=''210 20 20 255''}X {colour=''70 70 70 255''}X{font_pop}";
    const std::string_view in(data, size);
    // big-endian, 4-byte size_t (the header's bytes 6 and 8)
    if (!Installed() || size < 12 || data[6] != 0 || data[8] != 4)
        return {};
    const size_t at = in.find(kBarQuery);
    if (at == in.npos || at < 4)
        return {};
    const auto* p = reinterpret_cast<const uint8_t*>(data) + at - 4;
    const uint32_t length = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    if (length != kBarQuery.size() + 1 || at + length > size || data[at + length - 1] != 0)
    {
        Log("roguelike: the career bar's query isn't a whole string constant here; its footer keeps the game's");
        return {};
    }
    const std::string query = "SELECT COALESCE((SELECT Name FROM RogueShortNames WHERE Id = Garage.CarId) || CASE WHEN "
                              "Garage.Lvl >= 1 THEN '   " + std::string(kStruck) + "' ELSE '   " + std::string(kMarks) +
                              "' END, ModelShort) AS ModelName, Powertrains.SmallIconPath AS PowertrainIconSmall, "
                              "Garage.ClassID, Garage.PerformanceIndex\n\t\tFROM ";
    std::string out(in.substr(0, at - 4));
    const uint32_t n = uint32_t(query.size() + 1);
    out += char(n >> 24);
    out += char(n >> 16);
    out += char(n >> 8);
    out += char(n);
    out += query;
    out += '\0';
    out += in.substr(at + length);
    static std::atomic<bool> logged;
    if (!logged.exchange(true))
        Log("roguelike: the career bar's footer shows the car's strikes");
    return out;
}

// For hooks/lua.cpp, as a flow's Lua loads: the markers filled in (roguelike_ui.py), or empty when unchanged.
std::string RoguelikeLuaSource(const char* text, uint32_t size)
{
    static constexpr std::string_view kRun = "false --[[roguelike: run]]";
    static constexpr std::string_view kRegion = "2 --[[roguelike: region]]";
    static constexpr std::string_view kCar = "0 --[[roguelike: car]]";
    static constexpr std::string_view kTypesOrder = "FROM CareerEventTypes ORDER BY id"; // Race Type's list
    static constexpr std::string_view kEvents = "SELECT Id,Name,Level,Description,EventIcon"; // Event Select's
    // CarReveal.xml, after a race: the car won (0: none, -1: a win without one, -2: a strike, -3: the second, the
    // car raced is lost)
    static constexpr std::string_view kNext = "return 0 --[[roguelike: next car ";
    static constexpr std::string_view kStrikes = "return 0 --[[roguelike: strikes "; // "<gid>]]": CareerMenu.xml's title
    // and the run's level there and in the main menu's career box (-1: not a run)
    static constexpr std::string_view kLevel = "return 0 --[[roguelike: level]]";
    // every car's strikes, { [gid] = n }: written to the garage's car level (Lvl), which Select Car shows as Strikes
    static constexpr std::string_view kAllStrikes = "return {} --[[roguelike: all strikes]]";
    // CarReveal.xml, the last car lost: the run starts again, on a random Class D car
    static constexpr std::string_view kReset = "return 0 --[[roguelike: reset car]]";
    static constexpr std::string_view kRoll = "return 0 --[[roguelike: roll "; // a random 1..N (CarReveal.xml's part)
    const std::string_view in(text, size);
    if (!Installed())
        return {};
    static constexpr std::string_view kName = "return '' --[[roguelike: name "; // "<make> <model>" (CarReveal.xml)
    if (in.rfind(kName, 0) == 0)
    {
        unsigned make = 0, car = 0;
        sscanf(std::string(in.substr(kName.size())).c_str(), "%u %u", &make, &car);
        return "return " + LuaString(CarName(make, car));
    }
    if (in.rfind(kRoll, 0) == 0)
    {
        const uint32_t n = uint32_t(strtoul(std::string(in.substr(kRoll.size())).c_str(), nullptr, 10));
        return "return " + std::to_string(n ? 1 + Rng()() % n : 0);
    }
    if (in.rfind(kStrikes, 0) == 0)
    {
        std::lock_guard lock(g_mutex);
        const State s = Load();
        const auto it = s.strikes.find(uint32_t(strtoul(std::string(in.substr(kStrikes.size())).c_str(), nullptr, 10)));
        return "return " + std::to_string(s.run && it != s.strikes.end() ? it->second : 0);
    }
    // Forza.xml's main menu: the short model names, { [car] = "name" }, for the footer's query (RoguelikeLuaBytecode)
    static constexpr std::string_view kShortNames = "return {} --[[roguelike: short names]]";
    if (in.rfind(kShortNames, 0) == 0)
    {
        std::string out = "return {";
        if (const auto it = Names().find("short"); it != Names().end())
            for (const auto& [id, name] : it->second)
                out += "[" + std::to_string(id) + "]=" + LuaString(name) + ",";
        return out + "}";
    }
    if (in.rfind(kAllStrikes, 0) == 0)
    {
        std::lock_guard lock(g_mutex);
        const State s = Load();
        std::string out = "return {";
        if (s.run)
            for (const auto& [gid, n] : s.strikes)
                out += " [" + std::to_string(gid) + "] = " + std::to_string(n) + ",";
        return out + " }";
    }
    if (in.rfind(kLevel, 0) == 0)
    {
        std::lock_guard lock(g_mutex);
        const State s = Load();
        return "return " + std::to_string(s.run ? int(s.level) : -1); // -1: not a run
    }
    if (in.rfind(kReset, 0) == 0)
    {
        std::lock_guard lock(g_mutex);
        State s = Load();
        const uint32_t car = DrawCar(0);
        Log("roguelike: run lost at level %u: a new run on car %u", s.level, car);
        s.level = 0;
        s.strikes.clear();
        s.picks.clear();
        Save(s);
        return "return " + std::to_string(car);
    }
    if (in.rfind(kNext, 0) == 0)
    {
        unsigned event = 0, place = 0, cls = 0, gid = 0;
        sscanf(std::string(in.substr(kNext.size())).c_str(), "event=%u place=%u class=%u gid=%u", &event, &place, &cls, &gid);
        std::lock_guard lock(g_mutex);
        State s = Load();
        if (!s.run || s.given >= s.races)
            return "return 0";
        static LiveSwitch anyWin("FM2_ROGUELIKE_WIN");   // every finish counts as a win, for testing
        static LiveSwitch anyLoss("FM2_ROGUELIKE_LOSE"); // every finish counts as 4th or worse, for testing
        if (anyLoss)
            place = 8;
        else if (anyWin)
            place = 1;
        // the race's first answer: a podium is a level up and clears the car's strikes, 4th or worse a strike for it
        if (!s.pending)
        {
            if (place >= 1 && place <= 3)
            {
                s.strikes.erase(gid);
                s.level = std::min(s.level + 1, kTopLevel);
            }
            else if (place >= 4)
                s.strikes[gid]++;
        }
        if (place != 1 && !s.pending) // only a win earns a car: this race is done without one
        {
            s.given = s.races;
            int answer = 0;
            if (place >= 4 && s.strikes[gid] >= 2)
            {
                s.strikes.erase(gid);
                answer = -3;
            }
            else if (place >= 4)
                answer = -2;
            else
                s.strikes.erase(gid);
            Save(s);
            Log("roguelike: race %u (event %u, place %u, garage car %u): no car (not a win), level %u%s", s.races, event,
                place, gid, s.level, answer == -3 ? ", its second strike: it's lost" : answer == -2 ? ", a strike" : "");
            return "return " + std::to_string(answer);
        }
        static LiveSwitch alwaysCar("FM2_ROGUELIKE_CAR"); // every win earns a car, for testing
        if (!s.pending && !alwaysCar && Rng()() % kCarChance != 0) // a win without a car: its part only (-1)
        {
            s.given = s.races;
            Save(s);
            Log("roguelike: race %u (event %u, place %u): a win, no car (1 in %u), level %u", s.races, event, place, kCarChance,
                s.level);
            return "return -1";
        }
        if (!s.pending)
        {
            s.pending = DrawNextCar(event, place, cls);
            Save(s);
            Log("roguelike: race %u (event %u, place %u, class %u): next car %u", s.races, event, place, cls, s.pending);
        }
        return "return " + std::to_string(s.pending);
    }
    static constexpr std::string_view kFull = "false --[[roguelike: full]]"; // the full-garage check
    if (in.find(kRun) == in.npos && in.find(kCar) == in.npos && in.find(kTypesOrder) == in.npos && in.find(kEvents) == in.npos &&
        in.find(kFull) == in.npos)
        return {};
    std::string out(in);
    // Go Race lists only the run's category
    Replace(out, kTypesOrder, "FROM CareerEventTypes WHERE id=11 ORDER BY id");
    static LiveSwitch full("FM2_ROGUELIKE_FULL"); // the garage counts as full, to test the block
    if (full)
        Replace(out, kFull, "true --[[roguelike: full]]");
    if (out.find(kEvents) != out.npos)
    {
        const State s = Load();
        uint32_t seed = 2166136261u;
        for (char c : StateFile().string())
            seed = (seed ^ uint8_t(c)) * 16777619u;
        out.insert(0, EventBuilder(seed + s.set * 7919u, s.picks, s.level));
    }
    if (out.find(kRun) != out.npos)
    {
        const bool run = RunStarted();
        Log("roguelike: run check: %s", run ? "a run" : "not a run (Career turns it away if it has cars)");
        if (run)
            Replace(out, kRun, "true --[[roguelike: run]]");
    }
    if (out.find(kCar) != out.npos) // a new career: its home region and first car
    {
        const uint32_t region = 1 + Rng()() % 3, car = DrawCar(0);
        if (!car)
            Log("roguelike: no Class D cars in RoguelikeCars.txt; the new career keeps the flow's defaults");
        else
        {
            Replace(out, kRegion, std::to_string(region) + " --[[roguelike: region]]");
            Replace(out, kCar, std::to_string(car) + " --[[roguelike: car]]");
            Log("roguelike: new career: home region %u, car %u", region, car);
        }
    }
    return out == in ? std::string() : out;
}

#if FM2_TITLE_UPDATE
// Pasted together so tools/xexremap.py leaves these title update addresses be (as in random_events.cpp).
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

// RewardReveal::CheckForCarUnlocks(this): a car to show?
PPC_FUNC_IMPL(TU_IMP(822A54E0));
PPC_FUNC(TU_SUB(822A54E0))
{
    {
        std::lock_guard lock(g_revealMutex);
        if (!g_reveal.empty())
        {
            ctx.r3.u64 = 1;
            return;
        }
    }
    TU_IMP(822A54E0)(ctx, base);
}

// RewardReveal::GetCurrentCarUnlock(this): the car to show (taken off the list).
PPC_FUNC_IMPL(TU_IMP(822A5510));
PPC_FUNC(TU_SUB(822A5510))
{
    {
        std::lock_guard lock(g_revealMutex);
        if (!g_reveal.empty())
        {
            const uint32_t car = g_reveal.front();
            g_reveal.pop_front();
            Log("roguelike: car reward screen: car %u", car);
            ctx.r3.u64 = car;
            return;
        }
    }
    TU_IMP(822A5510)(ctx, base);
}
#endif
