// The game's network under FM2_LIVE (FM2-online-scope.md): its UDP sockets on a virtual network.
//
// From RecompiledHaloReach's src/kernel/net.cpp (github.com/Dyotson/RecompiledHaloReach, MIT
// License, Copyright (c) 2026 Dyotson and RecompiledHaloReach contributors), on this runtime:
//
// - The game's UDP sockets are ours, not the host's: this PC owns one host UDP port (the first
//   free one of 21000-21007), and every datagram carries the guest source and destination ports.
// - Every other player gets a virtual IP 0.77.x.y (a 360 secure address: the game only connects
//   to addresses whose first byte is 0) that sendto and recvfrom translate to the player's path.
// - FM2_SERVER=host[:port] (default port 21100) connects to an FM2 Live server
//   (tools/online/fm2_live_server.py). Everyone on it is on one network: broadcasts go to all
//   through the server, and datagrams for one player go directly once UDP hole punching opened a
//   path, through the server until then. Our XNADDR then carries the id the server gave us
//   (0xF0000000 | id). FM2_SERVER_RELAY=1 never punches (to test the relay).
// - Without a server, copies of the game on this PC find each other on the loopback network.
// - FM2_NETTRACE=1 logs every datagram's first bytes.
// Without FM2_LIVE the network stays down, as before.
#include "kernel/net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

#include "kernel/imports.h"
#include "kernel/sessions.h"
#include "kernel/websocket.h"
#include "log.h"
#include "settings.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
using Clock = std::chrono::steady_clock;

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t LoadBE16(uint32_t address) { return Load16(GuestPtr<uint8_t>(address)); }
void Store16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void Store32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// Winsock errors as the game sees them (WSAGetLastError).
constexpr uint32_t kWsaEWouldBlock = 10035, kWsaENotSock = 10038, kWsaEMsgSize = 10040, kWsaEAddrInUse = 10048,
                   kWsaENetUnreach = 10051, kWsaEInval = 10022, kWsaENetDown = 10050;
constexpr uint32_t kWsaOperationAborted = 995, kWsaIoIncomplete = 996, kWsaIoPending = 997;
// An OVERLAPPED's status word.
constexpr uint32_t kStatusPending = 0x00000103, kStatusBufferOverflow = 0x80000005, kStatusCancelled = 0xC0000120;
// WSAEventSelect events.
constexpr uint32_t kFdRead = 0x01, kFdWrite = 0x02;
thread_local uint32_t t_lastError = kWsaENetDown;
void SetError(uint32_t e) { t_lastError = e; }

constexpr uint32_t kHandleBase = 0x4E450000; // our guest SOCKET handles: 'NE' + index
constexpr uint32_t kMagic = 0x464E4554;      // 'FNET', first word of a loopback datagram
constexpr size_t kHeaderSize = 8;            // magic, guest source port, guest destination port
constexpr uint16_t kHostPortFirst = 21000, kHostPortCount = 8;
constexpr uint32_t kSelfVip = 0x004D0001; // 0.77.0.1
constexpr uint32_t kVipNet = 0x004D0000;  // 0.77.0.0/16
constexpr uint32_t kBroadcast = 0xFFFFFFFF;

// FM2 Live server protocol (tools/online/fm2_live_server.py): every packet starts with 'RLV1'
// and a type byte.
constexpr uint32_t kLiveMagic = 0x524C5631;
constexpr uint32_t kStunCookie = 0x2112A442;

// A message from FM2 Live for the toast at the bottom of the screen (gpu/renderer.cpp, through LiveNotice)
std::mutex g_noticeMutex;
std::wstring g_noticeTitle, g_noticeLine;
std::chrono::steady_clock::time_point g_noticeSince{};

void ShowLiveNotice(const std::wstring& title, const std::wstring& line)
{
    std::lock_guard lock(g_noticeMutex);
    g_noticeTitle = title, g_noticeLine = line;
    g_noticeSince = std::chrono::steady_clock::now();
}
constexpr uint16_t kLiveVersion = 1, kLiveDefaultPort = 21100;
enum LiveType : uint8_t
{
    kHello = 1, kWelcome, kError, kBroadcastMsg, kRelay, kForward, kPunch, kPunchAck, kData, kBye, kList, kPeers,
    kPresence, kInvite, kMatchPublish, kMatchSearch, kMatchResults,
};
// XNADDR.ina of a player on the server: 0xF0000000 | its id (240.0.0.0/8 is reserved).
constexpr uint32_t kLiveInaTag = 0xF0000000;

bool Env(const char* name)
{
    const char* v = getenv(name);
    return v && *v && *v != '0';
}

bool PacketTrace()
{
    static const bool on = Env("FM2_NETTRACE");
    return on;
}

struct Datagram
{
    uint32_t vip;
    uint16_t port;
    std::vector<uint8_t> data;
};

// An overlapped receive (WSARecvFrom/WSARecv) waiting for a datagram: FM2 keeps one posted on
// its game socket, waits for the OVERLAPPED's event and collects the datagram with
// WSAGetOverlappedResult (from the Forza Horizon recomp's net.cpp, MIT).
struct PendingRecv
{
    // The WSABUF entries (length, buffer) as they were when the receive was posted: like Windows,
    // the descriptors are copied at the call (the game keeps its WSABUF array on the stack, so read
    // at completion it named whatever the stack held by then, and a datagram from the server was
    // copied over live objects: the crash leaving Arcade under FM2_LIVE). The buffers stay the game's.
    std::vector<std::pair<uint32_t, uint32_t>> buffers;
    uint32_t from = 0, fromLen = 0;  // guest sockaddr_in*, int* (0 for WSARecv)
    uint32_t overlapped = 0;         // WSAOVERLAPPED { status, bytes, offset, offset high, event }
};

struct GuestSocket
{
    uint16_t port = 0; // bound guest port, 0 while unbound
    bool nonblocking = false;
    uint32_t connectedVip = 0;
    uint16_t connectedPort = 0;
    std::deque<Datagram> queue;
    std::deque<PendingRecv> pending;
    uint32_t selectEvent = 0, selectMask = 0; // WSAEventSelect
};

// Signals a guest event (as KeSetEvent does).
void SignalEvent(uint32_t handle)
{
    auto object = handle ? GetHandle(handle) : nullptr;
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

// Copies a datagram into guest WSABUFs; returns the bytes copied.
uint32_t Scatter(const std::vector<std::pair<uint32_t, uint32_t>>& buffers, const std::vector<uint8_t>& data)
{
    size_t done = 0;
    for (const auto& [length, buffer] : buffers)
    {
        if (done >= data.size())
            break;
        const size_t n = std::min<size_t>(length, data.size() - done);
        if (n)
            memcpy(GuestPtr(buffer), data.data() + done, n);
        done += n;
    }
    return uint32_t(done);
}

// The guest WSABUF array (u32 length, u32 buffer per entry), read now.
std::vector<std::pair<uint32_t, uint32_t>> ReadBuffers(uint32_t buffers, uint32_t count)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (uint32_t i = 0; i < count && i < 64; i++)
        out.emplace_back(LoadBE32(buffers + 8 * i), LoadBE32(buffers + 8 * i + 4));
    return out;
}

// Joins guest WSABUFs into one datagram.
std::vector<uint8_t> Gather(uint32_t buffers, uint32_t count)
{
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* p = GuestPtr<uint8_t>(LoadBE32(buffers + 8 * i + 4));
        data.insert(data.end(), p, p + LoadBE32(buffers + 8 * i));
    }
    return data;
}

// Guest sockaddr_in: u16 family, u16 port, u32 address, 8 bytes of zero.
void WriteSockaddr(uint32_t a, uint32_t ip, uint16_t port)
{
    uint8_t* p = GuestPtr<uint8_t>(a);
    memset(p, 0, 16);
    Store16(p, 2); // AF_INET
    Store16(p + 2, port);
    Store32(p + 4, ip);
}

struct Endpoint
{
    uint32_t ip = 0; // host order
    uint16_t port = 0;
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
};

// Where packets "from the server" come from when it's a Worker reached over a WebSocket: no real address
const Endpoint kWsServer{ 0xFFFFFFFEu, 1 };

// Another player on the server.
struct LivePeer
{
    uint32_t vip = 0;
    Endpoint publicEp, localEp; // as the server saw it / as it reported its LAN side
    Endpoint direct;            // the path that answered a punch; ip 0: through the server
    Clock::time_point punchUntil{}, nextPunch{}, punchStarted{};
};

std::string IpString(uint32_t ip)
{
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." + std::to_string((ip >> 8) & 0xFF) + "." +
        std::to_string(ip & 0xFF);
}

void LogPacket(const char* dir, uint32_t vip, uint16_t sourcePort, uint16_t port, const uint8_t* data, size_t size)
{
    static std::atomic<uint64_t> count{ 0 };
    const uint64_t n = count++;
    if (!PacketTrace() || (n >= 2000 && n % 500 != 0))
        return;
    char hex[3 * 48 + 1] = {};
    for (size_t i = 0; i < std::min<size_t>(size, 48); i++)
        snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    Log("net: %s %zu bytes %u -> %u (0.77.%u.%u) %s", dir, size, sourcePort, port, (vip >> 8) & 0xFF, vip & 0xFF, hex);
}

// XNADDR { ina, inaOnline, u16 portOnline, u8 MAC[6], SGADDR[0x14] }.
void WriteLanXnAddr(uint8_t* p, uint32_t ip, uint16_t port)
{
    memset(p, 0, 36);
    Store32(p, ip);
    Store16(p + 8, port);
    const uint8_t mac[6] = { 0x00, 0x22, 0x48, uint8_t(ip), uint8_t(port >> 8), uint8_t(port) };
    memcpy(p + 10, mac, 6);
}

void WriteLiveXnAddr(uint8_t* p, uint32_t id, uint16_t epoch, uint32_t publicIp)
{
    memset(p, 0, 36);
    Store32(p, kLiveInaTag | id);
    Store32(p + 4, publicIp);
    Store16(p + 8, epoch);
    const uint8_t mac[6] = { 0x02, uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id), uint8_t(epoch >> 8), uint8_t(epoch) };
    memcpy(p + 10, mac, 6);
}

class VNet
{
public:
    static VNet& Get()
    {
        static VNet net;
        return net;
    }

    bool Ready() const { return fd_ != INVALID_SOCKET; }

    uint32_t CreateSocket()
    {
        std::lock_guard lock(mutex_);
        const uint32_t handle = kHandleBase + nextHandle_++;
        sockets_[handle] = GuestSocket();
        return handle;
    }

    bool IsOurs(uint32_t handle)
    {
        std::lock_guard lock(mutex_);
        return sockets_.count(handle) != 0;
    }

    // Closing a socket aborts its overlapped receives, as on Windows.
    void Close(uint32_t handle)
    {
        std::vector<uint32_t> events;
        {
            std::lock_guard lock(mutex_);
            if (auto it = sockets_.find(handle); it != sockets_.end())
            {
                events = CancelRecvsLocked(it->second);
                sockets_.erase(it);
            }
            cv_.notify_all();
        }
        for (uint32_t e : events)
            SignalEvent(e);
    }

    // XNetCleanup / WSACleanup: every socket goes, with its pending receives dropped without
    // touching guest memory: the game frees their buffers and OVERLAPPEDs after a cleanup (leaving
    // Arcade, for one), and completing them later wrote into whatever had taken that memory.
    void CloseAll()
    {
        std::lock_guard lock(mutex_);
        if (!sockets_.empty())
            Log("net: network cleanup: %zu socket(s) closed", sockets_.size());
        sockets_.clear();
        cv_.notify_all();
    }

    // WSARecvFrom/WSARecv with an OVERLAPPED: a queued datagram completes it now (0, or
    // kWsaEMsgSize when cut short; `bytes` set); otherwise it stays pending (kWsaIoPending) until
    // Deliver completes it.
    uint32_t RecvOverlapped(uint32_t handle, const PendingRecv& r, uint32_t& bytes)
    {
        uint32_t event = 0, result = kWsaIoPending;
        {
            std::lock_guard lock(mutex_);
            auto it = sockets_.find(handle);
            if (it == sockets_.end())
                return kWsaENotSock;
            GuestSocket& s = it->second;
            if (s.pending.empty() && !s.queue.empty())
            {
                Datagram d = std::move(s.queue.front());
                s.queue.pop_front();
                result = CompleteRecvLocked(r, d, bytes, event) ? kWsaEMsgSize : 0;
            }
            else
            {
                StoreBE32(r.overlapped + 4, 0);
                StoreBE32(r.overlapped, kStatusPending);
                s.pending.push_back(r);
            }
        }
        SignalEvent(event);
        return result;
    }

    // WSACancelOverlappedIO.
    void CancelRecvs(uint32_t handle)
    {
        std::vector<uint32_t> events;
        {
            std::lock_guard lock(mutex_);
            if (auto it = sockets_.find(handle); it != sockets_.end())
                events = CancelRecvsLocked(it->second);
        }
        for (uint32_t e : events)
            SignalEvent(e);
    }

    // Waits up to 100 ms for an OVERLAPPED to leave the pending state.
    void WaitOverlapped(uint32_t overlapped)
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return LoadBE32(overlapped) != kStatusPending; });
    }

    // WSAEventSelect: `event` is set when a datagram arrives (FD_READ) and right away for
    // FD_WRITE (a datagram socket can always send). The socket becomes nonblocking.
    uint32_t EventSelect(uint32_t handle, uint32_t event, uint32_t mask)
    {
        bool signal;
        {
            std::lock_guard lock(mutex_);
            auto it = sockets_.find(handle);
            if (it == sockets_.end())
                return kWsaENotSock;
            GuestSocket& s = it->second;
            s.selectEvent = event;
            s.selectMask = event ? mask : 0;
            if (s.selectMask)
                s.nonblocking = true;
            signal = (s.selectMask & kFdWrite) || ((s.selectMask & kFdRead) && !s.queue.empty());
        }
        if (signal)
            SignalEvent(event);
        return 0;
    }

    uint32_t Bind(uint32_t handle, uint16_t port)
    {
        std::lock_guard lock(mutex_);
        auto it = sockets_.find(handle);
        if (it == sockets_.end())
            return kWsaENotSock;
        if (port == 0)
            port = nextEphemeral_++;
        for (auto& [h, s] : sockets_)
            if (h != handle && s.port == port)
                return kWsaEAddrInUse;
        it->second.port = port;
        return 0;
    }

    uint32_t SetNonblocking(uint32_t handle, bool nonblocking)
    {
        std::lock_guard lock(mutex_);
        auto it = sockets_.find(handle);
        if (it == sockets_.end())
            return kWsaENotSock;
        it->second.nonblocking = nonblocking;
        return 0;
    }

    void Connect(uint32_t handle, uint32_t vip, uint16_t port)
    {
        std::lock_guard lock(mutex_);
        if (auto it = sockets_.find(handle); it != sockets_.end())
            it->second.connectedVip = vip, it->second.connectedPort = port;
    }

    bool Connected(uint32_t handle, uint32_t& vip, uint16_t& port)
    {
        std::lock_guard lock(mutex_);
        auto it = sockets_.find(handle);
        if (it == sockets_.end() || !it->second.connectedVip)
            return false;
        vip = it->second.connectedVip;
        port = it->second.connectedPort;
        return true;
    }

    uint16_t LocalPort(uint32_t handle)
    {
        std::lock_guard lock(mutex_);
        auto it = sockets_.find(handle);
        return it == sockets_.end() ? 0 : it->second.port;
    }

    uint32_t PendingBytes(uint32_t handle)
    {
        std::lock_guard lock(mutex_);
        auto it = sockets_.find(handle);
        if (it == sockets_.end() || it->second.queue.empty())
            return 0;
        return uint32_t(it->second.queue.front().data.size());
    }

    // Sends one guest datagram; returns 0 or a Winsock error.
    uint32_t SendTo(uint32_t handle, uint32_t vip, uint16_t port, const uint8_t* data, size_t size)
    {
        uint16_t sourcePort;
        {
            std::lock_guard lock(mutex_);
            auto it = sockets_.find(handle);
            if (it == sockets_.end())
                return kWsaENotSock;
            if (!it->second.port)
                it->second.port = nextEphemeral_++;
            sourcePort = it->second.port;
        }
        LogPacket("send", vip, sourcePort, port, data, size);
        if (vip == kSelfVip)
        {
            Deliver(kSelfVip, sourcePort, port, data, size);
            return 0;
        }
        if (vip == kBroadcast || (vip & 0xFF) == 0xFF)
        {
            if (!live_)
            {
                const std::vector<uint8_t> packet = LanPacket(sourcePort, port, data, size);
                for (uint16_t p = kHostPortFirst; p < kHostPortFirst + kHostPortCount; p++)
                    if (p != hostPort_)
                        SendHost({ INADDR_LOOPBACK, p }, packet);
            }
            if (live_ && liveId_)
            {
                std::vector<uint8_t> body(4 + size);
                Store16(body.data(), sourcePort);
                Store16(body.data() + 2, port);
                memcpy(body.data() + 4, data, size);
                SendLive(server_, kBroadcastMsg, body);
            }
            return 0;
        }
        if (live_)
        {
            uint32_t id = 0;
            Endpoint direct;
            {
                std::lock_guard lock(mutex_);
                if (auto it = liveIdByVip_.find(vip); it != liveIdByVip_.end())
                {
                    id = it->second;
                    direct = livePeers_[id].direct;
                    if (!direct.ip)
                        StartPunchLocked(id);
                }
            }
            if (id)
            {
                std::vector<uint8_t> body(8 + size);
                Store32(body.data(), direct.ip && !relayOnly_ ? liveId_ : id);
                Store16(body.data() + 4, sourcePort);
                Store16(body.data() + 6, port);
                memcpy(body.data() + 8, data, size);
                if (direct.ip && !relayOnly_)
                    SendLive(direct, kData, body);
                else
                    SendLive(server_, kRelay, body);
                return 0;
            }
        }
        Endpoint endpoint;
        if (!LanEndpointOf(vip, endpoint))
            return kWsaENetUnreach;
        SendHost(endpoint, LanPacket(sourcePort, port, data, size));
        return 0;
    }

    // One datagram: 0, or a Winsock error (kWsaEWouldBlock when a nonblocking socket has none).
    uint32_t ReceiveFrom(uint32_t handle, Datagram& out)
    {
        std::unique_lock lock(mutex_);
        for (;;)
        {
            auto it = sockets_.find(handle);
            if (it == sockets_.end())
                return kWsaENotSock;
            if (!it->second.queue.empty())
            {
                out = std::move(it->second.queue.front());
                it->second.queue.pop_front();
                return 0;
            }
            if (it->second.nonblocking)
                return kWsaEWouldBlock;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
    }

    // Waits until one of `handles` has a datagram or the timeout passes (negative: forever).
    std::vector<uint32_t> WaitReadable(const std::vector<uint32_t>& handles, int64_t timeoutUs)
    {
        std::unique_lock lock(mutex_);
        const auto deadline = Clock::now() + std::chrono::microseconds(std::max<int64_t>(timeoutUs, 0));
        for (;;)
        {
            std::vector<uint32_t> ready;
            for (uint32_t h : handles)
                if (auto it = sockets_.find(h); it != sockets_.end() && !it->second.queue.empty())
                    ready.push_back(h);
            if (!ready.empty() || timeoutUs == 0)
                return ready;
            if (timeoutUs < 0)
                cv_.wait_for(lock, std::chrono::milliseconds(100));
            else if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
                timeoutUs = 0;
        }
    }

    void WriteSelfXnAddr(uint8_t* xna)
    {
        std::lock_guard lock(mutex_);
        if (liveId_)
            WriteLiveXnAddr(xna, liveId_, liveEpoch_, publicEp_.ip);
        else
            WriteLanXnAddr(xna, selfIp_, hostPort_);
    }

    void SendMatch(LiveType type, const std::vector<uint8_t>& body)
    {
        {
            std::lock_guard lock(mutex_);
            if (!liveId_)
                return;
        }
        SendLive(server_, type, body);
    }

    // The virtual IP of a peer's XNADDR.
    uint32_t VipOfXnAddr(const uint8_t* xna)
    {
        const uint32_t ina = Load32(xna);
        if ((ina & 0xFF000000) == kLiveInaTag)
        {
            const uint32_t id = ina & 0x00FFFFFF;
            std::lock_guard lock(mutex_);
            if (id == liveId_)
                return kSelfVip;
            return LivePeerLocked(id).vip;
        }
        return LanVipOf({ ina, Load16(xna + 8) });
    }

    // The XNADDR behind a virtual IP; false when the IP is unknown.
    bool XnAddrOfVip(uint32_t vip, uint8_t* xna)
    {
        if (vip == kSelfVip)
        {
            WriteSelfXnAddr(xna);
            return true;
        }
        std::lock_guard lock(mutex_);
        if (auto it = liveIdByVip_.find(vip); it != liveIdByVip_.end())
        {
            WriteLiveXnAddr(xna, it->second, liveEpoch_, livePeers_[it->second].publicEp.ip);
            return true;
        }
        if (auto it = endpointByVip_.find(vip); it != endpointByVip_.end())
        {
            WriteLanXnAddr(xna, it->second.ip, it->second.port);
            return true;
        }
        return false;
    }

private:
    VNet()
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        const char* server = getenv("FM2_SERVER");
        live_ = server && *server;
        relayOnly_ = Env("FM2_SERVER_RELAY");
        selfIp_ = live_ ? LanAddress() : INADDR_LOOPBACK;
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        BOOL one = TRUE;
        setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
        // A datagram to a closed port must not end the receive loop (WSAECONNRESET).
        DWORD off = FALSE, ret = 0;
        WSAIoctl(fd_, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr); // SIO_UDP_CONNRESET
        uint16_t first = kHostPortFirst, count = kHostPortCount;
        if (const char* port = getenv("FM2_NET_PORT"); port && *port)
            first = uint16_t(atoi(port)), count = 1;
        for (uint16_t p = first; p < first + count && !hostPort_; p++)
            if (BindHost(live_, p))
                hostPort_ = p;
        if (!hostPort_ && live_ && BindHost(true, 0))
        {
            sockaddr_in a{};
            int len = sizeof(a);
            getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
            hostPort_ = ntohs(a.sin_port);
        }
        if (!hostPort_)
        {
            Log("net: no free host UDP port in %u-%u", first, first + count - 1);
            closesocket(fd_);
            fd_ = INVALID_SOCKET;
            return;
        }
        Log("net: virtual network on %s UDP %u", IpString(selfIp_).c_str(), hostPort_);
        std::thread([this] { ReceiveLoop(); }).detach();
        if (live_)
        {
            std::random_device rd;
            liveToken_ = uint64_t(rd()) << 32 | rd();
            std::thread([this] { LiveLoop(); }).detach();
            // A moment for the server, so the game sees its Live address from the start.
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(3), [this] { return liveId_ != 0; });
            if (!liveId_)
                Log("net: no answer from the FM2 Live server %s yet; still trying", ServerName().c_str());
        }
    }

    bool BindHost(bool any, uint16_t port)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
        return bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }

    static uint32_t LanAddress()
    {
        // The source address Windows picks for an outgoing route; nothing is sent.
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(53);
        inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);
        uint32_t ip = INADDR_LOOPBACK;
        if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
        {
            sockaddr_in local{};
            int len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
                ip = ntohl(local.sin_addr.s_addr);
        }
        closesocket(s);
        return ip;
    }

    static std::vector<uint8_t> LanPacket(uint16_t sourcePort, uint16_t port, const uint8_t* data, size_t size)
    {
        std::vector<uint8_t> packet(kHeaderSize + size);
        Store32(packet.data(), kMagic);
        Store16(packet.data() + 4, sourcePort);
        Store16(packet.data() + 6, port);
        memcpy(packet.data() + kHeaderSize, data, size);
        return packet;
    }

    uint32_t LanVipOf(Endpoint endpoint)
    {
        if (endpoint.ip == selfIp_ && endpoint.port == hostPort_)
            return kSelfVip;
        std::lock_guard lock(mutex_);
        const uint64_t key = uint64_t(endpoint.ip) << 16 | endpoint.port;
        if (auto it = vipByEndpoint_.find(key); it != vipByEndpoint_.end())
            return it->second;
        const uint32_t vip = kVipNet + nextVip_++;
        vipByEndpoint_[key] = vip;
        endpointByVip_[vip] = endpoint;
        Log("net: peer %s:%u is 0.77.%u.%u", IpString(endpoint.ip).c_str(), endpoint.port, (vip >> 8) & 0xFF, vip & 0xFF);
        return vip;
    }

    bool LanEndpointOf(uint32_t vip, Endpoint& out)
    {
        std::lock_guard lock(mutex_);
        auto it = endpointByVip_.find(vip);
        if (it == endpointByVip_.end())
            return false;
        out = it->second;
        return true;
    }

    // The player with server id `id`, made on first sight. Needs mutex_.
    LivePeer& LivePeerLocked(uint32_t id)
    {
        LivePeer& peer = livePeers_[id];
        if (!peer.vip)
        {
            peer.vip = kVipNet + nextVip_++;
            liveIdByVip_[peer.vip] = id;
            Log("net: player #%u is 0.77.%u.%u", id, (peer.vip >> 8) & 0xFF, peer.vip & 0xFF);
        }
        return peer;
    }

    // Starts hole punching toward `id` unless a direct path exists or a recent attempt failed.
    // Needs mutex_.
    void StartPunchLocked(uint32_t id)
    {
        if (relayOnly_)
            return;
        LivePeer& peer = LivePeerLocked(id);
        const auto now = Clock::now();
        if (peer.direct.ip || !peer.publicEp.ip)
            return;
        if (peer.punchStarted != Clock::time_point{} && now - peer.punchStarted < std::chrono::seconds(30))
            return;
        peer.punchStarted = now;
        peer.punchUntil = now + std::chrono::seconds(6);
        peer.nextPunch = now;
    }

    std::string ServerName() const { return wsMode_ ? serverHost_ : serverHost_ + ":" + std::to_string(serverPort_); }

    bool ResolveServer()
    {
        std::string spec = getenv("FM2_SERVER");
        if (WebSocketClient::IsUrl(spec.c_str()))
        {
            // The server is a Worker (server/live): its messages go over a WebSocket, the players' datagrams
            // still go player to player (each finds its public address with STUN)
            std::lock_guard lock(mutex_);
            if (!wsMode_)
            {
                wsMode_ = true;
                serverHost_ = spec;
                server_ = kWsServer;
                std::thread([this] { WebSocketLoop(); }).detach();
            }
            return true;
        }
        serverPort_ = kLiveDefaultPort;
        if (auto colon = spec.rfind(':'); colon != std::string::npos)
        {
            serverPort_ = uint16_t(atoi(spec.c_str() + colon + 1));
            spec.resize(colon);
        }
        serverHost_ = spec;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(spec.c_str(), nullptr, &hints, &result) != 0 || !result)
        {
            Log("net: cannot resolve the FM2 Live server %s", spec.c_str());
            return false;
        }
        const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(result);
        std::lock_guard lock(mutex_);
        server_ = { ip, serverPort_ };
        return true;
    }

    void SendLive(Endpoint to, LiveType type, const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> packet(5 + body.size());
        Store32(packet.data(), kLiveMagic);
        packet[4] = type;
        memcpy(packet.data() + 5, body.data(), body.size());
        if (wsMode_ && to == kWsServer)
        {
            std::shared_ptr<WebSocketClient> ws;
            {
                std::lock_guard lock(mutex_);
                ws = ws_;
            }
            if (ws)
                ws->Send(packet.data(), packet.size());
            return;
        }
        SendHost(to, packet);
    }

    // The Worker's WebSocket: connected (again) while the game runs; each message is one server packet
    void WebSocketLoop()
    {
        for (;;)
        {
            std::shared_ptr<WebSocketClient> ws = WebSocketClient::Open(serverHost_);
            if (ws)
            {
                Log("net: connected to the FM2 Live server %s", serverHost_.c_str());
                {
                    std::lock_guard lock(mutex_);
                    ws_ = ws;
                }
                SendHello();
                for (std::vector<uint8_t> message; ws->Receive(message);)
                    if (message.size() >= 5 && Load32(message.data()) == kLiveMagic)
                        OnLive(kWsServer, message[4], message.data() + 5, message.size() - 5);
                {
                    std::lock_guard lock(mutex_);
                    ws_.reset();
                    liveId_ = 0;
                }
                Log("net: lost the FM2 Live server %s; reconnecting", serverHost_.c_str());
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    // STUN (RFC 5389) from our UDP socket: the public address other players reach us at, which the server
    // can't see over a WebSocket. FM2_STUN=host:port (default stun.cloudflare.com:3478).
    void SendStun()
    {
        if (!stunServer_.ip)
        {
            std::string spec = getenv("FM2_STUN") ? getenv("FM2_STUN") : "stun.cloudflare.com:3478";
            uint16_t port = 3478;
            if (auto colon = spec.rfind(':'); colon != std::string::npos)
                port = uint16_t(atoi(spec.c_str() + colon + 1)), spec.resize(colon);
            addrinfo hints{}, *result = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(spec.c_str(), nullptr, &hints, &result) != 0 || !result)
                return;
            stunServer_ = { ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr), port };
            freeaddrinfo(result);
        }
        std::vector<uint8_t> request(20, 0);
        Store16(request.data(), 0x0001); // binding request, no attributes
        Store32(request.data() + 4, kStunCookie);
        for (size_t i = 8; i < 20; i++)
            request[i] = uint8_t(liveToken_ >> (8 * (i % 8)));
        SendHost(stunServer_, request);
    }

    // A STUN binding success: its XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS) as our public endpoint
    bool OnStun(const uint8_t* p, size_t n)
    {
        if (n < 20 || Load16(p) != 0x0101 || Load32(p + 4) != kStunCookie)
            return false;
        for (size_t at = 20; at + 4 <= n;)
        {
            const uint16_t type = Load16(p + at), size = Load16(p + at + 2);
            if (at + 4 + size > n)
                break;
            if ((type == 0x0020 || type == 0x0001) && size >= 8 && p[at + 5] == 1) // IPv4
            {
                uint16_t port = Load16(p + at + 6);
                uint32_t ip = Load32(p + at + 8);
                if (type == 0x0020)
                    port ^= uint16_t(kStunCookie >> 16), ip ^= kStunCookie;
                std::lock_guard lock(mutex_);
                if (stunPublic_.ip != ip || stunPublic_.port != port)
                    Log("net: public address %s:%u (STUN)", IpString(ip).c_str(), port);
                stunPublic_ = { ip, port };
                return true;
            }
            at += 4 + ((size + 3) & ~3u);
        }
        return true;
    }

    void SendHello()
    {
        const std::string name = GetSettings().profile.substr(0, 15);
        const uint64_t xuid = OnlineLocalXuid(0);
        std::vector<uint8_t> body(24);
        Store16(body.data(), kLiveVersion);
        Store32(body.data() + 2, uint32_t(liveToken_ >> 32));
        Store32(body.data() + 6, uint32_t(liveToken_));
        Store32(body.data() + 10, uint32_t(xuid >> 32));
        Store32(body.data() + 14, uint32_t(xuid));
        Store32(body.data() + 18, selfIp_);
        Store16(body.data() + 22, hostPort_);
        body.push_back(0); // room: everyone on the server
        body.push_back(uint8_t(name.size()));
        body.insert(body.end(), name.begin(), name.end());
        if (wsMode_)
        {
            // where other players reach us (STUN; zeros until it answers), which a server behind a WebSocket can't see
            Endpoint pub;
            {
                std::lock_guard lock(mutex_);
                pub = stunPublic_;
            }
            body.resize(body.size() + 6);
            Store32(body.data() + body.size() - 6, pub.ip);
            Store16(body.data() + body.size() - 2, pub.port);
        }
        SendLive(server_, kHello, body);
    }

    // Registration, keep-alive and hole punching.
    void LiveLoop()
    {
        Clock::time_point nextHello{}, nextResolve{}, nextStun{};
        bool resolved = false;
        for (;;)
        {
            const auto now = Clock::now();
            if (!resolved && now >= nextResolve)
            {
                resolved = ResolveServer();
                nextResolve = now + std::chrono::seconds(10);
            }
            if (resolved && wsMode_ && now >= nextStun)
            {
                SendStun();
                Endpoint pub;
                {
                    std::lock_guard lock(mutex_);
                    pub = stunPublic_;
                }
                nextStun = now + std::chrono::seconds(pub.ip ? 25 : 2); // keeps the NAT mapping open too
            }
            if (resolved && now >= nextHello && now >= helloPausedUntil_)
            {
                SendHello();
                std::lock_guard lock(mutex_);
                nextHello = now + std::chrono::seconds(liveId_ ? 5 : 1);
                if (liveId_ && now - lastWelcome_ > std::chrono::seconds(20))
                {
                    Log("net: lost the FM2 Live server %s; reconnecting", ServerName().c_str());
                    lastWelcome_ = now;
                }
            }
            std::vector<std::pair<Endpoint, std::vector<uint8_t>>> punches;
            {
                std::lock_guard lock(mutex_);
                for (auto& [id, peer] : livePeers_)
                {
                    if (peer.direct.ip || now >= peer.punchUntil || now < peer.nextPunch)
                        continue;
                    peer.nextPunch = now + std::chrono::milliseconds(300);
                    std::vector<uint8_t> body(10);
                    Store32(body.data(), liveId_);
                    Store32(body.data() + 4, id);
                    Store16(body.data() + 8, liveEpoch_);
                    punches.push_back({ peer.publicEp, body });
                    if (peer.localEp.ip && peer.localEp != peer.publicEp)
                        punches.push_back({ peer.localEp, body });
                }
            }
            for (auto& [to, body] : punches)
                SendLive(to, kPunch, body);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void OnLive(Endpoint from, uint8_t type, const uint8_t* p, size_t n)
    {
        bool fromServer;
        {
            std::lock_guard lock(mutex_);
            fromServer = from == server_;
        }
        switch (type)
        {
        case kWelcome:
        {
            if (!fromServer || n < 14)
                return;
            const uint32_t id = Load32(p), publicIp = Load32(p + 6);
            const uint16_t epoch = Load16(p + 4), publicPort = Load16(p + 10);
            const size_t motdSize = std::min<size_t>(Load16(p + 12), n - 14);
            bool changed;
            {
                std::lock_guard lock(mutex_);
                changed = id != liveId_ || epoch != liveEpoch_;
                liveId_ = id;
                liveEpoch_ = epoch;
                publicEp_ = { publicIp, publicPort };
                lastWelcome_ = Clock::now();
                cv_.notify_all();
            }
            if (changed)
                Log("net: signed in to %s as %s (player #%u), public address %s:%u%s%.*s", ServerName().c_str(),
                    GetSettings().profile.c_str(), id, IpString(publicIp).c_str(), publicPort, motdSize ? ": " : "", int(motdSize),
                    reinterpret_cast<const char*>(p + 14));
            return;
        }
        case kError:
        {
            if (!fromServer || n < 3)
                return;
            const size_t size = std::min<size_t>(Load16(p + 1), n - 3);
            static std::atomic<int> logged{ 0 };
            if (logged++ < 20)
                Log("net: server error %u: %.*s", p[0], int(size), reinterpret_cast<const char*>(p + 3));
            if (p[0] == 1)
                SendHello(); // "not registered": the server restarted
            if (p[0] == 3) // the profile's name is someone else's (server/live/src/names.js): asked again in a minute
            {
                helloPausedUntil_ = Clock::now() + std::chrono::seconds(60);
                ShowLiveNotice(L"Your name is taken on FM2 Live",
                               L"Another player has " + std::wstring(GetSettings().profile.begin(), GetSettings().profile.end()) +
                                   L". Rename your profile in the launcher to play online.");
            }
            return;
        }
        case kForward:
        {
            if (!fromServer || n < 20)
                return;
            const uint32_t id = Load32(p);
            uint32_t vip;
            {
                std::lock_guard lock(mutex_);
                LivePeer& peer = LivePeerLocked(id);
                peer.publicEp = { Load32(p + 4), Load16(p + 8) };
                peer.localEp = { Load32(p + 10), Load16(p + 14) };
                vip = peer.vip;
                if (!peer.direct.ip)
                    StartPunchLocked(id);
            }
            Deliver(vip, Load16(p + 16), Load16(p + 18), p + 20, n - 20);
            return;
        }
        case kPunch:
        case kPunchAck:
        {
            if (n < 10)
                return;
            const uint32_t id = Load32(p);
            std::vector<uint8_t> ack;
            {
                std::lock_guard lock(mutex_);
                if (relayOnly_ || Load32(p + 4) != liveId_ || Load16(p + 8) != liveEpoch_)
                    return;
                LivePeer& peer = LivePeerLocked(id);
                if (!peer.direct.ip) // the first path that works; punches may arrive on several
                {
                    Log("net: direct path to player #%u via %s:%u", id, IpString(from.ip).c_str(), from.port);
                    peer.direct = from;
                }
                if (type == kPunch)
                {
                    ack.resize(10);
                    Store32(ack.data(), liveId_);
                    Store32(ack.data() + 4, id);
                    Store16(ack.data() + 8, liveEpoch_);
                }
            }
            if (!ack.empty())
                SendLive(from, kPunchAck, ack);
            return;
        }
        case kData:
        {
            if (n < 8)
                return;
            uint32_t vip;
            {
                std::lock_guard lock(mutex_);
                auto it = livePeers_.find(Load32(p));
                if (it == livePeers_.end())
                    return;
                LivePeer& peer = it->second;
                if (from != peer.direct && from != peer.publicEp && from != peer.localEp)
                    return;
                if (!peer.direct.ip)
                    peer.direct = from;
                vip = peer.vip;
            }
            Deliver(vip, Load16(p + 4), Load16(p + 6), p + 8, n - 8);
            return;
        }
        case kMatchResults:
            if (fromServer)
                SessionsMatchResults(p, n);
            return;
        default:
            return;
        }
    }

    void SendHost(Endpoint endpoint, const std::vector<uint8_t>& packet)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(endpoint.port);
        a.sin_addr.s_addr = htonl(endpoint.ip);
        sendto(fd_, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }

    void Deliver(uint32_t vip, uint16_t sourcePort, uint16_t port, const uint8_t* data, size_t size)
    {
        LogPacket("recv", vip, sourcePort, port, data, size);
        uint32_t events[2] = { 0, 0 }; // a completed receive's, WSAEventSelect's
        {
            std::lock_guard lock(mutex_);
            for (auto& [handle, s] : sockets_)
            {
                if (s.port != port)
                    continue;
                Datagram d{ vip, sourcePort, std::vector<uint8_t>(data, data + size) };
                if (!s.pending.empty())
                {
                    const PendingRecv r = s.pending.front();
                    s.pending.pop_front();
                    uint32_t bytes;
                    CompleteRecvLocked(r, d, bytes, events[0]);
                }
                else
                {
                    if (s.queue.size() >= 256)
                        s.queue.pop_front();
                    s.queue.push_back(std::move(d));
                    if (s.selectMask & kFdRead)
                        events[1] = s.selectEvent;
                }
                cv_.notify_all();
                break;
            }
        }
        for (uint32_t e : events)
            SignalEvent(e);
    }

    // Copies a datagram into a pending receive's buffers and completes its OVERLAPPED: the byte
    // count, then the status. Returns the status; `event` is the OVERLAPPED's event, which the
    // caller sets once it has unlocked.
    uint32_t CompleteRecvLocked(const PendingRecv& r, const Datagram& d, uint32_t& bytes, uint32_t& event)
    {
        bytes = Scatter(r.buffers, d.data);
        if (r.from)
            WriteSockaddr(r.from, d.vip, d.port);
        if (r.fromLen)
            StoreBE32(r.fromLen, 16);
        const uint32_t status = bytes < d.data.size() ? kStatusBufferOverflow : 0;
        StoreBE32(r.overlapped + 4, bytes);
        std::atomic_thread_fence(std::memory_order_release);
        StoreBE32(r.overlapped, status);
        event = LoadBE32(r.overlapped + 16);
        return status;
    }

    std::vector<uint32_t> CancelRecvsLocked(GuestSocket& s)
    {
        std::vector<uint32_t> events;
        for (const PendingRecv& r : s.pending)
        {
            StoreBE32(r.overlapped + 4, 0);
            StoreBE32(r.overlapped, kStatusCancelled);
            if (const uint32_t e = LoadBE32(r.overlapped + 16))
                events.push_back(e);
        }
        s.pending.clear();
        cv_.notify_all();
        return events;
    }

    void ReceiveLoop()
    {
        std::vector<uint8_t> buffer(65536);
        for (;;)
        {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(fd_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 5)
            {
                if (n == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK)
                    return;
                continue;
            }
            const Endpoint endpoint{ ntohl(from.sin_addr.s_addr), ntohs(from.sin_port) };
            if (wsMode_ && endpoint == stunServer_ && OnStun(buffer.data(), size_t(n)))
                continue;
            const uint32_t magic = Load32(buffer.data());
            if (magic == kLiveMagic)
            {
                OnLive(endpoint, buffer[4], buffer.data() + 5, size_t(n) - 5);
                continue;
            }
            if (n < int(kHeaderSize) || magic != kMagic)
                continue;
            if (endpoint.port == hostPort_ && (endpoint.ip == selfIp_ || endpoint.ip == INADDR_LOOPBACK))
                continue; // our own broadcast
            Deliver(LanVipOf(endpoint), Load16(buffer.data() + 4), Load16(buffer.data() + 6), buffer.data() + kHeaderSize,
                size_t(n) - kHeaderSize);
        }
    }

    SOCKET fd_ = INVALID_SOCKET;
    uint32_t selfIp_ = 0;
    uint16_t hostPort_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<uint32_t, GuestSocket> sockets_;
    uint32_t nextHandle_ = 1;
    uint16_t nextEphemeral_ = 49152;
    std::unordered_map<uint64_t, uint32_t> vipByEndpoint_;
    std::unordered_map<uint32_t, Endpoint> endpointByVip_;
    uint32_t nextVip_ = 2;

    // The FM2 Live server (FM2_SERVER).
    bool live_ = false, relayOnly_ = false;
    std::string serverHost_;
    uint16_t serverPort_ = kLiveDefaultPort;
    Endpoint server_;
    bool wsMode_ = false;                 // FM2_SERVER is a ws(s):// URL (the Worker)
    std::shared_ptr<WebSocketClient> ws_; // its connection, while there is one
    Endpoint stunServer_, stunPublic_;
    uint64_t liveToken_ = 0; // tells the server a restarted game from a stale registration
    uint32_t liveId_ = 0;    // our id on the server, 0 until it welcomes us
    uint16_t liveEpoch_ = 0;
    Endpoint publicEp_;
    Clock::time_point lastWelcome_{};
    Clock::time_point helloPausedUntil_{}; // the server refused our name
    std::unordered_map<uint32_t, LivePeer> livePeers_;
    std::unordered_map<uint32_t, uint32_t> liveIdByVip_;
};

bool NetOn() { return LiveTrace(); }


bool Ours(uint32_t handle)
{
    return NetOn() && (handle & 0xFFFF0000) == kHandleBase && VNet::Get().IsOurs(handle);
}

uint64_t Fail(uint32_t error)
{
    SetError(error);
    return uint64_t(-1);
}

// recvfrom/recv: one datagram into the guest buffer.
uint64_t Receive(PPCContext& ctx, uint32_t from, uint32_t fromLen)
{
    Datagram d;
    if (const uint32_t error = VNet::Get().ReceiveFrom(ctx.r4.u32, d))
        return Fail(error);
    const size_t n = std::min<size_t>(d.data.size(), ctx.r6.u32);
    memcpy(GuestPtr(ctx.r5.u32), d.data.data(), n);
    if (from)
    {
        uint8_t* a = GuestPtr<uint8_t>(from);
        memset(a, 0, 16);
        Store16(a, 2); // AF_INET
        Store16(a + 2, d.port);
        Store32(a + 4, d.vip);
        if (fromLen)
            StoreBE32(fromLen, 16);
    }
    if (n < d.data.size())
        return Fail(kWsaEMsgSize);
    return n;
}

// Guest fd_set: u32 count, SOCKET array[64].
std::vector<uint32_t> ReadSet(uint32_t set)
{
    std::vector<uint32_t> handles;
    if (!set)
        return handles;
    const uint32_t count = std::min<uint32_t>(LoadBE32(set), 64);
    for (uint32_t i = 0; i < count; i++)
        handles.push_back(LoadBE32(set + 4 + 4 * i));
    return handles;
}

void WriteSet(uint32_t set, const std::vector<uint32_t>& handles)
{
    if (!set)
        return;
    StoreBE32(set, uint32_t(handles.size()));
    for (size_t i = 0; i < handles.size(); i++)
        StoreBE32(set + 4 + 4 * uint32_t(i), handles[i]);
}
}

bool TitleRelayAddress(sockaddr_in& address); // kernel/title_relay.cpp

// --- TCP: the title server connection (auction house, gifting, ...) as a real host socket. The
// title server is 127.0.0.1 to the game (kernel/imports_xam.cpp); FM2_TITLE_SERVER=host:port says
// where it really is: host:port (default 127.0.0.1:3074, server/live/xts_server.py), or a ws:// or wss:// URL
// (the Worker, through kernel/title_relay.cpp) -------------------------------------------------------
namespace
{
constexpr uint32_t kTcpBase = 0x54430000; // 'TC' + index

struct Tcp
{
    std::mutex m;
    std::unordered_map<uint32_t, SOCKET> sockets;
    uint32_t next = 1;
    static Tcp& Get() { static Tcp t; return t; }
};

bool IsTcp(uint32_t handle)
{
    if ((handle & 0xFFFF0000) != kTcpBase)
        return false;
    Tcp& t = Tcp::Get();
    std::lock_guard lock(t.m);
    return t.sockets.count(handle) != 0;
}

SOCKET HostTcp(uint32_t handle)
{
    Tcp& t = Tcp::Get();
    std::lock_guard lock(t.m);
    auto it = t.sockets.find(handle);
    return it == t.sockets.end() ? INVALID_SOCKET : it->second;
}

uint32_t CreateTcp()
{
    static const int started = [] { WSADATA wsa; return WSAStartup(MAKEWORD(2, 2), &wsa); }();
    (void)started;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return 0;
    Tcp& t = Tcp::Get();
    std::lock_guard lock(t.m);
    const uint32_t handle = kTcpBase | (t.next++ & 0xFFFF);
    t.sockets[handle] = s;
    return handle;
}

uint64_t TcpResult(int r)
{
    if (r == SOCKET_ERROR)
        return Fail(uint32_t(WSAGetLastError()));
    return uint64_t(uint32_t(r));
}

void TitleServerAddress(sockaddr_in& a)
{
    if (TitleRelayAddress(a)) // FM2_TITLE_SERVER=ws(s)://...: through kernel/title_relay.cpp
        return;
    std::string host = "127.0.0.1";
    uint16_t port = 3074;
    if (const char* e = getenv("FM2_TITLE_SERVER"))
    {
        host = e;
        if (const size_t colon = host.find(':'); colon != std::string::npos)
            port = uint16_t(atoi(host.c_str() + colon + 1)), host.resize(colon);
    }
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
}

void LogTcp(const char* what, uint32_t handle, const uint8_t* data, int n)
{
    std::string hex;
    for (int i = 0; i < n && i < 64; i++)
    {
        char b[4];
        snprintf(b, sizeof(b), "%02X", data[i]);
        hex += b;
    }
    Log("net: tcp %08X %s %d bytes: %s", handle, what, n, hex.c_str());
}
}

void NetSelfXnAddr(uint32_t xnaddr)
{
    if (NetOn() && VNet::Get().Ready())
        VNet::Get().WriteSelfXnAddr(GuestPtr<uint8_t>(xnaddr));
    else
        memset(GuestPtr(xnaddr), 0, 0x24);
}

void NetSendMatch(bool search, const std::vector<uint8_t>& body)
{
    if (NetOn() && VNet::Get().Ready())
        VNet::Get().SendMatch(search ? kMatchSearch : kMatchPublish, body);
}

// --- XNet -----------------------------------------------------------------------------

GUEST_EXPORT(NetDll_XNetStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetCleanup)
{
    if (NetOn() && VNet::Get().Ready())
        VNet::Get().CloseAll();
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_WSACleanup)
{
    if (NetOn() && VNet::Get().Ready())
        VNet::Get().CloseAll();
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_WSAGetLastError) { Return(ctx, NetOn() ? t_lastError : kWsaENetDown); }

GUEST_EXPORT(NetDll_WSAStartup)
{
    // (caller, version, WSADATA*): FM2 checks that wVersion (the first u16) is the 2.2 it asked
    // for, or it tears its network down again; offline it stays down, as it always has.
    if (const uint32_t data = ctx.r5.u32; data && NetOn())
    {
        *GuestPtr<uint16_t>(data) = __builtin_bswap16(0x0202);
        *GuestPtr<uint16_t>(data + 2) = __builtin_bswap16(0x0202);
    }
    Return(ctx, 0);
}

// The link is up (XNET_ETHERNET_LINK_ACTIVE | 100 Mbps | full duplex) under FM2_LIVE.
GUEST_EXPORT(NetDll_XNetGetEthernetLinkStatus) { Return(ctx, NetOn() ? 0x0B : 0); }

GUEST_EXPORT(NetDll_XNetGetTitleXnAddr)
{
    // (caller, XNADDR*): Ethernet, static, gateway, DNS, online
    if (!NetOn() || !VNet::Get().Ready())
    {
        Return(ctx, 1); // XNET_GET_XNADDR_NONE
        return;
    }
    if (ctx.r4.u32)
        VNet::Get().WriteSelfXnAddr(GuestPtr<uint8_t>(ctx.r4.u32));
    Return(ctx, 0x2 | 0x4 | 0x20 | 0x40 | 0x80);
}

GUEST_EXPORT(NetDll_XNetGetConnectStatus) { Return(ctx, NetOn() ? 2 : 3); } // connected / lost

GUEST_EXPORT(NetDll_XNetXnAddrToInAddr)
{
    // (caller, XNADDR*, XNKID*, IN_ADDR*)
    if (!NetOn() || !VNet::Get().Ready() || !ctx.r4.u32 || !ctx.r6.u32)
    {
        Return(ctx, kWsaEInval);
        return;
    }
    StoreBE32(ctx.r6.u32, VNet::Get().VipOfXnAddr(GuestPtr<uint8_t>(ctx.r4.u32)));
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetInAddrToXnAddr)
{
    // (caller, IN_ADDR, XNADDR*, XNKID*)
    uint8_t xna[36];
    if (NetOn() && VNet::Get().Ready() && VNet::Get().XnAddrOfVip(ctx.r4.u32, xna))
    {
        if (ctx.r5.u32)
            memcpy(GuestPtr(ctx.r5.u32), xna, sizeof(xna));
        Return(ctx, 0);
    }
    else
        Return(ctx, kWsaEInval);
}

GUEST_EXPORT(NetDll_XNetCreateKey)
{
    // (caller, XNKID*, XNKEY*): a random system link key pair (an id with the top bits 0)
    static std::mt19937_64 rng(std::random_device{}());
    for (uint32_t i = 0; i < 8; i++)
        *GuestPtr<uint8_t>(ctx.r4.u32 + i) = uint8_t(rng());
    *GuestPtr<uint8_t>(ctx.r4.u32) &= 0x0F;
    for (uint32_t i = 0; ctx.r5.u32 && i < 16; i++)
        *GuestPtr<uint8_t>(ctx.r5.u32 + i) = uint8_t(rng());
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetRegisterKey) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetUnregisterKey) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetUnregisterInAddr) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetConnect) { Return(ctx, 0); }

GUEST_EXPORT(NetDll_XNetRandom)
{
    // (caller, buffer, length): different on every PC, or every copy of the game takes the
    // others' session nonces for its own.
    static std::mutex m;
    static std::mt19937_64 rng(std::random_device{}());
    std::lock_guard lock(m);
    Log("net: XNetRandom %08X length %u (r3 %08X lr %08X)", ctx.r4.u32, ctx.r5.u32, ctx.r3.u32, uint32_t(ctx.lr));
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        *GuestPtr<uint8_t>(ctx.r4.u32 + i) = uint8_t(rng() >> 24);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetServerToInAddr)
{
    // (caller, IN_ADDR, service id, IN_ADDR*): a new secure address each time, as Xbox Live gives. The XTS client keeps
    // its connections by this address: given the same one again after a connection dropped, it found the dead
    // connection still listed and called into freed memory (connect() sends every title server TCP connection to
    // FM2_TITLE_SERVER, so the address only has to be unique).
    static std::atomic<uint32_t> next{ 0 };
    StoreIfNonNull(ctx.r6.u32, NetOn() ? 0x7F010000u | (++next & 0xFFFF) : ctx.r4.u32);
    Return(ctx, 0);
}

// --- Sockets: UDP ones on the virtual network, TCP ones (the title server) as host sockets ---

GUEST_EXPORT(NetDll_socket)
{
    // (caller, af, type, protocol)
    if (NetOn() && ctx.r5.u32 == 2 && VNet::Get().Ready())
    {
        Return(ctx, VNet::Get().CreateSocket());
        return;
    }
    if (NetOn() && ctx.r5.u32 == 1)
    {
        const uint32_t handle = CreateTcp();
        Log("net: tcp socket %08X (lr %08X)", handle, uint32_t(ctx.lr));
        Return(ctx, handle ? handle : Fail(kWsaENetDown));
        return;
    }
    if (NetOn())
    {
        static std::atomic<int> logged{ 0 };
        if (logged++ < 20)
            Log("net: socket(af %u, type %u, protocol %u) refused (lr %08X)", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    }
    Return(ctx, Fail(kWsaENetDown));
}

GUEST_EXPORT(NetDll_bind)
{
    // (caller, socket, sockaddr*, length)
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    const uint32_t error = VNet::Get().Bind(ctx.r4.u32, LoadBE16(ctx.r5.u32 + 2));
    Return(ctx, error ? Fail(error) : 0);
}

GUEST_EXPORT(NetDll_connect)
{
    // (caller, socket, sockaddr*, length)
    if (IsTcp(ctx.r4.u32))
    {
        sockaddr_in a{};
        TitleServerAddress(a);
        const int r = ::connect(HostTcp(ctx.r4.u32), reinterpret_cast<sockaddr*>(&a), sizeof(a));
        Log("net: tcp %08X connect (the game asked for %08X:%u) -> %d (%d)", ctx.r4.u32, LoadBE32(ctx.r5.u32 + 4),
            LoadBE16(ctx.r5.u32 + 2), r, r ? WSAGetLastError() : 0);
        Return(ctx, TcpResult(r));
        return;
    }
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    VNet::Get().Connect(ctx.r4.u32, LoadBE32(ctx.r5.u32 + 4), LoadBE16(ctx.r5.u32 + 2));
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_closesocket)
{
    if (IsTcp(ctx.r4.u32))
    {
        Log("net: tcp %08X closed", ctx.r4.u32);
        Tcp& t = Tcp::Get();
        std::lock_guard lock(t.m);
        ::closesocket(t.sockets[ctx.r4.u32]);
        t.sockets.erase(ctx.r4.u32);
    }
    if (Ours(ctx.r4.u32))
        VNet::Get().Close(ctx.r4.u32);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_shutdown)
{
    if (IsTcp(ctx.r4.u32))
        ::shutdown(HostTcp(ctx.r4.u32), int(ctx.r5.u32));
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_setsockopt) { Return(ctx, Ours(ctx.r4.u32) || IsTcp(ctx.r4.u32) ? 0 : Fail(kWsaENotSock)); }

GUEST_EXPORT(NetDll_getsockopt)
{
    // (caller, socket, level, name, value*, length*): every option reads as 0
    if (!Ours(ctx.r4.u32) && !IsTcp(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    StoreIfNonNull(ctx.r7.u32, 0);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_getsockname)
{
    // (caller, socket, sockaddr*, length*): 0.0.0.0 and the bound port
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    uint8_t* a = GuestPtr<uint8_t>(ctx.r5.u32);
    memset(a, 0, 16);
    Store16(a, 2);
    Store16(a + 2, VNet::Get().LocalPort(ctx.r4.u32));
    StoreIfNonNull(ctx.r6.u32, 16);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_ioctlsocket)
{
    // (caller, socket, command, u_long*)
    if (IsTcp(ctx.r4.u32))
    {
        u_long v = LoadBE32(ctx.r6.u32);
        const int r = ::ioctlsocket(HostTcp(ctx.r4.u32), long(ctx.r5.u32), &v);
        if (ctx.r5.u32 == 0x4004667F)
            StoreBE32(ctx.r6.u32, uint32_t(v));
        Return(ctx, TcpResult(r));
        return;
    }
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    if (ctx.r5.u32 == 0x8004667E) // FIONBIO
        VNet::Get().SetNonblocking(ctx.r4.u32, LoadBE32(ctx.r6.u32) != 0);
    else if (ctx.r5.u32 == 0x4004667F) // FIONREAD
        StoreBE32(ctx.r6.u32, VNet::Get().PendingBytes(ctx.r4.u32));
    else
    {
        Return(ctx, Fail(kWsaEInval));
        return;
    }
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_sendto)
{
    // (caller, socket, buffer, length, flags, sockaddr*, length)
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    uint32_t vip = 0;
    uint16_t port = 0;
    if (ctx.r8.u32)
        vip = LoadBE32(ctx.r8.u32 + 4), port = LoadBE16(ctx.r8.u32 + 2);
    else
        VNet::Get().Connected(ctx.r4.u32, vip, port);
    const uint32_t error = VNet::Get().SendTo(ctx.r4.u32, vip, port, GuestPtr<uint8_t>(ctx.r5.u32), ctx.r6.u32);
    Return(ctx, error ? Fail(error) : ctx.r6.u32);
}

GUEST_EXPORT(NetDll_send)
{
    // (caller, socket, buffer, length, flags)
    if (IsTcp(ctx.r4.u32))
    {
        const uint8_t* data = GuestPtr<uint8_t>(ctx.r5.u32);
        const int r = ::send(HostTcp(ctx.r4.u32), reinterpret_cast<const char*>(data), int(ctx.r6.u32), 0);
        LogTcp("sent", ctx.r4.u32, data, r);
        Return(ctx, TcpResult(r));
        return;
    }
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    uint32_t vip = 0;
    uint16_t port = 0;
    VNet::Get().Connected(ctx.r4.u32, vip, port);
    const uint32_t error = VNet::Get().SendTo(ctx.r4.u32, vip, port, GuestPtr<uint8_t>(ctx.r5.u32), ctx.r6.u32);
    Return(ctx, error ? Fail(error) : ctx.r6.u32);
}

GUEST_EXPORT(NetDll_recvfrom)
{
    // (caller, socket, buffer, length, flags, sockaddr*, length*)
    Return(ctx, Ours(ctx.r4.u32) ? Receive(ctx, ctx.r8.u32, ctx.r9.u32) : Fail(kWsaENotSock));
}

GUEST_EXPORT(NetDll_recv)
{
    // (caller, socket, buffer, length, flags)
    if (IsTcp(ctx.r4.u32))
    {
        uint8_t* data = GuestPtr<uint8_t>(ctx.r5.u32);
        const int r = ::recv(HostTcp(ctx.r4.u32), reinterpret_cast<char*>(data), int(ctx.r6.u32), 0);
        if (r != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
            LogTcp("received", ctx.r4.u32, data, r);
        Return(ctx, TcpResult(r));
        return;
    }
    Return(ctx, Ours(ctx.r4.u32) ? Receive(ctx, 0, 0) : Fail(kWsaENotSock));
}

GUEST_EXPORT(NetDll_select)
{
    // (caller, nfds, read set*, write set*, except set*, timeval*): our sockets are always
    // writable and readable when they have a datagram.
    int64_t timeoutUs = -1;
    if (ctx.r8.u32)
        timeoutUs = int64_t(int32_t(LoadBE32(ctx.r8.u32))) * 1000000 + int32_t(LoadBE32(ctx.r8.u32 + 4));
    // TCP sockets go to a host select (waiting up to 100 ms when there is nothing else to wait on)
    std::vector<uint32_t> writable, readable, udpRead, failed;
    std::vector<std::pair<uint32_t, SOCKET>> tcpRead, tcpWrite;
    for (uint32_t h : ReadSet(ctx.r5.u32))
        IsTcp(h) ? tcpRead.push_back({ h, HostTcp(h) }) : udpRead.push_back(h);
    for (uint32_t h : ReadSet(ctx.r6.u32))
        IsTcp(h) ? tcpWrite.push_back({ h, HostTcp(h) }) : writable.push_back(h);
    if (!tcpRead.empty() || !tcpWrite.empty())
    {
        fd_set r, w, x;
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_ZERO(&x);
        for (auto [h, s] : tcpRead)
            FD_SET(s, &r);
        for (auto [h, s] : tcpWrite)
        {
            FD_SET(s, &w);
            FD_SET(s, &x);
        }
        timeval wait{ 0, 0 };
        if (udpRead.empty() && writable.empty() && timeoutUs != 0)
            wait.tv_usec = long(timeoutUs < 0 ? 100000 : std::min<int64_t>(timeoutUs, 100000));
        ::select(0, &r, &w, &x, &wait);
        for (auto [h, s] : tcpRead)
            if (FD_ISSET(s, &r))
                readable.push_back(h);
        for (auto [h, s] : tcpWrite)
        {
            if (FD_ISSET(s, &w))
                writable.push_back(h);
            if (FD_ISSET(s, &x))
                failed.push_back(h);
        }
    }
    if (!writable.empty() || !readable.empty() || !failed.empty())
        timeoutUs = 0;
    if (NetOn() && !udpRead.empty())
    {
        const std::vector<uint32_t> udp = VNet::Get().WaitReadable(udpRead, timeoutUs);
        readable.insert(readable.end(), udp.begin(), udp.end());
    }
    WriteSet(ctx.r5.u32, readable);
    WriteSet(ctx.r6.u32, writable);
    WriteSet(ctx.r7.u32, failed);
    Return(ctx, readable.size() + writable.size() + failed.size());
}

GUEST_EXPORT(NetDll___WSAFDIsSet)
{
    // (socket, fd_set*): r3 is the socket here, not the caller
    for (uint32_t h : ReadSet(ctx.r4.u32))
        if (h == ctx.r3.u32)
        {
            Return(ctx, 1);
            return;
        }
    Return(ctx, 0);
}

// --- Overlapped I/O: FM2's session networking posts WSARecvFrom with an OVERLAPPED and sends with
// WSASendTo (from the Forza Horizon recomp's net.cpp, MIT) ------------------------------------

namespace
{
// WSARecvFrom/WSARecv on a datagram socket: r4 socket, r5 WSABUF*, r6 count, r7 received*,
// r8 flags*.
uint64_t RecvBuffers(PPCContext& ctx, uint32_t from, uint32_t fromLen, uint32_t overlapped)
{
    const uint32_t handle = ctx.r4.u32, buffers = ctx.r5.u32, count = ctx.r6.u32, received = ctx.r7.u32, flags = ctx.r8.u32;
    if (flags)
        StoreBE32(flags, 0);
    uint32_t bytes = 0, error = 0;
    if (overlapped)
        error = VNet::Get().RecvOverlapped(handle, { ReadBuffers(buffers, count), from, fromLen, overlapped }, bytes);
    else
    {
        Datagram d;
        error = VNet::Get().ReceiveFrom(handle, d);
        if (!error)
        {
            bytes = Scatter(ReadBuffers(buffers, count), d.data);
            if (from)
                WriteSockaddr(from, d.vip, d.port);
            if (fromLen)
                StoreBE32(fromLen, 16);
            if (bytes < d.data.size())
                error = kWsaEMsgSize;
        }
    }
    if (received && (!error || error == kWsaEMsgSize))
        StoreBE32(received, bytes);
    return error ? Fail(error) : 0; // kWsaIoPending: the receive completes later
}

// WSASendTo/WSASend on a datagram socket: r4 socket, r5 WSABUF*, r6 count, r7 sent*. A send
// completes at once, overlapped or not.
uint64_t SendBuffers(PPCContext& ctx, uint32_t vip, uint16_t port, uint32_t overlapped)
{
    const std::vector<uint8_t> data = Gather(ctx.r5.u32, ctx.r6.u32);
    if (const uint32_t error = VNet::Get().SendTo(ctx.r4.u32, vip, port, data.data(), data.size()))
        return Fail(error);
    const uint32_t size = uint32_t(data.size());
    StoreIfNonNull(ctx.r7.u32, size);
    if (overlapped)
    {
        StoreBE32(overlapped + 4, size);
        StoreBE32(overlapped, 0);
        SignalEvent(LoadBE32(overlapped + 16));
    }
    return 0;
}
}

// (caller, socket, WSABUF*, count, received*, flags*, sockaddr* from, int* fromlen, [stack]
// WSAOVERLAPPED*, completion routine)
GUEST_EXPORT(NetDll_WSARecvFrom)
{
    Return(ctx, Ours(ctx.r4.u32) ? RecvBuffers(ctx, ctx.r9.u32, ctx.r10.u32, StackArg(ctx, 8)) : Fail(kWsaENotSock));
}

// (caller, socket, WSABUF*, count, received*, flags*, WSAOVERLAPPED*, routine)
GUEST_EXPORT(NetDll_WSARecv)
{
    Return(ctx, Ours(ctx.r4.u32) ? RecvBuffers(ctx, 0, 0, ctx.r9.u32) : Fail(kWsaENotSock));
}

// (caller, socket, WSABUF*, count, sent*, flags, sockaddr* to, tolen, [stack] WSAOVERLAPPED*, routine)
GUEST_EXPORT(NetDll_WSASendTo)
{
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    uint32_t vip = 0;
    uint16_t port = 0;
    if (ctx.r9.u32)
        vip = LoadBE32(ctx.r9.u32 + 4), port = LoadBE16(ctx.r9.u32 + 2);
    else
        VNet::Get().Connected(ctx.r4.u32, vip, port);
    Return(ctx, SendBuffers(ctx, vip, port, StackArg(ctx, 8)));
}

// (caller, socket, WSABUF*, count, sent*, flags, WSAOVERLAPPED*, routine)
GUEST_EXPORT(NetDll_WSASend)
{
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    uint32_t vip = 0;
    uint16_t port = 0;
    VNet::Get().Connected(ctx.r4.u32, vip, port);
    Return(ctx, SendBuffers(ctx, vip, port, ctx.r9.u32));
}

// (caller, socket, WSAOVERLAPPED*, transferred*, wait, flags*) -> BOOL
GUEST_EXPORT(NetDll_WSAGetOverlappedResult)
{
    const uint32_t ov = ctx.r5.u32;
    if (!Ours(ctx.r4.u32) || !ov)
    {
        Return(ctx, Fail(kWsaENotSock) & 0);
        return;
    }
    uint32_t status = LoadBE32(ov);
    while (status == kStatusPending && ctx.r7.u32 && Ours(ctx.r4.u32))
    {
        VNet::Get().WaitOverlapped(ov);
        status = LoadBE32(ov);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    StoreIfNonNull(ctx.r6.u32, LoadBE32(ov + 4));
    StoreIfNonNull(ctx.r8.u32, 0);
    uint32_t error = kWsaEInval;
    switch (status)
    {
    case 0: error = 0; break;
    case kStatusPending: error = kWsaIoIncomplete; break;
    case kStatusCancelled: error = kWsaOperationAborted; break;
    case kStatusBufferOverflow: error = kWsaEMsgSize; break;
    }
    if (error)
        SetError(error);
    Return(ctx, error ? 0 : 1);
}

// (caller, socket)
GUEST_EXPORT(NetDll_WSACancelOverlappedIO)
{
    if (Ours(ctx.r4.u32))
        VNet::Get().CancelRecvs(ctx.r4.u32);
    Return(ctx, 0);
}

// (caller, socket, event, network events)
GUEST_EXPORT(NetDll_WSAEventSelect)
{
    if (!Ours(ctx.r4.u32))
    {
        Return(ctx, Fail(kWsaENotSock));
        return;
    }
    const uint32_t error = VNet::Get().EventSelect(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    Return(ctx, error ? Fail(error) : 0);
}

// For gpu/renderer.cpp: FM2 Live's message while its toast is up (seconds since it showed in `age`), or false
bool LiveNotice(std::wstring& title, std::wstring& line, float& age)
{
    std::lock_guard lock(g_noticeMutex);
    if (g_noticeSince == std::chrono::steady_clock::time_point{})
        return false;
    age = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_noticeSince).count();
    if (age > 6.0f)
        return false;
    title = g_noticeTitle, line = g_noticeLine;
    return true;
}
