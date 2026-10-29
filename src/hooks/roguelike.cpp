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
// to show; it keeps the race's unlocked cars as a vector of car IDs (this+12..+16). A flow names the run's car with
// "--roguelike: reveal <car>", and the hooks below show it once: CheckForCarUnlocks ("not empty") says yes and
// GetCurrentCarUnlock (which takes the first) returns it. Otherwise both are the game's own.
#include <algorithm>
#include <atomic>
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
std::atomic<uint32_t> g_revealCar{ 0 }; // the car the reward screen shows next, or 0

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
    return s;
}

void Save(const State& s)
{
    std::ofstream(StateFile(), std::ios::trunc) << (s.run ? "run=1\n" : "") << "races=" << s.races << "\ngiven=" << s.given
                                                 << "\npending=" << s.pending << "\n";
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
const std::string& NamesLua()
{
    static const std::string lua = [] {
        std::map<std::string, std::string> kinds;
        std::ifstream in(PackFolder() / "RoguelikeNames.txt");
        for (std::string line; std::getline(in, line);)
        {
            const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos)
                continue;
            std::string name;
            for (char c : line.substr(b + 1))
                name += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
            kinds[line.substr(0, a)] += "[" + line.substr(a + 1, b - a - 1) + "]=\"" + name + "\",";
        }
        std::string out = "{";
        for (const char* kind : { "make", "country", "track", "cylinders", "car" })
            out += std::string(kind) + "={" + kinds[kind] + "},";
        return out + "}";
    }();
    return lua;
}

// Lua run as Event Select loads (before its events query): the run's four events built from the garage
// (FM2-roguelike-scope.md, "Events built from the garage"). Four cars are picked: the newest always, the rest spread
// across the classes owned. Each event takes its car's class (as upgraded) and a theme the car passes with at least
// seven AI cars of that class (else "Class X Open"), a random track, the tier's distance in laps, an AI field from
// those cars, and no pay. Rules other than the last are joined with 'And' (the game's query has no joiner
// otherwise). The seed is the run's race count, so the events stay the same until the next race.
std::string EventBuilder(uint32_t seed)
{
    static const char* kLua = R"LUA(
local ok, err = pcall(function()
if tonumber(UI.Storage.CareerEventType) ~= 11 then return end
local NAMES = @NAMES@
local s = @SEED@
local TEST = @TEST@
local function rnd(n) s = math.mod(s * 16807, 2147483647) return math.floor(s / 2147483647 * n) + 1 end
local function shuffle(t) for i = table.getn(t), 2, -1 do local j = rnd(i) t[i], t[j] = t[j], t[i] end end
local function q(sql) return DB:execute(sql):toTable() or {} end
local function x(sql) DB:execute(sql) end
local function sq(text) return "'" .. (string.gsub(text, "'", "''")) .. "'" end
-- name, distance (m), AI skill on Intermediate (AISkills: 1 best .. 20; Novice 4 easier, Hardcore 4 harder), the
-- generator's torque-boosted skills (+100: 5% more torque, +200: 10%), AI aggression (AITemperaments: 1 most .. 10)
local TIERS = { { "Easy", 10000, 16, 0, 8 }, { "Medium", 15000, 11, 0, 6 }, { "Hard", 20000, 6, 100, 4 }, { "Extreme", 25000, 1, 200, 2 } }
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
local picks, used, others = { garage[n] }, { [garage[n].cls] = true }, {}
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
    local laps = math.max(1, math.floor(tier[2] / track.len + 0.5))
    if TEST then -- FM2_ROGUELIKE_TEST: one lap of the shortest layout
        for _, t in ipairs(tracks) do if t.len < track.len then track = t end end
        laps = 1
    end
    x("UPDATE Races SET TrackId = " .. track.id .. ", NumLaps = " .. laps .. " WHERE EventId = " .. id)
    -- the AI field, by the tier: the theme's cars in stock PI order, against the PI of the car the event is built on
    -- (as upgraded): Easy the slowest seven, Medium the seven nearest it, Hard the seven from it up, Extreme the fastest
    table.sort(pool, function(a, b) return a.pi < b.pi end)
    local np, from = table.getn(pool), 1
    if slot == 2 or slot == 3 then
        local at = 1
        for i, p in ipairs(pool) do if p.pi <= (c.gpi or c.pi) then at = i end end
        from = slot == 2 and at - 3 or at + 1
    elseif slot == 4 then
        from = np - 6
    end
    from = math.max(1, math.min(from, np - 6))
    local field = {}
    for i = from, math.min(np, from + 6) do table.insert(field, pool[i]) end
    shuffle(field)
    local skill, boost, aggro = tier[3], tier[4], tier[5]
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
      ", CashPrize = 0 WHERE Id = " .. id)
    x("UPDATE EventScoring SET Credits = 0 WHERE Event_ID = " .. id)
end
loadstring("--roguelike: events built")
end)
if not ok then loadstring("--roguelike: events failed: " .. tostring(err)) end
)LUA";
    std::string lua = kLua;
    Replace(lua, "@NAMES@", NamesLua());
    Replace(lua, "@SEED@", std::to_string(seed % 2147483646 + 1));
    static LiveSwitch test("FM2_ROGUELIKE_TEST"); // every event one lap of the shortest layout, for testing
    Replace(lua, "@TEST@", test ? "true" : "false");
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
    Save(s);
    Log("roguelike: race %u finished (event %u)", s.races, eventId);
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
    else if (sscanf(note.c_str(), "got %u", &car) == 1)
    {
        std::lock_guard lock(g_mutex);
        State s = Load();
        s.given = s.races;
        s.pending = 0;
        Save(s);
        Log("roguelike: car %u given for race %u", car, s.races);
    }
    else if (note == "events built")
        Log("roguelike: Event Select's events built from the garage");
    else if (note.rfind("events failed", 0) == 0)
        Log("roguelike: Event Select's events weren't built: %s", note.c_str());
    else if (sscanf(note.c_str(), "reveal %u", &car) == 1 && car)
    {
        g_revealCar = car;
        Log("roguelike: the car reward screen shows car %u", car);
    }
    else
        Log("roguelike: unknown note \"%s\"", note.c_str());
    return true;
}

// For hooks/lua.cpp, as a flow's Lua loads: the markers filled in (roguelike_ui.py), or empty when unchanged.
std::string RoguelikeLuaSource(const char* text, uint32_t size)
{
    static constexpr std::string_view kRun = "false --[[roguelike: run]]";
    static constexpr std::string_view kRegion = "2 --[[roguelike: region]]";
    static constexpr std::string_view kCar = "0 --[[roguelike: car]]";
    static constexpr std::string_view kTypesOrder = "FROM CareerEventTypes ORDER BY id"; // Race Type's list
    static constexpr std::string_view kEvents = "SELECT Id,Name,Level,Description,EventIcon"; // Event Select's
    static constexpr std::string_view kNext = "return 0 --[[roguelike: next car "; // CarReveal.xml, after a race
    const std::string_view in(text, size);
    if (!Installed())
        return {};
    if (in.rfind(kNext, 0) == 0)
    {
        unsigned event = 0, place = 0, cls = 0;
        sscanf(std::string(in.substr(kNext.size())).c_str(), "event=%u place=%u class=%u", &event, &place, &cls);
        std::lock_guard lock(g_mutex);
        State s = Load();
        if (!s.run || s.given >= s.races)
            return "return 0";
        static LiveSwitch anyWin("FM2_ROGUELIKE_WIN"); // every finish counts as a win, for testing
        if (place != 1 && !anyWin && !s.pending) // only a win earns a car: this race is done without one
        {
            s.given = s.races;
            Save(s);
            Log("roguelike: race %u (event %u, place %u): no car (not a win)", s.races, event, place);
            return "return 0";
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
        out.insert(0, EventBuilder(seed + s.races * 7919u));
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
    if (g_revealCar)
    {
        ctx.r3.u64 = 1;
        return;
    }
    TU_IMP(822A54E0)(ctx, base);
}

// RewardReveal::GetCurrentCarUnlock(this): the car to show (taken off the list).
PPC_FUNC_IMPL(TU_IMP(822A5510));
PPC_FUNC(TU_SUB(822A5510))
{
    if (const uint32_t car = g_revealCar.exchange(0))
    {
        Log("roguelike: car reward screen: car %u", car);
        ctx.r3.u64 = car;
        return;
    }
    TU_IMP(822A5510)(ctx, base);
}
#endif
