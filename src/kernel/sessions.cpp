// Xbox Live sessions (FM2-online-scope.md, step 2), under FM2_LIVE: the session objects the game
// creates (XamSessionCreateHandle, XamSessionRefObjByHandle) and the XGI app's session messages
// (XMsg app 0xFB). Creating one as host fills in its session info with our address and a fresh
// key; joins, starts, ends and deletes are kept. Message ids and layouts follow the Xenia netplay
// fork (github.com/AdrianCassar/xenia-canary, netplay_canary_experimental: xgi_app.cc,
// xsession.cc, xnet.h; BSD), as the Forza Horizon recomp does.
//
// Matchmaking on the FM2 Live server (kernel/net.cpp), after RecompiledHaloReach's
// live_xmsg.cpp (MIT): a session we host with the matchmaking flag is published with its slots,
// our properties and contexts (what the game set with XUserSetContext / XUserSetPropertyEx: game
// type, track, class...) and its QoS data (XNetQosListen); XSessionSearch asks the server for
// the published sessions and fills in the results with theirs; XNetQosLookup answers from them.
#include "kernel/sessions.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <random>
#include <string>

#include "kernel/imports.h"
#include "kernel/net.h"
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

// --- Matchmaking state ----------------------------------------------------------------------

// Everything is published under one search key for now (FM2's title id): a search finds every
// session on the server, and the game filters by the properties it reads back.
constexpr uint32_t kSearchKey = 0x4D5307EA;
constexpr uint32_t kResultSize = 0x5C, kPropertySize = 0x18;

struct UserValue
{
    uint8_t type = 0;
    uint8_t value[8] = {};
};
std::map<uint32_t, UserValue> g_properties;                   // ours, by property id
std::map<uint32_t, uint32_t> g_contexts;                      // ours, by context id
std::unordered_map<uint64_t, std::vector<uint8_t>> g_qosData; // XNetQosListen data by session id
std::weak_ptr<Session> g_published;                           // the session we publish

// A session a search found.
struct FoundSession
{
    uint64_t xuid = 0;
    uint8_t info[kSessionInfoSize] = {};
    uint8_t slots[4] = {}; // max public, max private, filled public, filled private
    std::vector<std::pair<uint32_t, UserValue>> properties;
    std::vector<std::pair<uint32_t, uint32_t>> contexts;
    std::vector<uint8_t> qos;
};
std::mutex g_matchMutex;
std::condition_variable g_matchAnswered;
uint32_t g_matchRequest = 0, g_matchAnswer = 0;
std::vector<FoundSession> g_matchResults;                      // of the last answer
std::unordered_map<uint64_t, std::vector<uint8_t>> g_foundQos; // by session id

uint64_t SessionId(const uint8_t* info)
{
    uint64_t id = 0;
    for (int i = 0; i < 8; i++)
        id = id << 8 | info[i];
    return id;
}

void Put32(std::vector<uint8_t>& b, uint32_t v)
{
    const uint8_t x[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
    b.insert(b.end(), x, x + 4);
}

// The session behind the X_KSESSION pointer a message carries.
std::shared_ptr<Session> Lookup(uint32_t obj)
{
    if (!obj)
        return nullptr;
    return std::dynamic_pointer_cast<Session>(GetHandle(LoadBE32(obj)));
}

// Publishes the session we host for matchmaking, or withdraws it. Needs g_mutex.
void PublishLocked()
{
    auto s = g_published.lock();
    if (!s || !s->created)
    {
        NetSendMatch(false, { 0 });
        return;
    }
    std::vector<uint8_t> body = { 1 };
    Put32(body, kSearchKey);
    body.insert(body.end(), s->info, s->info + kSessionInfoSize);
    uint32_t filledPrivate = 0;
    for (auto& [xuid, priv] : s->members)
        filledPrivate += priv;
    const uint32_t filledPublic = uint32_t(s->members.size()) - filledPrivate;
    for (uint32_t n : { s->publicSlots, s->privateSlots, filledPublic, filledPrivate })
        body.push_back(uint8_t(std::min<uint32_t>(n, 255)));
    body.push_back(uint8_t(std::min<size_t>(g_properties.size(), 32)));
    size_t count = 0;
    for (auto& [id, v] : g_properties)
    {
        if (count++ == 32)
            break;
        Put32(body, id);
        body.push_back(v.type);
        body.insert(body.end(), v.value, v.value + 8);
    }
    body.push_back(uint8_t(std::min<size_t>(g_contexts.size(), 32)));
    count = 0;
    for (auto& [id, value] : g_contexts)
    {
        if (count++ == 32)
            break;
        Put32(body, id);
        Put32(body, value);
    }
    auto q = g_qosData.find(s->id);
    const size_t qos = q == g_qosData.end() ? 0 : std::min<size_t>(q->second.size(), 1000);
    body.push_back(uint8_t(qos >> 8));
    body.push_back(uint8_t(qos));
    if (qos)
        body.insert(body.end(), q->second.begin(), q->second.begin() + qos);
    NetSendMatch(false, body);
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
    if ((s->flags & kHost) && (s->flags & kMatchmaking))
    {
        g_published = s;
        PublishLocked();
    }
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
    if (g_published.lock() == s)
        PublishLocked();
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

// XSessionSearch(Ex) { procedure, user, max results, property count u16, context count u16,
// properties*, contexts*, results size, results* }: the server's published sessions, each
// XSESSION_SEARCHRESULT followed (in the buffer's tail) by its properties and contexts.
// Called without g_mutex (it waits for the server).
uint32_t Search(uint32_t buffer)
{
    const uint32_t maxResults = LoadBE32(buffer + 8), resultsSize = LoadBE32(buffer + 0x18), results = LoadBE32(buffer + 0x1C);
    if (!results || resultsSize < 8)
        return X_E_INVALIDARG;
    std::vector<FoundSession> found;
    {
        std::unique_lock lock(g_matchMutex);
        const uint32_t request = ++g_matchRequest;
        std::vector<uint8_t> body;
        Put32(body, kSearchKey);
        Put32(body, request);
        NetSendMatch(true, body);
        if (g_matchAnswered.wait_for(lock, std::chrono::milliseconds(800), [&] { return g_matchAnswer == request; }))
            found = g_matchResults;
    }
    uint64_t own = 0;
    {
        std::lock_guard lock(g_mutex);
        if (auto s = g_published.lock())
            own = s->id;
    }
    std::erase_if(found, [&](const FoundSession& f) { return SessionId(f.info) == own; });
    memset(GuestPtr(results), 0, resultsSize);
    const uint32_t count = std::min<uint32_t>({ uint32_t(found.size()), maxResults, (resultsSize - 8) / kResultSize });
    StoreBE32(results, count);
    StoreBE32(results + 4, results + 8);
    uint32_t extra = results + 8 + count * kResultSize; // properties and contexts go here
    const uint32_t end = results + resultsSize;
    for (uint32_t i = 0; i < count; i++)
    {
        const FoundSession& f = found[i];
        const uint32_t r = results + 8 + i * kResultSize;
        memcpy(GuestPtr(r), f.info, kSessionInfoSize);
        StoreBE32(r + 0x3C, f.slots[0] > f.slots[2] ? f.slots[0] - f.slots[2] : 0);
        StoreBE32(r + 0x40, f.slots[1] > f.slots[3] ? f.slots[1] - f.slots[3] : 0);
        StoreBE32(r + 0x44, f.slots[2]);
        StoreBE32(r + 0x48, f.slots[3]);
        const uint32_t propsSize = uint32_t(f.properties.size()) * kPropertySize, contextsSize = uint32_t(f.contexts.size()) * 8;
        if (extra + propsSize + contextsSize > end)
            continue;
        // XUSER_PROPERTY { id, pad, XUSER_DATA { type u8, pad, value at +8 } }, XUSER_CONTEXT { id, value }
        StoreBE32(r + 0x4C, uint32_t(f.properties.size()));
        StoreBE32(r + 0x50, uint32_t(f.contexts.size()));
        StoreBE32(r + 0x54, extra);
        for (auto& [id, v] : f.properties)
        {
            StoreBE32(extra, id);
            *GuestPtr<uint8_t>(extra + 8) = v.type;
            memcpy(GuestPtr(extra + 0x10), v.value, 8);
            extra += kPropertySize;
        }
        StoreBE32(r + 0x58, extra);
        for (auto& [id, value] : f.contexts)
        {
            StoreBE32(extra, id);
            StoreBE32(extra + 4, value);
            extra += 8;
        }
    }
    Log("live: session search: %u found", count);
    return X_ERROR_SUCCESS;
}

// XSESSION_SEARCHRESULT_HEADER { count, results* }: nothing found.
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
    NetSelfXnAddr(xnaddr);
}

bool OnlineXgiMessage(uint32_t message, uint32_t buffer, uint32_t size, uint32_t& result)
{
    if (message == 0x000B0016 || message == 0x000B001C) // XSessionSearch(Ex): waits for the server
    {
        result = Search(buffer);
        return true;
    }
    std::lock_guard lock(g_mutex);
    switch (message)
    {
    case 0x000B0006: // XUserSetContext { user, unused, XUID, context id, value }
        g_contexts[LoadBE32(buffer + 16)] = LoadBE32(buffer + 20);
        result = X_ERROR_SUCCESS;
        return true;
    case 0x000B0007: // XUserSetPropertyEx { user, unused, XUID, property id, size, value* }
    {
        // The type is the id's top nibble; strings and binary ones are not kept.
        const uint32_t id = LoadBE32(buffer + 16), size = LoadBE32(buffer + 20), value = LoadBE32(buffer + 24);
        const uint8_t type = uint8_t(id >> 28);
        if (value && type != 4 && type != 6 && size <= 8)
        {
            UserValue v;
            v.type = type;
            memcpy(v.value, GuestPtr(value), size);
            g_properties[id] = v;
        }
        result = X_ERROR_SUCCESS;
        return true;
    }
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
            if (g_published.lock() == s)
            {
                PublishLocked();
                g_published.reset();
            }
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
            if (g_published.lock() == s)
                PublishLocked();
            result = X_ERROR_SUCCESS;
        }
        else
            result = X_E_INVALIDARG;
        return true;
    case 0x000B001D: // XSessionGetDetails
        result = Details(buffer);
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

// The server's answer to a search: request u32, count u16, then per session the host's id u32,
// XUID u64 and record (u16 length + bytes): XSESSION_INFO, slots (4 bytes), properties (u8 count,
// then id u32, type u8, value 8 bytes), contexts (u8 count, then id u32, value u32), QoS data
// (u16 length + bytes).
void SessionsMatchResults(const uint8_t* p, size_t n)
{
    if (n < 6)
        return;
    auto u32 = [](const uint8_t* q) { return uint32_t(q[0]) << 24 | q[1] << 16 | q[2] << 8 | q[3]; };
    std::vector<FoundSession> found;
    size_t pos = 6;
    for (uint32_t i = 0, count = uint32_t(p[4]) << 8 | p[5]; i < count; i++)
    {
        if (pos + 14 > n)
            break;
        const size_t size = size_t(p[pos + 12]) << 8 | p[pos + 13];
        if (pos + 14 + size > n)
            break;
        FoundSession f;
        f.xuid = uint64_t(u32(p + pos + 4)) << 32 | u32(p + pos + 8);
        const uint8_t* rec = p + pos + 14;
        size_t at = kSessionInfoSize + 4;
        bool ok = size >= at + 1;
        if (ok)
        {
            memcpy(f.info, rec, kSessionInfoSize);
            memcpy(f.slots, rec + kSessionInfoSize, 4);
            for (size_t k = 0, c = rec[at++]; ok && k < c; k++, at += 13)
            {
                if (at + 13 > size)
                {
                    ok = false;
                    break;
                }
                UserValue v;
                v.type = rec[at + 4];
                memcpy(v.value, rec + at + 5, 8);
                f.properties.emplace_back(u32(rec + at), v);
            }
            if (ok && at + 1 <= size)
                for (size_t k = 0, c = rec[at++]; k < c; k++, at += 8)
                {
                    if (at + 8 > size)
                    {
                        ok = false;
                        break;
                    }
                    f.contexts.emplace_back(u32(rec + at), u32(rec + at + 4));
                }
            if (ok && at + 2 <= size)
            {
                const size_t qos = size_t(rec[at]) << 8 | rec[at + 1];
                if (at + 2 + qos <= size)
                    f.qos.assign(rec + at + 2, rec + at + 2 + qos);
            }
        }
        if (ok)
            found.push_back(std::move(f));
        pos += 14 + size;
    }
    std::lock_guard lock(g_matchMutex);
    for (const auto& f : found)
        g_foundQos[SessionId(f.info)] = f.qos;
    g_matchResults = std::move(found);
    g_matchAnswer = u32(p);
    g_matchAnswered.notify_all();
}

// --- QoS ------------------------------------------------------------------------------------

namespace
{
// Signals a guest event (as KeSetEvent does).
void SignalEvent(uint32_t handle)
{
    auto object = GetHandle(handle);
    if (!object || object->kind != KernelObject::Kind::Event)
        return;
    {
        std::lock_guard lock(g_objectMutex);
        auto& e = static_cast<EventObject&>(*object);
        e.signaled = true;
        SyncGuestSignalState(e);
    }
    SignalObjectsChanged();
}
}

// (caller, XNKID*, data*, size, bits per second, flags): a host publishes its game's
// description for players probing it; kept per session and sent with the published session.
GUEST_EXPORT(NetDll_XNetQosListen)
{
    constexpr uint32_t kSetData = 0x4, kDisable = 0x2, kRelease = 0x10;
    const uint32_t idPtr = ctx.r4.u32, data = ctx.r5.u32, size = ctx.r6.u32, flags = ctx.r8.u32;
    if (LiveTrace() && idPtr)
    {
        const uint64_t id = SessionId(GuestPtr<uint8_t>(idPtr));
        std::lock_guard lock(g_mutex);
        if ((flags & kSetData) && data)
            g_qosData[id].assign(GuestPtr<uint8_t>(data), GuestPtr<uint8_t>(data) + std::min<uint32_t>(size, 1000));
        if (flags & (kDisable | kRelease))
            g_qosData.erase(id);
        if (auto s = g_published.lock(); s && s->id == id)
            PublishLocked();
    }
    Return(ctx, 0);
}

// (caller, count, XNADDR*[], XNKID*[], XNKEY*[], server count, IN_ADDR[], service ids[], probes,
// bits per second, flags, event, XNQOS**): probing hosts before joining, answered at once from
// the sessions the last search found: their QoS data, a nominal round trip and bandwidth.
GUEST_EXPORT(NetDll_XNetQosLookup)
{
    const uint32_t count = std::min<uint32_t>(ctx.r4.u32, 64), xnkids = ctx.r6.u32;
    const uint32_t probes = StackArg(ctx, 8), event = StackArg(ctx, 11), out = StackArg(ctx, 12);
    std::vector<std::vector<uint8_t>> data(count);
    std::vector<bool> known(count, false);
    {
        std::lock_guard lock(g_matchMutex);
        for (uint32_t i = 0; i < count && xnkids; i++)
            if (const uint32_t kid = LoadBE32(xnkids + 4 * i))
                if (auto it = g_foundQos.find(SessionId(GuestPtr<uint8_t>(kid))); it != g_foundQos.end())
                {
                    known[i] = true;
                    data[i] = it->second;
                }
    }
    // XNQOS { count, pending, XNQOSINFO[count] }, each XNQOSINFO { flags u8, reserved u8, probes
    // sent u16, probes received u16, data size u16, data*, RTT min u16, RTT median u16, up and down
    // bits per second } (0x18 bytes); the data after them.
    constexpr uint32_t kInfoSize = 0x18;
    uint32_t total = 8 + kInfoSize * std::max<uint32_t>(count, 1);
    for (const auto& d : data)
        total += uint32_t(d.size());
    const uint32_t qos = GuestAlloc(Heap::Virtual, total);
    memset(GuestPtr(qos), 0, total);
    StoreBE32(qos, count);
    uint32_t dataPtr = qos + 8 + kInfoSize * std::max<uint32_t>(count, 1);
    const uint16_t sent = uint16_t(std::max<uint32_t>(probes, 1));
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t info = qos + 8 + kInfoSize * i;
        // flags: complete, target contacted, data received
        *GuestPtr<uint8_t>(info) = known[i] ? uint8_t(0x01 | 0x02 | (data[i].empty() ? 0 : 0x08)) : 0x01;
        *GuestPtr<uint16_t>(info + 2) = __builtin_bswap16(sent);
        if (!known[i])
            continue;
        *GuestPtr<uint16_t>(info + 4) = __builtin_bswap16(sent);
        *GuestPtr<uint16_t>(info + 6) = __builtin_bswap16(uint16_t(data[i].size()));
        if (!data[i].empty())
        {
            StoreBE32(info + 8, dataPtr);
            memcpy(GuestPtr(dataPtr), data[i].data(), data[i].size());
            dataPtr += uint32_t(data[i].size());
        }
        *GuestPtr<uint16_t>(info + 0xC) = __builtin_bswap16(20); // ms
        *GuestPtr<uint16_t>(info + 0xE) = __builtin_bswap16(25);
        StoreBE32(info + 0x10, 2000000);
        StoreBE32(info + 0x14, 2000000);
    }
    StoreIfNonNull(out, qos);
    if (event)
        SignalEvent(event);
    Log("live: QoS lookup of %u host(s)", count);
    Return(ctx, 0);
}

// (caller, XNQOS*)
GUEST_EXPORT(NetDll_XNetQosRelease)
{
    if (ctx.r4.u32)
        GuestFree(ctx.r4.u32);
    Return(ctx, 0);
}
