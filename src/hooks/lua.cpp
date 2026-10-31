// The title's Lua 5.1 runtime: log script errors as they are thrown.
// luaD_throw(L, errcode) has the message on the stack top for runtime
// errors. Layout (32-bit big-endian): L->top at +8, TValue 16 bytes with
// the type tag at +8, TString characters at +16.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"
#include "input/wheel.h"
#include "settings.h"

PPC_FUNC_IMPL(__imp__sub_824B91F0);

// luaD_throw
PPC_FUNC(sub_824B91F0)
{
    const uint32_t L = ctx.r3.u32, errcode = ctx.r4.u32;
    const uint32_t top = __builtin_bswap32(*GuestPtr<uint32_t>(L + 8));
    const uint32_t tag = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16 + 8));
    if (tag == 4) // LUA_TSTRING
    {
        const uint32_t str = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16));
        const uint32_t len = __builtin_bswap32(*GuestPtr<uint32_t>(str + 12));
        Log("lua: error %u: %.*s", errcode, int(len < 1024 ? len : 1024), GuestPtr<const char>(str + 16));
    }
    else
        Log("lua: error %u (value type %u)", errcode, tag);
    __imp__sub_824B91F0(ctx, base);
}

// luaL_loadbuffer(L, buffer, size, name) and its variant: scripts on their way into the title's
// Lua. With a PlayStation pad in use (input/wheel.cpp; not with the launcher's "original Xbox 360
// button icons"), the options screen's controller layout labels its D-pad and left stick the other
// way round: the DualSense picture there (gpu/ps_glyphs.h) has its D-pad where the 360 pad has its
// left stick, and the game's label lines stay where they are. The script fills the label objects
// "LeftThumbstick" and "DPad"; their two names are swapped in the compiled chunk (each a
// length-prefixed constant, so the chunk keeps its size).
static void SwapLayoutLabels(uint32_t buffer, uint32_t size, uint32_t name)
{
    if (!buffer || !name || size < 64 || !PlayStationPadInUse() || GetSettings().xboxPrompts)
        return;
    const char* n = GuestPtr<const char>(name);
    if (!strstr(n, "P_OPT_ControllerBehavior"))
        return;
    uint8_t* p = GuestPtr<uint8_t>(buffer);
    static const uint8_t kStick[] = { 0, 0, 0, 15, 'L', 'e', 'f', 't', 'T', 'h', 'u', 'm', 'b', 's', 't', 'i', 'c', 'k', 0 };
    static const uint8_t kDPad[] = { 0, 0, 0, 5, 'D', 'P', 'a', 'd', 0 };
    std::vector<uint8_t> chunk(p, p + size);
    auto find = [&](const uint8_t* pat, size_t len) {
        auto it = std::search(chunk.begin(), chunk.end(), pat, pat + len);
        return it == chunk.end() ? SIZE_MAX : size_t(it - chunk.begin());
    };
    const size_t a = find(kStick, sizeof(kStick)), b = find(kDPad, sizeof(kDPad));
    if (a == SIZE_MAX || b == SIZE_MAX)
    {
        Log("lua: %s: layout labels not found, left as they are", n);
        return;
    }
    std::vector<uint8_t> out;
    out.reserve(size);
    const size_t first = std::min(a, b), second = std::max(a, b);
    const bool stickFirst = a < b;
    out.insert(out.end(), chunk.begin(), chunk.begin() + first);
    out.insert(out.end(), stickFirst ? kDPad : kStick, (stickFirst ? kDPad : kStick) + (stickFirst ? sizeof(kDPad) : sizeof(kStick)));
    out.insert(out.end(), chunk.begin() + first + (stickFirst ? sizeof(kStick) : sizeof(kDPad)), chunk.begin() + second);
    out.insert(out.end(), stickFirst ? kStick : kDPad, (stickFirst ? kStick : kDPad) + (stickFirst ? sizeof(kStick) : sizeof(kDPad)));
    out.insert(out.end(), chunk.begin() + second + (stickFirst ? sizeof(kDPad) : sizeof(kStick)), chunk.end());
    if (out.size() != size)
        return;
    memcpy(p, out.data(), size);
    Log("lua: %s: D-pad and left stick labels swapped for the DualSense picture", n);
}

std::string RandomEventsLuaBefore(const char* name, bool source, const char* text, uint32_t size); // hooks/random_events.cpp
std::string RandomEventsLuaSource(const char* text, uint32_t size);
bool RoguelikeLuaNote(const char* text, uint32_t size); // hooks/roguelike.cpp
std::string RoguelikeLuaSource(const char* text, uint32_t size);
std::string RoguelikeLuaBytecode(const char* data, uint32_t size);
bool MusicLuaNote(const char* text, uint32_t size); // audio/music.cpp
std::string MusicLuaSource(const char* text, uint32_t size);
bool CarSoundLuaNote(const char* text, uint32_t size); // audio/car_sound.cpp
std::string CarSoundLuaSource(const char* text, uint32_t size);

// A flow's Lua as the mods change it (Random Events, Roguelike Career), with in-race music's notes and the car sound
// mix's audio rows, or empty when unchanged.
static std::string ModLuaSource(const char* text, uint32_t size)
{
    std::string out = RandomEventsLuaSource(text, size);
    if (std::string rogue = out.empty() ? RoguelikeLuaSource(text, size) : RoguelikeLuaSource(out.data(), uint32_t(out.size())); !rogue.empty())
        out = std::move(rogue);
    if (std::string music = out.empty() ? MusicLuaSource(text, size) : MusicLuaSource(out.data(), uint32_t(out.size())); !music.empty())
        out = std::move(music);
    if (std::string mix = out.empty() ? CarSoundLuaSource(text, size) : CarSoundLuaSource(out.data(), uint32_t(out.size())); !mix.empty())
        out = std::move(mix);
    return out;
}

#if FM2_TITLE_UPDATE
// lua_pcall(L, nargs, nresults, errfunc): luaB_pcall's call (the base library's "pcall"); a title update address,
// pasted together so tools/xexremap.py leaves it be.
#define TU_SUB(address) sub_##address
PPC_EXTERN_FUNC(TU_SUB(824D2898));
#endif

// Lua to run before a script loads (the Random Events prize car): loaded with the same loader into the same state
// and run there with lua_pcall (its errors still reach luaD_throw's log), the stack put back as it was; then the
// script loads as usual.
template <typename Loader>
static void RunBefore(PPCContext& ctx, uint8_t* base, Loader load)
{
    const uint32_t L = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32, name = ctx.r6.u32;
    if (!L || !buffer || !name)
        return;
    const bool source = size < 4 || memcmp(GuestPtr<const char>(buffer), "\x1bLua", 4) != 0;
    std::string code = RandomEventsLuaBefore(GuestPtr<const char>(name), source, GuestPtr<const char>(buffer), size);
    // FM2_GIVE_CARS=id,id,...: those cars go into the garage once, free, as the main menu loads (Random Events' prize
    // purchase), then the profile is saved. For giving back cars a failed auction took.
    static const char* give = getenv("FM2_GIVE_CARS");
    if (code.empty() && give && *give && strstr(GuestPtr<const char>(name), "C_MYC_MainMenu.lua"))
    {
        code = "pcall(function() local p = ProfileManager.Main local c = p.Credits p.Credits = c + 10000000 ";
        for (const char* id = give; *id;)
        {
            code += "pcall(function() Lobby.activePlayer.garage:BuyCar(" + std::to_string(atoi(id)) + ", 1) end) ";
            id = strchr(id, ',') ? strchr(id, ',') + 1 : id + strlen(id);
        }
        code += "p.Credits = c ProfileManager.activePlayerProfile:Save() end)";
        Log("lua: giving cars %s", give);
        give = nullptr;
    }
#if FM2_TITLE_UPDATE
    if (code.empty())
        return;
    static const char kName[] = "=random events";
    const uint32_t mem = GuestAlloc(Heap::Virtual, uint32_t(code.size() + sizeof(kName)));
    if (!mem)
        return;
    memcpy(GuestPtr<char>(mem), code.data(), code.size());
    memcpy(GuestPtr<char>(mem + uint32_t(code.size())), kName, sizeof(kName));
    const PPCContext saved = ctx;
    const uint32_t top = LoadBE32(L + 8); // L->top
    ctx.r4.u64 = mem;
    ctx.r5.u64 = code.size();
    ctx.r6.u64 = mem + code.size();
    load(ctx, base);
    if (ctx.r3.u32 == 0)
    {
        ctx.r3.u64 = L;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        TU_SUB(824D2898)(ctx, base);
        Log("lua: random events code run before %s (lua_pcall %u)", GuestPtr<const char>(name), ctx.r3.u32);
    }
    else
        Log("lua: random events code didn't load (%u)", ctx.r3.u32);
    StoreBE32(L + 8, top);
    ctx = saved;
#else
    (void)base; (void)load;
#endif
}

// A script's text changed before it loads (RandomEventsLuaSource; compiled ones, RoguelikeLuaBytecode): the new text
// goes in place of the buffer. One guest copy per distinct text, kept, as a flow's scripts load again each time its
// screen opens.
static void Rewrite(PPCContext& ctx)
{
    const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32;
    if (!buffer || size < 4)
        return;
    const bool compiled = memcmp(GuestPtr<const char>(buffer), "\x1bLua", 4) == 0;
    const std::string text = compiled ? RoguelikeLuaBytecode(GuestPtr<const char>(buffer), size)
                                      : ModLuaSource(GuestPtr<const char>(buffer), size);
    if (text.empty())
        return;
    static std::mutex mutex;
    static std::unordered_map<std::string, uint32_t> copies;
    std::lock_guard lock(mutex);
    uint32_t& mem = copies[text];
    if (!mem && !(mem = GuestAlloc(Heap::Virtual, uint32_t(text.size()))))
        return;
    memcpy(GuestPtr<char>(mem), text.data(), text.size());
    ctx.r4.u64 = mem;
    ctx.r5.u64 = text.size();
}

#if FM2_TITLE_UPDATE
// luaL_loadstring(L, s): the flows' scripts (the Lua in Media\UI\Flows\*.xml) load through this one, the text
// being its own name. Rewritten as Rewrite does, with the copy NUL-terminated.
#define TU_IMP(address) __imp__sub_##address
PPC_FUNC_IMPL(TU_IMP(8256AD00));
PPC_FUNC(TU_SUB(8256AD00))
{
    const uint32_t s = ctx.r4.u32;
    if (s)
    {
        const char* text = GuestPtr<const char>(s);
        const std::string changed = ModLuaSource(text, uint32_t(strlen(text)));
        if (!changed.empty())
        {
            static std::mutex mutex;
            static std::unordered_map<std::string, uint32_t> copies;
            std::lock_guard lock(mutex);
            uint32_t& mem = copies[changed];
            if (mem || (mem = GuestAlloc(Heap::Virtual, uint32_t(changed.size() + 1))))
            {
                memcpy(GuestPtr<char>(mem), changed.c_str(), changed.size() + 1);
                ctx.r4.u64 = mem;
            }
        }
    }
    TU_IMP(8256AD00)(ctx, base);
}
#endif

PPC_FUNC_IMPL(__imp__sub_8254DDF8);
PPC_FUNC(sub_8254DDF8)
{
    SwapLayoutLabels(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (ctx.r4.u32 && !MusicLuaNote(GuestPtr<const char>(ctx.r4.u32), ctx.r5.u32) && !CarSoundLuaNote(GuestPtr<const char>(ctx.r4.u32), ctx.r5.u32))
        RoguelikeLuaNote(GuestPtr<const char>(ctx.r4.u32), ctx.r5.u32);
    RunBefore(ctx, base, __imp__sub_8254DDF8);
    Rewrite(ctx);
    __imp__sub_8254DDF8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_8254DF80);
PPC_FUNC(sub_8254DF80)
{
    SwapLayoutLabels(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (ctx.r4.u32 && !MusicLuaNote(GuestPtr<const char>(ctx.r4.u32), ctx.r5.u32) && !CarSoundLuaNote(GuestPtr<const char>(ctx.r4.u32), ctx.r5.u32))
        RoguelikeLuaNote(GuestPtr<const char>(ctx.r4.u32), ctx.r5.u32);
    RunBefore(ctx, base, __imp__sub_8254DDF8); // the plain loader: this one also runs what it loads
    Rewrite(ctx);
    __imp__sub_8254DF80(ctx, base);
}
