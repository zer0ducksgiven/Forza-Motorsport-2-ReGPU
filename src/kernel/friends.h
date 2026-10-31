#pragma once
// The friends list under FM2_LIVE (kernel/friends.cpp): players added by profile name, kept in the profile's
// folder. Gift a Car sends to them (hooks/live_services.cpp hands them to the game's friends list).
#include <cstdint>
#include <string>
#include <vector>

struct LiveFriend
{
    uint64_t xuid = 0;
    std::string name;
};

std::vector<LiveFriend> FriendsList();
// Bumped whenever the list changes, so the game's copy can be refilled.
uint32_t FriendsVersion();
// XamShowFriendsUI: asks for a profile name, finds it on the FM2 Live server and adds it.
void FriendsAddFromUi();
