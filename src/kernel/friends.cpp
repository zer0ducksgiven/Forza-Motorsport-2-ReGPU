// The friends list under FM2_LIVE. Xbox Live friends don't exist here, and FM2 only gifts cars to friends, so
// "add a friend" (XamShowFriendsUI) asks for another player's profile name and looks it up on the FM2 Live
// server (POST /players/find; players are listed there from their title services sign-in). The list is a text
// file in the profile's folder: friends.txt, "<xuid hex> <name>" a line.
#include "kernel/friends.h"

#include <atomic>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "kernel/leaderboards.h"
#include "kernel/runtime.h"
#include "log.h"
#include "render/presenter.h"
#include "settings.h"

namespace
{
std::mutex g_mutex;
std::atomic<uint32_t> g_version{ 1 };

std::filesystem::path FriendsPath() { return ContentDir().parent_path() / "friends.txt"; }

std::vector<LiveFriend> Load()
{
    std::vector<LiveFriend> out;
    std::ifstream in(FriendsPath());
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream fields(line);
        std::string xuid, name;
        if (fields >> xuid && std::getline(fields >> std::ws, name) && xuid.size() == 16 && !name.empty())
            out.push_back({ strtoull(xuid.c_str(), nullptr, 16), name });
    }
    return out;
}

bool SameName(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

// "31 Oct 2026" for a time in ms since 1970
std::wstring Day(long long ms)
{
    const time_t t = time_t(ms / 1000);
    tm local{};
    if (ms <= 0 || localtime_s(&local, &t) != 0)
        return L"a while ago";
    wchar_t text[32];
    wcsftime(text, 32, L"%#d %b %Y", &local);
    return text;
}
}

std::vector<LiveFriend> FriendsList()
{
    std::lock_guard lock(g_mutex);
    return Load();
}

// The file's time is in it too: the launcher's friends list adds to the file while the game runs
uint32_t FriendsVersion()
{
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(FriendsPath(), ec);
    const uint32_t stamp = ec ? 0 : uint32_t(written.time_since_epoch().count() / 10000000);
    return g_version.load() * 2654435761u + stamp;
}

void FriendsAddFromUi()
{
    std::wstring description = L"Type the profile name of another FM2 Live player.";
    for (;;)
    {
        std::wstring typed;
        if (!PresenterAskText(L"Add a friend", description, typed, 15))
            return;
        std::string name;
        for (wchar_t c : typed) // gamertags can have spaces ("Test Debug")
            if (c >= 32 && c < 127)
                name += char(c);
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        while (!name.empty() && name.front() == ' ')
            name.erase(0, 1);
        if (name.empty())
            continue;
        if (SameName(name, GetSettings().profile))
        {
            description = L"That's you. Type another player's profile name.";
            continue;
        }
        // "player <xuid hex> <last seen, ms since 1970> <name>" or "none"; a name is one player's
        std::istringstream reply(LiveServerPost(L"/players/find", name));
        std::string word, xuid, found;
        long long seen = 0;
        if (!(reply >> word >> xuid >> seen) || !std::getline(reply >> std::ws, found) || word != "player" || xuid.size() != 16)
        {
            Log("friends: no player called %s", name.c_str());
            description = L"No player called \"" + typed + L"\" has been on FM2 Live. Check the name and try again.";
            continue;
        }
        // Who it is before adding them: OK adds, Cancel goes back to the name
        std::wstring confirm(found.begin(), found.end());
        const std::wstring shown = confirm;
        if (!PresenterAskText(L"Add a friend", L"Add " + shown + L" (last on FM2 Live " + Day(seen) + L")? OK adds them.", confirm, 15))
        {
            description = L"Type the profile name of another FM2 Live player.";
            continue;
        }
        std::lock_guard lock(g_mutex);
        bool known = false;
        for (const LiveFriend& f : Load())
            known |= SameName(f.name, found);
        if (!known)
        {
            std::ofstream(FriendsPath(), std::ios::app) << xuid << ' ' << found << '\n';
            g_version++;
        }
        Log("friends: %s %s (%s)", known ? "already have" : "added", found.c_str(), xuid.c_str());
        return;
    }
}
