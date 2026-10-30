// The leaderboards under FM2_LIVE: the game's stats writes (XSessionWriteStats) go to the FM2 Live server
// (server/live, a Cloudflare Worker) and its reads (XUserReadStats, stats enumerators) are answered from it.
// FM2's 1884 views (its XLAST: Arcade per class and track, Free Run, Time Trials, Career events and their
// rollups) all have the same columns, so the server keeps one row per view and player: Rating (the order),
// Car, Assists, DateTime, Time, Mode and Perf Index. FM2_STATS_URL is the server (default a local
// wrangler dev); without one the views come back empty, as before.
#include "kernel/leaderboards.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "memory.h"
#include "settings.h"

namespace
{
// XUSER_DATA types
constexpr uint8_t kInt32 = 1, kInt64 = 2;

// The columns by the views' attribute ids (the XLAST's Field attributeId) and property ids.
struct Column
{
    uint16_t attribute;
    uint32_t property;
    const char* name;
};
constexpr Column kColumns[] = {
    { 1, 0x20000020, "car" }, { 2, 0x20000021, "time" },     { 3, 0x10000022, "mode" },
    { 4, 0x10000023, "pi" },  { 5, 0x20000024, "assists" }, { 6, 0x20000025, "datetime" },
};
constexpr uint32_t kRating = 0x20000026;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// A POST to the server; empty on failure. Reads wait for it on the game's thread, so after a failure the
// server is left alone for half a minute (the views come back empty meanwhile).
std::atomic<int64_t> g_retryAt{ 0 };

std::string Post(const wchar_t* path, const std::string& body)
{
    const int64_t now = int64_t(GetTickCount64());
    if (now < g_retryAt.load())
        return {};
    const char* env = getenv("FM2_STATS_URL");
    const std::wstring url = Widen(env && *env ? env : "http://127.0.0.1:8787");
    URL_COMPONENTS parts{ sizeof(parts) };
    wchar_t host[256] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return {};
    std::string out;
    HINTERNET session = WinHttpOpen(L"FM2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return {};
    WinHttpSetTimeouts(session, 1500, 1500, 1500, 3000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    DWORD status = 0, size = sizeof(status);
    if (request && WinHttpSendRequest(request, L"Content-Type: text/plain\r\n", DWORD(-1L), const_cast<char*>(body.data()), DWORD(body.size()),
                                      DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX) &&
        status == 200)
    {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(request, &avail) && avail)
        {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read))
                break;
            out.append(chunk.data(), read);
        }
    }
    else
    {
        Log("leaderboards: %ls failed (status %lu, error %lu)", path, status, GetLastError());
        g_retryAt = now + 30000;
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
}

std::string Hex(uint64_t xuid)
{
    char s[17];
    snprintf(s, sizeof(s), "%016llX", (unsigned long long)xuid);
    return s;
}

struct Row
{
    uint64_t xuid = 0;
    uint32_t rank = 0;
    int64_t rating = 0;
    std::string name;
    std::string values[6]; // kColumns' order, "-" when never written
};
struct View
{
    uint32_t id = 0, total = 0;
    std::vector<Row> rows;
};

std::vector<View> Parse(const std::string& text)
{
    std::vector<View> views;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream l(line);
        std::string kind;
        l >> kind;
        if (kind == "view")
        {
            views.emplace_back();
            l >> views.back().id >> views.back().total;
        }
        else if (kind == "row" && !views.empty())
        {
            Row r;
            std::string xuid, car, assists, datetime, time, mode, pi;
            l >> xuid >> r.rank >> r.rating >> car >> assists >> datetime >> time >> mode >> pi;
            r.xuid = strtoull(xuid.c_str(), nullptr, 16);
            // values in kColumns' order: car, time, mode, pi, assists, datetime
            r.values[0] = car, r.values[1] = time, r.values[2] = mode, r.values[3] = pi, r.values[4] = assists, r.values[5] = datetime;
            std::getline(l >> std::ws, r.name);
            views.back().rows.push_back(std::move(r));
        }
    }
    return views;
}
}

std::vector<LeaderboardSpec> LeaderboardSpecs(uint32_t specPtr, uint32_t count)
{
    std::vector<LeaderboardSpec> specs;
    for (uint32_t i = 0; i < count && specPtr && i < 64; i++)
    {
        LeaderboardSpec s;
        s.view = LoadBE32(specPtr + i * 0x88);
        const uint32_t columns = LoadBE32(specPtr + i * 0x88 + 4);
        for (uint32_t k = 0; k < columns && k < 64; k++)
            s.attributes.push_back(__builtin_bswap16(*GuestPtr<uint16_t>(specPtr + i * 0x88 + 8 + 2 * k)));
        specs.push_back(std::move(s));
    }
    return specs;
}

// XSessionWriteStats' views: each XSESSION_VIEW_PROPERTIES { view id, property count, XUSER_PROPERTY* },
// a property { id, pad, XUSER_DATA { type, pad[7], value at +8 } } (0x18 bytes). Sent in the background.
void LeaderboardsWrite(uint64_t xuid, uint32_t viewCount, uint32_t viewPtr)
{
    std::string body = Hex(xuid) + " " + GetSettings().profile + "\n";
    for (uint32_t i = 0; i < viewCount && viewPtr && i < 64; i++)
    {
        const uint32_t v = viewPtr + i * 12, count = LoadBE32(v + 4), props = LoadBE32(v + 8);
        std::string line = std::to_string(LoadBE32(v));
        for (uint32_t k = 0; k < count && props && k < 32; k++)
        {
            const uint32_t p = props + k * 0x18, id = LoadBE32(p);
            const uint8_t type = *GuestPtr<uint8_t>(p + 8);
            const int64_t value = type == kInt32 ? int64_t(int32_t(LoadBE32(p + 16)))
                                  : type == kInt64 ? int64_t(uint64_t(LoadBE32(p + 16)) << 32 | LoadBE32(p + 20))
                                                   : 0;
            if (type != kInt32 && type != kInt64)
            {
                Log("leaderboards: view %u property %08X has type %u", LoadBE32(v), id, type);
                continue;
            }
            const char* name = id == kRating ? "rating" : nullptr;
            for (const Column& c : kColumns)
                if (c.property == id)
                    name = c.name;
            if (name)
                line += " " + std::string(name) + "=" + std::to_string(value);
        }
        body += line + "\n";
    }
    Log("leaderboards: write %s", body.c_str());
    std::thread([body] { Log("leaderboards: written %s", Post(L"/stats/write", body).c_str()); }).detach();
}

// X_USER_STATS_READ_RESULTS { views, XUSER_STATS_VIEW* } at out, then the views { id, total rows, rows,
// XUSER_STATS_ROW* } (0x10), the rows { XUID, rank, pad, rating i64, gamertag[16], columns,
// XUSER_STATS_COLUMN* } (0x30) and their columns { attribute id u16, pad, XUSER_DATA } (0x18), as many
// as fit in size. query is the server's first line ("rank 1 50", "around <xuid> 50", "xuids - 0").
void LeaderboardsFill(uint32_t out, uint32_t size, const std::vector<LeaderboardSpec>& specs, const std::string& query,
                      const std::vector<uint64_t>& xuids)
{
    std::string body = query + "\n";
    for (const LeaderboardSpec& s : specs)
        body += std::to_string(s.view) + " ";
    body += "\n";
    for (uint64_t x : xuids)
        body += Hex(x) + " ";
    body += "\n";
    const std::vector<View> views = Parse(Post(L"/stats/read", body));

    const uint32_t n = uint32_t(specs.size());
    memset(GuestPtr(out), 0, std::min(size, 8 + n * 0x10));
    StoreBE32(out, n);
    StoreBE32(out + 4, n ? out + 8 : 0);
    uint32_t next = out + 8 + n * 0x10;
    const uint32_t end = out + size;
    uint32_t rowsTotal = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const LeaderboardSpec& spec = specs[i];
        const uint32_t v = out + 8 + i * 0x10;
        StoreBE32(v, spec.view);
        const View* found = nullptr;
        for (const View& view : views)
            if (view.id == spec.view)
                found = &view;
        if (!found)
            continue;
        StoreBE32(v + 4, found->total);
        const uint32_t rowSize = 0x30 + 0x18 * uint32_t(spec.attributes.size());
        uint32_t rows = 0;
        const uint32_t rowPtr = next;
        while (rows < found->rows.size() && next + 0x30 * (rows + 1) <= end)
            rows++;
        // the rows first, then all their columns (rows * 0x30 + rows * columns * 0x18)
        while (rows && rowPtr + rows * rowSize > end)
            rows--;
        uint32_t column = rowPtr + rows * 0x30;
        for (uint32_t r = 0; r < rows; r++)
        {
            const Row& row = found->rows[r];
            const uint32_t p = rowPtr + r * 0x30;
            memset(GuestPtr(p), 0, 0x30);
            StoreBE32(p, uint32_t(row.xuid >> 32));
            StoreBE32(p + 4, uint32_t(row.xuid));
            StoreBE32(p + 8, row.rank);
            StoreBE32(p + 16, uint32_t(uint64_t(row.rating) >> 32));
            StoreBE32(p + 20, uint32_t(row.rating));
            strncpy(GuestPtr<char>(p + 24), row.name.c_str(), 15);
            StoreBE32(p + 40, uint32_t(spec.attributes.size()));
            StoreBE32(p + 44, spec.attributes.empty() ? 0 : column);
            for (uint16_t attribute : spec.attributes)
            {
                memset(GuestPtr(column), 0, 0x18);
                *GuestPtr<uint16_t>(column) = __builtin_bswap16(attribute);
                uint32_t property = 0;
                int index = -1;
                for (int c = 0; c < 6; c++)
                    if (kColumns[c].attribute == attribute)
                        property = kColumns[c].property, index = c;
                const std::string& value = index >= 0 ? row.values[index] : std::string("-");
                if (value == "-" || !property)
                    *GuestPtr<uint8_t>(column + 8) = 0xFF; // XUSER_DATA_TYPE_NULL
                else if (property >> 28 == kInt32)
                {
                    *GuestPtr<uint8_t>(column + 8) = kInt32;
                    StoreBE32(column + 16, uint32_t(int32_t(strtoll(value.c_str(), nullptr, 10))));
                }
                else
                {
                    const uint64_t x = uint64_t(strtoll(value.c_str(), nullptr, 10));
                    *GuestPtr<uint8_t>(column + 8) = kInt64;
                    StoreBE32(column + 16, uint32_t(x >> 32));
                    StoreBE32(column + 20, uint32_t(x));
                }
                column += 0x18;
            }
        }
        StoreBE32(v + 8, rows);
        StoreBE32(v + 12, rows ? rowPtr : 0);
        next = column;
        rowsTotal += rows;
    }
    Log("leaderboards: read (%s) %u views, %u rows", query.c_str(), n, rowsTotal);
}
