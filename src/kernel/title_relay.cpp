// The title server connection (the auction house's Xbox Live title services) over a WebSocket, for a server that
// only takes HTTP: FM2_TITLE_SERVER=wss://host/xts (server/live, the Worker). A loopback listener stands in for the
// title server; each connection the game makes to it is carried to the WebSocket and back, so the game's TCP
// socket (net.cpp) works as it does with a plain host:port.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel/websocket.h"
#include "log.h"

namespace
{
// Carry one accepted connection: the game's bytes to the WebSocket, and its messages back
void Carry(SOCKET game, std::string url)
{
    std::shared_ptr<WebSocketClient> ws = WebSocketClient::Open(url);
    if (!ws)
    {
        closesocket(game);
        return;
    }
    Log("title relay: connected to %s", url.c_str());
    std::thread back([ws, game] {
        for (std::vector<uint8_t> message; ws->Receive(message);)
            for (size_t sent = 0; sent < message.size();)
            {
                const int n = send(game, reinterpret_cast<const char*>(message.data() + sent), int(message.size() - sent), 0);
                if (n <= 0)
                    break;
                sent += size_t(n);
            }
        shutdown(game, SD_BOTH); // the game sees the connection drop, and reconnects
    });
    char buffer[64 * 1024];
    for (int n; (n = recv(game, buffer, sizeof(buffer), 0)) > 0;)
        if (!ws->Send(buffer, size_t(n)))
            break;
    ws->Close();
    back.join();
    closesocket(game);
    Log("title relay: connection closed");
}
}

// The address the game's title server connection goes to when FM2_TITLE_SERVER is a ws:// or wss:// URL: a loopback
// listener started on first use. False for anything else (a plain host:port, connected to directly).
bool TitleRelayAddress(sockaddr_in& address)
{
    static std::mutex m;
    static sockaddr_in listening{};
    static std::string url;
    static bool tried = false, ready = false;
    std::lock_guard lock(m);
    if (!tried)
    {
        tried = true;
        const char* target = getenv("FM2_TITLE_SERVER");
        if (!WebSocketClient::IsUrl(target))
            return false;
        url = target;
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        listening.sin_family = AF_INET;
        listening.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(listening);
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&listening), sizeof(listening)) != 0 ||
            listen(s, 4) != 0 || getsockname(s, reinterpret_cast<sockaddr*>(&listening), &size) != 0)
        {
            Log("title relay: no listener (%d)", WSAGetLastError());
            return false;
        }
        std::thread([s] {
            for (SOCKET game; (game = accept(s, nullptr, nullptr)) != INVALID_SOCKET;)
                std::thread(Carry, game, url).detach();
        }).detach();
        ready = true;
        Log("title relay: 127.0.0.1:%u carries the title server to %s", ntohs(listening.sin_port), target);
    }
    if (ready)
        address = listening;
    return ready;
}
