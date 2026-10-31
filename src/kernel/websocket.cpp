#include "kernel/websocket.h"

#include <windows.h>
#include <winhttp.h>

#include "log.h"

bool WebSocketClient::IsUrl(const char* text)
{
    return text && (strncmp(text, "ws://", 5) == 0 || strncmp(text, "wss://", 6) == 0);
}

std::unique_ptr<WebSocketClient> WebSocketClient::Open(const std::string& url)
{
    if (!IsUrl(url.c_str()))
        return nullptr;
    const bool secure = url.rfind("wss://", 0) == 0;
    std::string rest = url.substr(secure ? 6 : 5);
    const size_t slash = rest.find('/');
    std::string host = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (const size_t colon = host.find(':'); colon != std::string::npos)
        port = INTERNET_PORT(atoi(host.c_str() + colon + 1)), host.resize(colon);
    const std::wstring wideHost(host.begin(), host.end()), widePath(path.begin(), path.end());

    std::unique_ptr<WebSocketClient> ws(new WebSocketClient);
    ws->session_ = WinHttpOpen(L"FM2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ws->session_)
    {
        WinHttpSetTimeouts(ws->session_, 5000, 5000, 5000, 0); // resolve, connect, send; receives wait for the server
        ws->connection_ = WinHttpConnect(ws->session_, wideHost.c_str(), port, 0);
    }
    HINTERNET request = ws->connection_ ? WinHttpOpenRequest(ws->connection_, L"GET", widePath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                        : nullptr;
    if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
        ws->socket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
    const DWORD error = GetLastError();
    if (request)
        WinHttpCloseHandle(request);
    if (!ws->socket_)
    {
        static int logged = 0;
        if (logged++ < 10)
            Log("websocket: can't connect to %s (error %lu)", url.c_str(), error);
        return nullptr;
    }
    return ws;
}

WebSocketClient::~WebSocketClient()
{
    if (socket_)
    {
        WinHttpWebSocketClose(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(socket_);
    }
    if (connection_)
        WinHttpCloseHandle(connection_);
    if (session_)
        WinHttpCloseHandle(session_);
}

bool WebSocketClient::Send(const void* data, size_t size)
{
    return WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<void*>(data), DWORD(size)) == NO_ERROR;
}

bool WebSocketClient::Receive(std::vector<uint8_t>& message)
{
    message.clear();
    uint8_t buffer[16384];
    for (;;)
    {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(socket_, buffer, sizeof(buffer), &read, &type) != NO_ERROR ||
            type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return false;
        message.insert(message.end(), buffer, buffer + read);
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return true; // the last (or only) piece
    }
}

void WebSocketClient::Close()
{
    WinHttpWebSocketShutdown(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
}
