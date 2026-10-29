// Zip overlays: members of the game's archives replaced as the game opens them. The game reads its UI,
// string tables and data from zips (game:\media\ui.zip, the title update's update:\media.zip, ...); for a zip
// with overrides, a patched copy is built in Documents\fm2\cache\overlay (again when its inputs change) and
// that copy is opened instead. Overrides come from two places:
//   - built in (below): the main menu's X is Quit, with the game's own confirm pop-up
//   - mods: Documents\fm2\mods\<mod>\<zip path>\<member path>, e.g.
//       mods\MyMod\game\media\ui.zip\Flows\Forza.xml       (a file of the disc's ui.zip)
//       mods\MyMod\update\media.zip\UI\Flows\Options.xml   (a file of the title update's media.zip)
//     A file there replaces the member of that name, or is added. Mods apply in name order, after the built-in
//     changes; a mod folder starting with '_' or '.' is skipped.
// Members we change are stored (not compressed); the rest are copied byte for byte. FM2_NO_OVERLAY=1 opens
// the original zips (no built-in changes, no mods).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "kernel/runtime.h"
#include "kernel/vfs.h"
#include "log.h"
#include "settings.h"
#include "zip.h"

namespace fs = std::filesystem;

namespace
{
std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(tolower(uint8_t(c)));
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// ---- built-in: X on the main menu is Quit ----------------------------------------------------------------
//
// The X button opened the Xbox Live Marketplace, which never answers here (the game sat on a loading
// screen). The main menu footer (P_MAI_Footer.lua, compiled; this is its source with onX changed) now fires
// FM2Quit, which the main menu's flow (Forza.xml) answers with the game's confirm pop-up; Yes calls
// Game:ShowMarketplace(), whose native the quit hook replaces (src/hooks/quit_button.cpp). The button's
// label is IDS_Marketplace in each loadnewprofile_Flow.str, rewritten to "Quit" in that language.

const char kFooter[] = R"lua(
self.onInitialize = function(self)
    if not ProfileManager:IsFakeGamerProfile(Lobby.activePlayer.profile.port) then
        HelpButtonTexts.X = "{colour='255 255 255 255'}[UI.StringTables.loadnewprofile_Flow.IDS_Marketplace]"
        self:registerForEvent(scene.LoggedInPlayerInput, "x_press", self.onX)
    end
    self.parent.refreshComponent = function(self)
        self["C_MAI_Help Text"]["Help Text"].DataBoundText:set("mDataString", self.mHelpTextDataString)
    end
    self.parent.onUpdate = function(self)
        self:refreshComponent()
        self.onUpdate = nil
    end
end

self.onX = function(self)
    Scene:fireControlEvent("Alt2")
    UI:fireEvent("FM2Quit")
end
)lua";

// Into the main menu's branch (after its Options choice), and the pop-up's scene into the menu's package.
const char kQuitChoice[] = R"xml(
            <choice event="FM2Quit">
                <popup path="GAME:\Media\UI\Scenes\PopUp_Message.xgf">
                    PopupTitle = UI.StringTables.LiveCircuitRace_flow.IDS_AreYouSure
                    PopupMessage = UI.StringTables.LiveCircuitRace_flow.IDS_AreYouSure_Msg
                    HelpButtonTexts =
                    {
                        A = "{colour='255 255 255 255'}[UI.Strings.IDS_Yes]",
                        B = "{colour='255 255 255 255'}[UI.Strings.IDS_Cancel]"
                    }
                </popup>
                <logic>
                    Game:ShowMarketplace()
                </logic>
            </choice>)xml";
const char kPopupPackage[] = R"xml(
            <scene path="GAME:\Media\UI\Scenes\PopUp_Message.xgf"/>
            <package name="GAME:\Media\UI\Scenes\PopUp_Message.xgf"/>)xml";

bool InsertAfter(std::string& s, const std::string& anchor, size_t from, const std::string& text, size_t* at = nullptr)
{
    const size_t i = s.find(anchor, from);
    if (i == std::string::npos)
        return false;
    s.insert(i + anchor.size(), text);
    if (at)
        *at = i;
    return true;
}

bool PatchForzaFlow(std::vector<uint8_t>& data)
{
    std::string s(data.begin(), data.end());
    // The main menu flow: the "invite" flow's branch ends with the Options choice.
    const size_t menu = s.find("<flow label=\"invite\">");
    if (menu == std::string::npos || s.find("FM2Quit") != std::string::npos)
        return false;
    const size_t options = s.find("<flow path=\"game:\\Media\\ui\\Flows\\Options.xml\"/>", menu);
    if (options == std::string::npos || !InsertAfter(s, "</choice>", options, kQuitChoice))
        return false;
    // MainMenuCore loads the main menu's scenes: the pop-up's scene too.
    const size_t core = s.find("<package name=\"MainMenuCore\">");
    if (core == std::string::npos || !InsertAfter(s, "p_mai_footer.lua\"/>", core, kPopupPackage))
        return false;
    data.assign(s.begin(), s.end());
    return true;
}

// The X button's label word, and "Quit" in the same language (no longer than it).
struct Word
{
    const char* table;
    const char16_t* marketplace;
    const char16_t* quit;
};
const Word kWords[] = {
    { "en", u"Marketplace", u"Quit" },      { "it", u"Marketplace", u"Esci" },    { "de", u"Marktplatz", u"Beenden" },
    { "fr", u"Marché", u"Sortir" },     { "es", u"Bazar", u"Salir" },         { "jp", u"マーケットプレース", u"終了" },
    { "ko", u"마켓플레이스", u"종료" }, { "chs", u"卖场", u"退出" },
    { "cht", u"賣場", u"離開" }, { "pl", u"Rynek usług", u"Wyjdź" }, { "cz", u"Tržiště", u"Konec" },
    { "hu", u"Piactér", u"Kilépés" },
};

std::vector<uint8_t> Utf16Be(const char16_t* s)
{
    std::vector<uint8_t> out;
    for (; *s; s++)
        out.push_back(uint8_t(*s >> 8)), out.push_back(uint8_t(*s));
    return out;
}

// An LSB2 string table (big-endian UTF-16): the whole string `from` (NULs on both sides) becomes `to`
// padded with NULs, so no offset in the table moves.
bool PatchTable(std::vector<uint8_t>& t, const char16_t* from, const char16_t* to)
{
    const std::vector<uint8_t> a = Utf16Be(from), b = Utf16Be(to);
    if (b.size() > a.size())
        return false;
    for (size_t i = 2; i + a.size() + 2 <= t.size(); i += 2)
        if (!t[i - 2] && !t[i - 1] && !memcmp(&t[i], a.data(), a.size()) && !t[i + a.size()] && !t[i + a.size() + 1])
        {
            std::fill(t.begin() + ptrdiff_t(i), t.begin() + ptrdiff_t(i + a.size()), uint8_t(0));
            std::copy(b.begin(), b.end(), t.begin() + ptrdiff_t(i));
            return true;
        }
    return false;
}

// ---- built-in: louder exhaust --------------------------------------------------------------------------
//
// With the launcher's car sound mix on, each car's exhaust is scaled as its audio tuning zip opens: the
// exhaust's loop volumes (HarmonicTuning: <ExhaustL>/<ExhaustR> Loop volume="...", apart from EngineA/B).
// Above 1 still gets louder. The turbo is made louder in its samples instead (audio/car_sound.cpp): its
// level here (DSPTuning's TurboVolume) is capped at full volume by the game.
constexpr const char* kHarmonicZip = "media/audio/cars/engines/harmonictuning.zip";

// The exhaust's scale (1 = the game's own; 1 with the mix off).
float ExhaustScale()
{
    const Settings& s = GetSettings();
    return s.carSoundMix ? std::clamp(s.exhaustVolume, 0u, 400u) / 100.0f : 1.0f;
}

// Every attr="number" in s[from, to) times `scale`; the count changed.
int ScaleAttr(std::string& s, size_t from, size_t to, const std::string& attr, float scale)
{
    const std::string key = attr + "=\"";
    int n = 0;
    for (size_t at = s.find(key, from); at != s.npos && at < to; at = s.find(key, at))
    {
        const size_t v = at + key.size(), end = s.find('"', v);
        if (end == s.npos)
            break;
        char text[32];
        snprintf(text, sizeof(text), "%f", strtof(s.c_str() + v, nullptr) * scale);
        s.replace(v, end - v, text);
        to += strlen(text) - (end - v);
        at = v;
        n++;
    }
    return n;
}

bool ScaleCarSound(const std::string& zip, std::vector<uint8_t>& data)
{
    const float exhaust = ExhaustScale();
    std::string s(data.begin(), data.end());
    int n = 0;
    if (zip == kHarmonicZip && exhaust != 1.0f)
        for (const char* side : { "<ExhaustL", "<ExhaustR" })
            for (size_t at = s.find(side); at != s.npos; at = s.find(side, at + 1))
            {
                const size_t end = s.find(std::string("</") + (side + 1), at);
                if (end == s.npos)
                    break;
                n += ScaleAttr(s, at, end, " volume", exhaust);
            }
    if (!n)
        return false;
    data.assign(s.begin(), s.end());
    return true;
}

// Built-in changes to one member: true if it was changed. `zip` is the zip's path under its root
// ("media/ui.zip"), `member` the member's lowercase name.
bool BuiltIn(const std::string& zip, const std::string& member, std::vector<uint8_t>& data)
{
    if (zip == kHarmonicZip)
        return ScaleCarSound(zip, data);
    if (zip.size() >= 9 && zip.compare(zip.size() - 9, 9, "media.zip") == 0)
    {
        if (member == "ui/behaviors/components/p_comp/p_mai/p_mai_footer.lua")
        {
            data.assign(kFooter, kFooter + sizeof(kFooter) - 1);
            return true;
        }
        if (member.rfind("stringtables/", 0) == 0 && member.find("/loadnewprofile_flow.str") != std::string::npos)
        {
            const std::string lang = member.substr(13, member.find('/', 13) - 13);
            for (const Word& w : kWords)
                if (lang == w.table)
                    return PatchTable(data, w.marketplace, w.quit);
        }
    }
    if (zip == "media/ui.zip" && member == "flows/forza.xml")
        return PatchForzaFlow(data);
    return false;
}

// ---- mods ------------------------------------------------------------------------------------------------

struct ModFile
{
    std::string member; // as it goes in the zip ('/', the mod's own case)
    fs::path file;
};

// The mods' files for one zip, in apply order (mods by name; a later mod wins).
std::vector<ModFile> ModFiles(const std::string& zip)
{
    std::vector<ModFile> files;
    std::error_code ec;
    const fs::path mods = UserDir() / "mods";
    if (!fs::is_directory(mods, ec))
        return files;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(mods, ec))
        if (e.is_directory(ec) && e.path().filename().string()[0] != '_' && e.path().filename().string()[0] != '.')
            dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& dir : dirs)
    {
        // The zip's folder inside the mod, matched case-insensitively: <mod>\game\media\ui.zip\...
        for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        {
            if (!e.is_regular_file(ec))
                continue;
            const std::string rel = fs::relative(e.path(), dir, ec).generic_string();
            const std::string lrel = Lower(rel);
            for (const char* root : { "game/", "update/" })
                if (lrel.rfind(root + zip + "/", 0) == 0)
                    files.push_back({ rel.substr(strlen(root) + zip.size() + 1), e.path() });
        }
    }
    return files;
}

// ---- DLC names and thumbnails for a mod that has the DLC cars ---------------------------------------------
//
// With such a mod (Forza Plus 2) the DLC packs are hidden (XamContentCreateEnumerator): the mod's database
// lists their cars already. The cars' names and dealership thumbnails were only ever in the packs, though:
// each pack's <n>Overrides1_pri_<p>.zip has every language's Data_Car.str (the highest priority one is the
// newest), and its other zips media/UI/Textures/Thumbnails/*. They go into the game's string table and
// texture zips, extracted once to Documentsm2\cache\dlc-names.
std::vector<ModFile> DlcFiles(const std::string& zip)
{
    static const std::map<std::string, std::vector<ModFile>> byZip = [] {
        std::map<std::string, std::vector<ModFile>> out;
        if (!VfsModsCarryDlc())
            return out;
        std::error_code ec;
        const fs::path cache = UserDir() / "cache" / "dlc-names";
        std::map<std::string, std::pair<int, fs::path>> names; // language -> (priority, pack zip)
        std::vector<fs::path> zips;
        for (const auto& pack : fs::directory_iterator(DlcDir(), ec))
            if (pack.is_directory(ec))
                for (const auto& z : fs::directory_iterator(DlcPackageRoot(pack.path()) / "Media" / "DLCZips", ec))
                    if (z.path().extension() == ".zip")
                        zips.push_back(z.path());
        auto extract = [&](const std::string& target, const std::string& member, const std::vector<uint8_t>& data) {
            fs::path file = cache / target / member;
            file.make_preferred();
            if (fs::file_size(file, ec) != data.size())
            {
                fs::create_directories(file.parent_path(), ec);
                std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            }
            out[target].push_back({ member, file });
        };
        for (const fs::path& z : zips)
        {
            const std::string stem = Lower(z.stem().string());
            const size_t pri = stem.find("_pri_");
            std::ifstream in(z, std::ios::binary);
            const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), {} };
            if (stem.find("overrides") != std::string::npos && pri != std::string::npos)
            {
                const int priority = atoi(stem.c_str() + pri + 5);
                ForEachZipMember(bytes, "data_car.str", [&](const std::string& n, const std::vector<uint8_t>&) {
                    const std::string lang = Lower(n.substr(0, n.find('/')));
                    if (!names.count(lang) || names[lang].first < priority)
                        names[lang] = { priority, z };
                });
            }
            else
                ForEachZipMember(bytes, "media/ui/textures/thumbnails/", [&](const std::string& n, const std::vector<uint8_t>& d) {
                    extract("media/ui/textures.zip", "Thumbnails/" + n.substr(n.rfind('/') + 1), d);
                });
        }
        for (const auto& [lang, best] : names)
        {
            std::vector<uint8_t> data;
            if (ReadZipMember(best.second, lang + "/Data_Car.str", data))
                extract("media/stringtables/" + lang + ".zip", "Data_Car.str", data);
        }
        size_t count = 0;
        for (const auto& [_, files] : out)
            count += files.size();
        Log("overlay: %zu DLC names and thumbnails from the packs, for the mod", count);
        return out;
    }();
    auto it = byZip.find(zip);
    return it == byZip.end() ? std::vector<ModFile>{} : it->second;
}

// ---- zip rewriting ---------------------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* p, size_t n)
{
    static uint32_t table[256] = {};
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
void Put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); }
void Put32(std::vector<uint8_t>& o, uint32_t v) { Put16(o, v & 0xFFFF), Put16(o, v >> 16); }

struct Writer
{
    std::vector<uint8_t> out, central;
    uint32_t count = 0;
    void Add(const std::string& name, uint16_t method, uint16_t time, uint16_t date, uint32_t crc, const uint8_t* body, uint32_t csize,
        uint32_t usize)
    {
        const uint32_t offset = uint32_t(out.size());
        Put32(out, 0x04034B50), Put16(out, 20), Put16(out, 0), Put16(out, method), Put16(out, time), Put16(out, date);
        Put32(out, crc), Put32(out, csize), Put32(out, usize), Put16(out, uint32_t(name.size())), Put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), body, body + csize);
        Put32(central, 0x02014B50), Put16(central, 20), Put16(central, 20), Put16(central, 0), Put16(central, method);
        Put16(central, time), Put16(central, date), Put32(central, crc), Put32(central, csize), Put32(central, usize);
        Put16(central, uint32_t(name.size())), Put16(central, 0), Put16(central, 0), Put16(central, 0), Put16(central, 0);
        Put32(central, 0), Put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
        count++;
    }
    void Stored(const std::string& name, const std::vector<uint8_t>& data)
    {
        Add(name, 0, 0, 0x21, Crc32(data.data(), data.size()), data.data(), uint32_t(data.size()), uint32_t(data.size()));
    }
    std::vector<uint8_t> Finish()
    {
        const uint32_t cdStart = uint32_t(out.size());
        out.insert(out.end(), central.begin(), central.end());
        Put32(out, 0x06054B50), Put16(out, 0), Put16(out, 0), Put16(out, count), Put16(out, count);
        Put32(out, uint32_t(central.size())), Put32(out, cdStart), Put16(out, 0);
        return std::move(out);
    }
};

// The zip with its members changed (built in, then mods). `changed` counts the members that differ.
bool Rewrite(const std::string& zipName, const std::vector<uint8_t>& zip, const std::vector<ModFile>& mods, std::vector<uint8_t>& result,
    int& changed)
{
    changed = 0;
    if (zip.size() < 22)
        return false;
    size_t eocd = zip.size() - 22;
    while (eocd > 0 && U32(&zip[eocd]) != 0x06054B50)
        eocd--;
    if (U32(&zip[eocd]) != 0x06054B50)
        return false;
    std::map<std::string, const ModFile*> modByMember; // the last mod wins
    for (const ModFile& m : mods)
        modByMember[Lower(m.member)] = &m;
    // Members to inflate and change: by name, from one pass over the archive.
    std::map<std::string, std::vector<uint8_t>> inflated;
    ForEachZipMember(zip, "", [&](const std::string& n, const std::vector<uint8_t>& d) {
        std::vector<uint8_t> copy = d;
        const std::string l = Lower(n);
        if (modByMember.count(l) || BuiltIn(zipName, l, copy))
            inflated.emplace(l, std::move(copy));
    });
    const uint32_t count = U16(&zip[eocd + 10]), cdOffset = U32(&zip[eocd + 16]);
    Writer w;
    size_t p = cdOffset;
    for (uint32_t e = 0; e < count; e++)
    {
        if (p + 46 > zip.size() || U32(&zip[p]) != 0x02014B50)
            return false;
        const uint16_t method = U16(&zip[p + 10]), time = U16(&zip[p + 12]), date = U16(&zip[p + 14]);
        const uint32_t crc = U32(&zip[p + 16]), csize = U32(&zip[p + 20]), usize = U32(&zip[p + 24]);
        const uint16_t nameLen = U16(&zip[p + 28]), extraLen = U16(&zip[p + 30]), commentLen = U16(&zip[p + 32]);
        const uint32_t local = U32(&zip[p + 42]);
        const std::string name(reinterpret_cast<const char*>(&zip[p + 46]), nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (local + 30 > zip.size() || U32(&zip[local]) != 0x04034B50)
            return false;
        const size_t data = local + 30 + U16(&zip[local + 26]) + U16(&zip[local + 28]);
        if (data + csize > zip.size())
            return false;
        const std::string l = Lower(name);
        if (auto mod = modByMember.find(l); mod != modByMember.end())
        {
            std::ifstream in(mod->second->file, std::ios::binary);
            w.Stored(name, std::vector<uint8_t>{ std::istreambuf_iterator<char>(in), {} });
            modByMember.erase(mod);
            changed++;
        }
        else if (auto it = inflated.find(l); it != inflated.end())
        {
            w.Stored(name, it->second);
            changed++;
        }
        else
            w.Add(name, method, time, date, crc, &zip[data], csize, usize);
    }
    for (const auto& [l, m] : modByMember) // new files from mods
    {
        std::ifstream in(m->file, std::ios::binary);
        w.Stored(m->member, std::vector<uint8_t>{ std::istreambuf_iterator<char>(in), {} });
        changed++;
    }
    result = w.Finish();
    return true;
}

// What the overlay depends on: the zip, the mods' files, and this code's version.
std::string Stamp(const fs::path& zip, const std::vector<ModFile>& mods)
{
    std::error_code ec;
    std::string s = "overlay 3 " + std::to_string(fs::file_size(zip, ec)) + " " + std::to_string(fs::last_write_time(zip, ec).time_since_epoch().count()) +
                    " exhaust " + std::to_string(ExhaustScale());
    for (const ModFile& m : mods)
        s += "\n" + m.file.string() + " " + std::to_string(fs::file_size(m.file, ec)) + " " + std::to_string(fs::last_write_time(m.file, ec).time_since_epoch().count());
    return s;
}
}

// The zip the game should open for `host` (a zip under the game or update folder): `host` itself, or a
// patched copy. `zip` is its path under that folder, lowercase with '/' ("media/ui.zip").
fs::path ZipOverlay(const std::string& zip, const fs::path& host)
{
    static std::mutex mutex;
    static std::map<std::string, fs::path> done;
    static const bool off = getenv("FM2_NO_OVERLAY") != nullptr;
    if (off)
        return host;
    std::lock_guard lock(mutex);
    const float exhaust = ExhaustScale();
    // the exhaust's scale can change while the game runs (the audio options): a zip opened again then is built again
    const std::string key = Lower(host.string()) + " " + std::to_string(exhaust);
    if (auto it = done.find(key); it != done.end())
        return it->second;
    fs::path result = host;
    std::vector<ModFile> mods = DlcFiles(zip);
    for (ModFile& m : ModFiles(zip)) // the mods' own files win
        mods.push_back(std::move(m));
    const bool builtIn = zip == "media/ui.zip" || zip == "media.zip" || (zip == kHarmonicZip && exhaust != 1.0f);
    if (builtIn || !mods.empty())
    {
        std::error_code ec;
        const fs::path dir = UserDir() / "cache" / "overlay";
        std::string flat = Lower(host.parent_path().filename().string() + "_" + zip);
        std::replace(flat.begin(), flat.end(), '/', '_');
        if (zip == kHarmonicZip) // one copy per scale: the game may still have another open
            flat += "_" + std::to_string(int(exhaust * 100)) + ".zip";
        const fs::path patched = dir / flat, stampFile = dir / (flat + ".stamp");
        const std::string want = Stamp(host, mods);
        std::string have;
        {
            std::ifstream in(stampFile, std::ios::binary);
            have.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (have == want && fs::exists(patched, ec))
            result = patched;
        else
        {
            std::ifstream in(host, std::ios::binary);
            const std::vector<uint8_t> original{ std::istreambuf_iterator<char>(in), {} };
            std::vector<uint8_t> out;
            int changed = 0;
            if (Rewrite(zip, original, mods, out, changed) && changed)
            {
                fs::create_directories(dir, ec);
                std::ofstream(patched, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
                std::ofstream(stampFile, std::ios::binary) << want;
                result = patched;
                Log("overlay: %s: %d members changed (%zu mod files), %s", host.string().c_str(), changed, mods.size(), patched.string().c_str());
            }
        }
    }
    done[key] = result;
    return result;
}
