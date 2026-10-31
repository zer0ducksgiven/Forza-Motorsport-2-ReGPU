// Xbox Live title services (the auction house) under FM2_LIVE: the XTS client's own trace messages, the ban
// check, and Xbox Live user storage, which the auction connector keeps its transaction journal in.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "kernel/imports.h"
#include "kernel/thread.h"

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
#endif

// Xbox Live user storage (XStorage), which the auction connector keeps its transaction journal in: kept as
// files under out/live_storage. Upload sub_823E40A0(user, path, size, buffer, overlapped) and
// download sub_823E3DC0(user, path, size, buffer, results size, results*, overlapped) -> 0, or an error
// with the thread's last error 0x8015C004 (XONLINE_E_STORAGE_FILE_NOT_FOUND).
namespace
{
// The file name: the path argument's first 16-bit units, in hex. That argument isn't the plain path string it looked
// like (its units are stack addresses, different each run), so a journal is only found again within one run; a
// missing journal reads as none, which the connector takes as a fresh start. To do: the real path (sub_823E40A0).
std::string StoragePath(uint32_t path)
{
    std::string s;
    char unit[5];
    for (uint32_t i = 0, c; i < 100 && (c = __builtin_bswap16(*GuestPtr<uint16_t>(path + 2 * i))) != 0; i++)
    {
        snprintf(unit, sizeof(unit), "%04X", c);
        s += unit;
    }
    return s;
}

std::string StorageFile(const std::string& path)
{
    return "out/live_storage/" + path;
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
    const std::string path = StoragePath(ctx.r4.u32);
    const uint32_t size = ctx.r5.u32;
    CreateDirectoryA("out", nullptr);
    CreateDirectoryA("out/live_storage", nullptr);
    if (FILE* f = fopen(StorageFile(path).c_str(), "wb"))
    {
        fwrite(base + ctx.r6.u32, 1, size, f);
        fclose(f);
    }
    Log("live storage: upload '%s' %u bytes", path.c_str(), size);
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
    const std::string path = StoragePath(ctx.r4.u32);
    const uint32_t size = ctx.r5.u32, buffer = ctx.r6.u32, results = ctx.r8.u32;
    std::vector<uint8_t> data;
    if (FILE* f = fopen(StorageFile(path).c_str(), "rb"))
    {
        uint8_t chunk[4096];
        for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
            data.insert(data.end(), chunk, chunk + n);
        fclose(f);
    }
    else
    {
        Log("live storage: download '%s': not found", path.c_str());
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
    Log("live storage: download '%s' %u bytes", path.c_str(), n);
    ctx.r3.u64 = 0;
}
