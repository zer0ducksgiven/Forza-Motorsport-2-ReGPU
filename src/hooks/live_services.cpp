// Xbox Live title services (the auction house) under FM2_LIVE: the XTS client's own trace messages, the ban
// check, and Xbox Live user storage, which the auction connector keeps its transaction journal in.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <vector>

#include <windows.h>

#include "kernel/friends.h"
#include "kernel/imports.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "memory.h"

#if FM2_TITLE_UPDATE
// Title update addresses, pasted together so tools/xexremap.py leaves them be (functions the disc doesn't have)
#define TU_SUB(address) sub_##address
#define TU_IMP(address) __imp__sub_##address

// The XTS client's trace messages (its logger's slot 3: this, level, text): logged under FM2_LIVE.
PPC_FUNC_IMPL(TU_IMP(8250F448));
PPC_FUNC(TU_SUB(8250F448))
{
    if (const uint32_t text = ctx.r5.u32; text && getenv("FM2_LIVE"))
    {
        std::string s(reinterpret_cast<const char*>(base + text), strnlen(reinterpret_cast<const char*>(base + text), 1024));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        Log("xts: [%u] %s", ctx.r4.u32, s.c_str());
    }
    TU_IMP(8250F448)(ctx, base);
}

// Lobby.activePlayer.isBanned (the getter's only use): no one is banned from our own server. The TU's
// AuctionHouse flows show "...or your gamertag has been banned" when it's true.
PPC_FUNC_IMPL(TU_IMP(82302CE0));
PPC_FUNC_IMPL(__imp__sub_822A0028);
PPC_FUNC(TU_SUB(82302CE0))
{
    if (!getenv("FM2_LIVE"))
    {
        TU_IMP(82302CE0)(ctx, base);
        return;
    }
    static bool logged = false;
    if (!logged)
        logged = true, Log("live: isBanned read, answering false");
    ctx.r3.u64 = ctx.r4.u64; // the Lua value to set
    ctx.r4.u64 = 0;
    __imp__sub_822A0028(ctx, base);
}

// The friends list (Lobby.activePlayer:HasFriends / GetFriendsList, which Gift a Car sends to) is our own list
// (kernel/friends.cpp), not Xbox Live's. The player's friends object keeps it as a std::list at +76 built from
// XONLINE_FRIENDs (196 bytes) at +68 by sub_82503448(this, count) when its enumeration completes; +64 is
// "ready". The parser is handed our friends instead, and the getter sub_825037C8(this, list*, wait) has it
// rebuilt when they change.
PPC_FUNC_IMPL(TU_IMP(8226EB08));
PPC_FUNC_IMPL(TU_IMP(82503448));
PPC_FUNC(TU_SUB(82503448))
{
    if (!getenv("FM2_LIVE"))
    {
        TU_IMP(82503448)(ctx, base);
        return;
    }
    constexpr uint32_t kRecord = 196, kMax = 100;
    static const uint32_t buffer = GuestAlloc(Heap::Virtual, kRecord * kMax);
    const std::vector<LiveFriend> friends = FriendsList();
    const uint32_t count = std::min<uint32_t>(uint32_t(friends.size()), kMax);
    memset(base + buffer, 0, kRecord * kMax);
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t* r = base + buffer + i * kRecord;
        for (int b = 0; b < 8; b++)
            r[b] = uint8_t(friends[i].xuid >> (56 - 8 * b));
        strncpy(reinterpret_cast<char*>(r + 8), friends[i].name.c_str(), 15); // szGamertag[16]
    }
    const uint32_t self = ctx.r3.u32;
    uint32_t& records = *reinterpret_cast<uint32_t*>(base + self + 68);
    const uint32_t theirs = records; // the game's own buffer (it frees it), put back after
    PPCContext call = ctx;
    call.r3.u64 = self + 76;
    TU_IMP(8226EB08)(call, base); // empty the list
    records = __builtin_bswap32(buffer);
    ctx.r4.u64 = count;
    TU_IMP(82503448)(ctx, base);
    records = theirs;
    Log("live: friends list: %u", count);
}

PPC_FUNC_IMPL(TU_IMP(825037C8));
PPC_FUNC(TU_SUB(825037C8))
{
    if (getenv("FM2_LIVE") && base[ctx.r3.u32 + 49] == 0)
    {
        static uint32_t seen = 0, object = 0;
        if (seen != FriendsVersion() || object != ctx.r3.u32)
        {
            seen = FriendsVersion(), object = ctx.r3.u32;
            PPCContext call = ctx;
            call.r4.u64 = 0;
            TU_SUB(82503448)(call, base);
        }
    }
    TU_IMP(825037C8)(ctx, base);
}
#endif

// Xbox Live user storage (XStorage), which the auction connector keeps its transaction journal in: kept as
// files in the profile folder. Upload sub_823E40A0(user, path, size, buffer, overlapped) and
// download sub_823E3DC0(user, path, size, buffer, results size, results*, overlapped) -> 0, or an error
// with the thread's last error 0x8015C004 (XONLINE_E_STORAGE_FILE_NOT_FOUND).
namespace
{
// The file: profiles\<name>\live_storage\<path>, the path being a UTF-16 string. The auction connector passes one
// (its journal's name); the XTS client's own call at sign-in passes a pointer to other data, stored as "unnamed".
std::filesystem::path StorageFile(uint32_t path)
{
    std::string name;
    for (uint32_t i = 0; i < 100; i++)
    {
        const uint32_t c = __builtin_bswap16(*GuestPtr<uint16_t>(path + 2 * i));
        if (c == 0)
            break;
        if (c < 0x20 || c > 0x7E)
        {
            name.clear();
            break;
        }
        name += (isalnum(int(c)) || c == '.' || c == '-') ? char(c) : '_';
    }
    return ContentDir().parent_path() / "live_storage" / (name.empty() ? "unnamed" : name);
}
}

PPC_FUNC_IMPL(__imp__sub_823E40A0);
PPC_FUNC(sub_823E40A0)
{
    if (!getenv("FM2_LIVE"))
    {
        __imp__sub_823E40A0(ctx, base);
        return;
    }
    const std::filesystem::path file = StorageFile(ctx.r4.u32);
    const uint32_t size = ctx.r5.u32;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (FILE* f = _wfopen(file.c_str(), L"wb"))
    {
        fwrite(base + ctx.r6.u32, 1, size, f);
        fclose(f);
    }
    Log("live storage: upload '%s' %u bytes", file.filename().string().c_str(), size);
    ctx.r3.u64 = 0;
}

PPC_FUNC_IMPL(__imp__sub_823E3DC0);
PPC_FUNC(sub_823E3DC0)
{
    if (!getenv("FM2_LIVE"))
    {
        __imp__sub_823E3DC0(ctx, base);
        return;
    }
    const std::filesystem::path file = StorageFile(ctx.r4.u32);
    const uint32_t size = ctx.r5.u32, buffer = ctx.r6.u32, results = ctx.r8.u32;
    std::vector<uint8_t> data;
    if (FILE* f = _wfopen(file.c_str(), L"rb"))
    {
        uint8_t chunk[4096];
        for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
            data.insert(data.end(), chunk, chunk + n);
        fclose(f);
    }
    else
    {
        Log("live storage: download '%s': not found", file.filename().string().c_str());
        if (GuestThread* t = CurrentGuestThread())
            StoreBE32(t->kthread + kthread::LastError, 0x8015C004);
        ctx.r3.u64 = 0x8015C004;
        return;
    }
    const uint32_t n = std::min<uint32_t>(size, uint32_t(data.size()));
    memcpy(base + buffer, data.data(), n);
    if (results && ctx.r7.u32 >= 4)
    {
        memset(base + results, 0, ctx.r7.u32);
        StoreBE32(results, uint32_t(data.size()));
    }
    Log("live storage: download '%s' %u bytes", file.filename().string().c_str(), n);
    ctx.r3.u64 = 0;
}
