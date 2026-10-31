#pragma once
// A blocking WebSocket client (WinHTTP), for the servers that are Cloudflare Workers: the title server relay
// (kernel/title_relay.cpp) and the FM2 Live session server (kernel/net.cpp). Binary messages only.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WebSocketClient
{
public:
    // url: ws://host[:port]/path or wss://...; null when it can't connect (logged)
    static std::unique_ptr<WebSocketClient> Open(const std::string& url);
    static bool IsUrl(const char* text); // ws:// or wss://
    ~WebSocketClient();
    bool Send(const void* data, size_t size);
    // The next message (pieces of a fragmented one are joined); false once the connection is gone
    bool Receive(std::vector<uint8_t>& message);
    void Close(); // unblocks a Receive in another thread

private:
    WebSocketClient() = default;
    void* session_ = nullptr;
    void* connection_ = nullptr;
    void* socket_ = nullptr;
};
