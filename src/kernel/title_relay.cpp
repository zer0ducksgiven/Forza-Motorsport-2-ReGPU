// The title server connection (the auction house's Xbox Live title services) over a WebSocket, for a server that
// only takes HTTP: FM2_TITLE_SERVER=wss://host/xts (server/live, the Worker). A loopback listener stands in for the
// title server; each connection the game makes to it is carried to the WebSocket and back, so the game's TCP
// socket (net.cpp) works as it does with a plain host:port.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "log.h"

namespace
{
struct Url
{
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool ParseUrl(const char* text, Url& url)
{
    std::string s = text;
    if (s.rfind("wss://", 0) == 0)
        url.secure = true, s = s.substr(6);
    else if (s.rfind("ws://", 0) == 0)
        s = s.substr(5);
    else
        return false;
    const size_t slash = s.find('/');
    std::string host = s.substr(0, slash), path = slash == std::string::npos ? "/" : s.substr(slash);
    url.port = url.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (const size_t colon = host.find(':'); colon != std::string::npos)
        url.port = INTERNET_PORT(atoi(host.c_str() + colon + 1)), host.resize(colon);
    url.host.assign(host.begin(), host.end());
    url.path.assign(path.begin(), path.end());
    return true;
}

// One WebSocket to the server: the handles to close with it
struct WebSocket
{
    HINTERNET session = nullptr, connection = nullptr, socket = nullptr;
    ~WebSocket()
    {
        if (socket)
        {
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket);
        }
        if (connection)
            WinHttpCloseHandle(connection);
        if (session)
            WinHttpCloseHandle(session);
    }
};

std::unique_ptr<WebSocket> Open(const Url& url)
{
    auto ws = std::make_unique<WebSocket>();
    ws->session = WinHttpOpen(L"FM2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ws->session)
        return nullptr;
    WinHttpSetTimeouts(ws->session, 5000, 5000, 5000, 0); // resolve, connect, send; receives wait for the server
    ws->connection = WinHttpConnect(ws->session, url.host.c_str(), url.port, 0);
    if (!ws->connection)
        return nullptr;
    HINTERNET request = WinHttpOpenRequest(ws->connection, L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
        return nullptr;
    bool ok = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok)
        ws->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    return ws->socket ? std::move(ws) : nullptr;
}

// Carry one accepted connection: the game's bytes to the WebSocket, and its messages back
void Carry(SOCKET game, Url url)
{
    std::shared_ptr<WebSocket> ws = Open(url);
    if (!ws)
    {
        Log("title relay: no WebSocket to %ls%ls (error %lu)", url.host.c_str(), url.path.c_str(), GetLastError());
        closesocket(game);
        return;
    }
    Log("title relay: connected to %ls%ls", url.host.c_str(), url.path.c_str());
    std::thread back([ws, game] {
        std::string buffer(64 * 1024, '\0');
        for (;;)
        {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            if (WinHttpWebSocketReceive(ws->socket, buffer.data(), DWORD(buffer.size()), &read, &type) != NO_ERROR ||
                type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                break;
            for (DWORD sent = 0; sent < read;)
            {
                const int n = send(game, buffer.data() + sent, int(read - sent), 0);
                if (n <= 0)
                    break;
                sent += DWORD(n);
            }
        }
        shutdown(game, SD_BOTH); // the game sees the connection drop, and reconnects
    });
    char buffer[64 * 1024];
    for (int n; (n = recv(game, buffer, sizeof(buffer), 0)) > 0;)
        if (WinHttpWebSocketSend(ws->socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, buffer, DWORD(n)) != NO_ERROR)
            break;
    WinHttpWebSocketClose(ws->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
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
    static Url url;
    static bool tried = false, ready = false;
    std::lock_guard lock(m);
    if (!tried)
    {
        tried = true;
        const char* target = getenv("FM2_TITLE_SERVER");
        if (!target || !ParseUrl(target, url))
            return false;
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
