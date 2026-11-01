// The game's HashedName start-up constants: about 16,000 static initialisers, half the functions the game runs to
// reach a race, each hashing one name string into a global (the property names the UI and the game look values up
// by). Their replacements are generated from the image at build time (tools/decomp/gen_hashed_names.py, which
// only takes a function that does exactly that) and all call this. With FM2_DECOMP_CHECK=1 each runs the
// original first and compares the global.
#include "decomp/decomp.h"

void HashedNameInit(PPCContext& ctx, uint8_t* base, PPCFunc* original, uint32_t string, uint32_t global)
{
    if (!DecompChecking())
    {
        if (DecompOff())
            original(ctx, base);
        else
            PPC_STORE_U32(global, DecompHashName(base, string));
        return;
    }
    static DecompStats stats{ "hashed name constant" };
    const uint32_t before = PPC_LOAD_U32(global);
    PPCContext o = ctx;
    original(o, base);
    const uint32_t expected = PPC_LOAD_U32(global);
    PPC_STORE_U32(global, before);
    const uint32_t hash = DecompHashName(base, string);
    PPC_STORE_U32(global, hash);
    DecompReport(stats, hash == expected, global, expected, hash, "global");
}
