#pragma once
// The virtual network under FM2_LIVE (kernel/net.cpp).
#include <cstdint>
#include <vector>

// FM2_LIVE: the profile is signed in to Xbox Live and the virtual network is on (imports_xam.cpp).
bool LiveTrace();
// This PC's XNADDR (0x24 bytes at a guest address): its id on the FM2 Live server once it
// has one, else its address on the loopback network.
void NetSelfXnAddr(uint32_t xnaddr);
// Sessions to the server (sessions.cpp builds the records): publish (or withdraw) ours, or search.
void NetSendMatch(bool search, const std::vector<uint8_t>& body);
// The server's answer to a search (sessions.cpp).
void SessionsMatchResults(const uint8_t* p, size_t n);
