#pragma once
// Xbox Live sessions under FM2_LIVE (kernel/sessions.cpp).
#include <cstdint>

// XamSessionCreateHandle: a new session object's handle.
uint32_t OnlineSessionCreateHandle();
// XamSessionRefObjByHandle: the guest X_KSESSION of a session handle, 0 for another handle.
uint32_t OnlineSessionObject(uint32_t handle);
// One of the XGI app's (0xFB) messages; false if it isn't a session or stats message.
bool OnlineXgiMessage(uint32_t message, uint32_t buffer, uint32_t size, uint32_t& result);
// This PC's XNADDR (0x24 bytes at a guest address).
void OnlineSelfXnAddr(uint32_t xnaddr);
// The XUID of a local player (imports_xam.cpp).
uint64_t OnlineLocalXuid(uint32_t userIndex);
