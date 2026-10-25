// Xbox Live sessions (FM2-online-scope.md, step 2), under FM2_LIVE: the session objects the game
// creates (XamSessionCreateHandle, XamSessionRefObjByHandle) and the XGI app's session messages
// (XMsg app 0xFB). For now a session lives on this PC only: creating one as host fills in its
// session info with our address and a fresh key, joins, starts, ends and deletes are kept, and
// searches find nothing. Message ids and layouts follow the Xenia netplay fork
// (github.com/AdrianCassar/xenia-canary, netplay_canary_experimental: xgi_app.cc, xsession.cc,
// xnet.h; BSD), as the Forza Horizon recomp does.
#include "kernel/sessions.h"

#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

namespace
{
constexpr uint32_t X_E_INVALIDARG = 0x80070057;
constexpr uint32_t X_ONLINE_E_SESSION_WRONG_STATE = 0x80155206;

// XSESSION_INFO: XNKID sessionID (8), XNADDR hostAddress (0x24), XNKEY keyExchangeKey (0x10).
constexpr uint32_t kSessionInfoSize = 0x3C;
// The top byte of a session id says what kind it is.
constexpr uint8_t kSystemLinkId = 0x00, kOnlinePeerId = 0x80;
// Session flags (XSESSION_CREATE_*).
constexpr uint32_t kHost = 0x01, kPresence = 0x02, kStats = 0x04, kMatchmaking = 0x08, kArbitration = 0x10;

std::mt19937_64& Random()
{
    static std::mt19937_64 r{ std::random_device{}() };
    return r;
}

void RandomBytes(uint32_t guest, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
        *GuestPtr<uint8_t>(guest + i) = uint8_t(Random()());
}

struct Session : KernelObject
{
    Session() : KernelObject(Kind::Other) {}
    uint32_t guest = 0; // X_KSESSION { u32 handle }: what the game holds
    uint64_t id = 0;
    uint32_t flags = 0, publicSlots = 0, privateSlots = 0, userIndex = 0;
    uint64_t nonce = 0;
    uint8_t info[kSessionInfoSize] = {};
    bool created = false, started = false;
    std::map<uint64_t, bool> members; // XUID -> private slot
};

std::mutex g_mutex;

// The session behind the X_KSESSION pointer a message carries.
std::shared_ptr<Session> Lookup(uint32_t obj)
{
    if (!obj)
        return nullptr;
    return std::dynamic_pointer_cast<Session>(GetHandle(LoadBE32(obj)));
}

std::string Kind(uint32_t flags)
{
    if (!flags)
        return "offline";
    if (flags & (kPresence | kStats | kMatchmaking | kArbitration))
        return flags & kArbitration ? "Xbox Live, ranked" : "Xbox Live";
    return "system link";
}

// XGI_SESSION_CREATE { obj, flags, public slots, private slots, user, XSESSION_INFO*, u64 nonce* }
uint32_t Create(uint32_t buffer)
{
    auto s = Lookup(LoadBE32(buffer));
    if (!s)
        return X_E_INVALIDARG;
    if (s->created)
        return X_ONLINE_E_SESSION_WRONG_STATE;
    s->flags = LoadBE32(buffer + 4);
    s->publicSlots = LoadBE32(buffer + 8);
    s->privateSlots = LoadBE32(buffer + 12);
    s->userIndex = LoadBE32(buffer + 16);
    const uint32_t infoPtr = LoadBE32(buffer + 20), noncePtr = LoadBE32(buffer + 24);
    s->created = true;
    if (s->flags & kHost)
    {
        // Hosting: a new id of our kind, our address, a fresh key exchange key.
        const bool live = Kind(s->flags) != "system link" && Kind(s->flags) != "offline";
        s->id = (Random()() & 0x00FFFFFFFFFFFFFFull) | uint64_t(live ? kOnlinePeerId : kSystemLinkId) << 56;
        s->nonce = Random()();
        if (infoPtr)
        {
            for (int i = 0; i < 8; i++)
                *GuestPtr<uint8_t>(infoPtr + i) = uint8_t(s->id >> (56 - 8 * i));
            OnlineSelfXnAddr(infoPtr + 8);
            RandomBytes(infoPtr + 8 + 0x24, 0x10);
        }
        if (noncePtr)
            StoreBE64(noncePtr, s->nonce);
    }
    else if (infoPtr)
    {
        // Joining someone else's: the game filled in their session info.
        for (int i = 0; i < 8; i++)
            s->id = s->id << 8 | *GuestPtr<uint8_t>(infoPtr + i);
    }
    if (infoPtr)
        memcpy(s->info, GuestPtr(infoPtr), kSessionInfoSize);
    Log("live: session %016llX created (%s, %s, %u public and %u private slots, flags %X)", (unsigned long long)s->id,
        s->flags & kHost ? "host" : "joined", Kind(s->flags).c_str(), s->publicSlots, s->privateSlots, s->flags);
    return X_ERROR_SUCCESS;
}

// XGI_SESSION_MANAGE { obj, count, XUID*, user indices*, private slot flags* }
uint32_t Join(uint32_t buffer, bool leave)
{
    auto s = Lookup(LoadBE32(buffer));
    if (!s)
        return X_E_INVALIDARG;
    const uint32_t count = LoadBE32(buffer + 4), xuids = LoadBE32(buffer + 8), indices = LoadBE32(buffer + 12),
                   privates = LoadBE32(buffer + 16);
    for (uint32_t i = 0; i < count; i++)
    {
        // Local players come by user index, remote ones by XUID.
        const uint64_t xuid = xuids ? LoadBE64(xuids + i * 8) : OnlineLocalXuid(LoadBE32(indices + i * 4));
        if (leave)
            s->members.erase(xuid);
        else
            s->members[xuid] = privates && *GuestPtr<uint8_t>(privates + i);
    }
    Log("live: session %016llX: %u player(s) %s, %zu in it", (unsigned long long)s->id, count, leave ? "left" : "joined",
        s->members.size());
    return X_ERROR_SUCCESS;
}

// XGI_SESSION_DETAILS { obj, size, XSESSION_LOCAL_DETAILS* }
uint32_t Details(uint32_t buffer)
{
    auto s = Lookup(LoadBE32(buffer));
    if (!s)
        return X_E_INVALIDARG;
    const uint32_t size = LoadBE32(buffer + 4), out = LoadBE32(buffer + 8);
    const uint32_t members = uint32_t(s->members.size());
    if (!out || size < 0x80 + members * 0x10)
        return 122; // ERROR_INSUFFICIENT_BUFFER
    memset(GuestPtr(out), 0, 0x80);
    StoreBE32(out + 0x00, s->userIndex);
    StoreBE32(out + 0x0C, s->flags);
    StoreBE32(out + 0x10, s->publicSlots);
    StoreBE32(out + 0x14, s->privateSlots);
    const uint32_t used = members > s->publicSlots ? s->publicSlots : members;
    StoreBE32(out + 0x18, s->publicSlots - used);
    StoreBE32(out + 0x1C, s->privateSlots);
    StoreBE32(out + 0x20, members);
    StoreBE32(out + 0x24, members);
    StoreBE32(out + 0x28, s->started ? 2 : 0); // XSESSION_STATE: lobby, in game
    StoreBE64(out + 0x2C, s->nonce);
    memcpy(GuestPtr(out + 0x34), s->info, kSessionInfoSize);
    const uint32_t list = out + 0x80;
    StoreBE32(out + 0x7C, members ? list : 0);
    uint32_t i = 0;
    for (auto& [xuid, priv] : s->members)
    {
        // XSESSION_MEMBER { u64 XUID, u32 user index, u32 flags }
        StoreBE64(list + i * 0x10, xuid);
        StoreBE32(list + i * 0x10 + 8, xuid == OnlineLocalXuid(0) ? 0 : 0xFE);
        StoreBE32(list + i * 0x10 + 12, priv ? 1 : 0);
        i++;
    }
    return X_ERROR_SUCCESS;
}

// XGI_XUSER_READ_STATS { title, XUID count, XUIDs*, spec count, specs*, results size, results* }:
// no stats kept yet, so every view comes back with no rows.
uint32_t ReadStats(uint32_t buffer)
{
    const uint32_t specs = LoadBE32(buffer + 12), specPtr = LoadBE32(buffer + 16), size = LoadBE32(buffer + 20),
                   out = LoadBE32(buffer + 24);
    if (!out || !specPtr || size < 8 + specs * 0x10)
        return X_E_INVALIDARG;
    // X_USER_STATS_READ_RESULTS { views, view* }, then X_USER_STATS_VIEW { id, total rows, rows, row* } each;
    // a spec is { view id, column count, u16 columns[64] } (0x88 bytes).
    StoreBE32(out, specs);
    StoreBE32(out + 4, out + 8);
    for (uint32_t i = 0; i < specs; i++)
    {
        const uint32_t v = out + 8 + i * 0x10;
        StoreBE32(v, LoadBE32(specPtr + i * 0x88));
        StoreBE32(v + 4, 0);
        StoreBE32(v + 8, 0);
        StoreBE32(v + 12, 0);
    }
    return X_ERROR_SUCCESS;
}

// XSESSION_SEARCHRESULT_HEADER { count, results* }: nothing found (no server yet).
uint32_t SearchNothing(uint32_t header)
{
    if (header)
    {
        StoreBE32(header, 0);
        StoreBE32(header + 4, 0);
    }
    return X_ERROR_SUCCESS;
}
}

uint32_t OnlineSessionCreateHandle()
{
    auto s = std::make_shared<Session>();
    s->guest = GuestAlloc(Heap::Virtual, 16);
    const uint32_t handle = CreateHandle(s);
    StoreBE32(s->guest, handle);
    return handle;
}

uint32_t OnlineSessionObject(uint32_t handle)
{
    auto s = std::dynamic_pointer_cast<Session>(GetHandle(handle));
    return s ? s->guest : 0;
}

void OnlineSelfXnAddr(uint32_t xnaddr)
{
    // XNADDR { ina, inaOnline, u16 portOnline, u8 MAC[6], SGADDR[0x14] }: this PC on the loopback
    // network for now (the server will hand out real ones).
    memset(GuestPtr(xnaddr), 0, 0x24);
    StoreBE32(xnaddr + 0, 0x7F000001);
    StoreBE32(xnaddr + 4, 0x7F000001);
    *GuestPtr<uint16_t>(xnaddr + 8) = __builtin_bswap16(1000);
    static const uint64_t mac = Random()() | 0x020000000000ull; // a locally administered address
    for (int i = 0; i < 6; i++)
        *GuestPtr<uint8_t>(xnaddr + 10 + i) = uint8_t(mac >> (40 - 8 * i));
}

bool OnlineXgiMessage(uint32_t message, uint32_t buffer, uint32_t size, uint32_t& result)
{
    std::lock_guard lock(g_mutex);
    switch (message)
    {
    case 0x000B0006: // XUserSetContext
    case 0x000B0007: // XUserSetPropertyEx
    case 0x000B0008: // XUserWriteAchievements (none in FM2)
        result = X_ERROR_SUCCESS;
        return true;
    case 0x000B0010: // XSessionCreate
        result = Create(buffer);
        return true;
    case 0x000B0011: // XSessionDelete { obj, flags, u64 nonce }
        if (auto s = Lookup(LoadBE32(buffer)))
        {
            Log("live: session %016llX deleted", (unsigned long long)s->id);
            s->created = s->started = false;
            s->members.clear();
            result = X_ERROR_SUCCESS;
        }
        else
            result = X_E_INVALIDARG;
        return true;
    case 0x000B0012: // XSessionJoin (local and remote)
    case 0x000B0013: // XSessionLeave
        result = Join(buffer, message == 0x000B0013);
        return true;
    case 0x000B0014: // XSessionStart
    case 0x000B0015: // XSessionEnd
        if (auto s = Lookup(LoadBE32(buffer)))
        {
            s->started = message == 0x000B0014;
            Log("live: session %016llX %s", (unsigned long long)s->id, s->started ? "started" : "ended");
            result = X_ERROR_SUCCESS;
        }
        else
            result = X_E_INVALIDARG;
        return true;
    case 0x000B0018: // XSessionModify { obj, flags, public, private }
        if (auto s = Lookup(LoadBE32(buffer)))
        {
            s->flags = (s->flags & ~0xF80u) | (LoadBE32(buffer + 4) & 0xF80u);
            s->publicSlots = LoadBE32(buffer + 8);
            s->privateSlots = LoadBE32(buffer + 12);
            result = X_ERROR_SUCCESS;
        }
        else
            result = X_E_INVALIDARG;
        return true;
    case 0x000B001D: // XSessionGetDetails
        result = Details(buffer);
        return true;
    case 0x000B0016: // XSessionSearch { ..., results size, header* at +0x1C }
        result = SearchNothing(LoadBE32(buffer + 0x1C));
        return true;
    case 0x000B001C: // XSessionSearchEx
        result = SearchNothing(LoadBE32(buffer + 0x1C));
        return true;
    case 0x000B001B: // XSessionSearchByID { user, XNKID, size, header* at +0x10 }
        result = SearchNothing(LoadBE32(buffer + 0x10));
        return true;
    case 0x000B0060: // XSessionSearchByIds { user, count, ids*, size, header* at +0x10 }
        result = SearchNothing(LoadBE32(buffer + 0x10));
        return true;
    case 0x000B001F: // XSessionModifySkill
    case 0x000B0025: // XSessionWriteStats
    case 0x000B0026: // XSessionFlushStats
    case 0x000B0020: // XUserResetStatsView
        result = X_ERROR_SUCCESS;
        return true;
    case 0x000B0021: // XUserReadStats
        result = ReadStats(buffer);
        return true;
    default:
        return false;
    }
}
