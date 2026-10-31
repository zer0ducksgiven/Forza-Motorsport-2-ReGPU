#pragma once
// The leaderboards under FM2_LIVE, kept by the FM2 Live server (kernel/leaderboards.cpp).
#include <cstdint>
#include <string>
#include <vector>

// A view asked for: its id and the columns (attribute ids) wanted.
struct LeaderboardSpec
{
    uint32_t view = 0;
    std::vector<uint16_t> attributes;
};

// XSessionWriteStats: a player's XSESSION_VIEW_PROPERTIES, sent to the server.
void LeaderboardsWrite(uint64_t xuid, uint32_t viewCount, uint32_t viewPtr);
// X_USER_STATS_READ_RESULTS at out (size bytes) for those views from the server: query is "rank <first>
// <count>", "around <xuid hex> <count>" or "xuids - 0" (then xuids are the players).
void LeaderboardsFill(uint32_t out, uint32_t size, const std::vector<LeaderboardSpec>& specs, const std::string& query,
                      const std::vector<uint64_t>& xuids);

// A POST to the FM2 Live server (FM2_STATS_URL): its reply, or empty on failure.
std::string LiveServerPost(const wchar_t* path, const std::string& body);

// A spec array's specs ({ view id, column count, u16 columns[64] }, 0x88 bytes each).
std::vector<LeaderboardSpec> LeaderboardSpecs(uint32_t specPtr, uint32_t count);
