// Turn 10's name hash (TU 827F4280), the first decompiled game function. The game hashes every name it looks
// things up by with it: the ~15,800 property-name constants built at start-up ("height", "opacity", ...) and
// names at run time. It's sdbm-style: h = h * 65599 + c over at most 100 characters, the characters signed
// (extsb), and the top bit set on the result.
#include "decomp/decomp.h"

uint32_t DecompHashName(const uint8_t* base, uint32_t s)
{
    uint32_t h = 0;
    for (int n = 0; n < 100; n++, s++)
    {
        const int8_t c = int8_t(PPC_LOAD_U8(s));
        if (c == 0)
            break;
        h = h * 65599 + uint32_t(int32_t(c));
    }
    return h | 0x80000000;
}

#if FM2_TITLE_UPDATE
PPC_FUNC_IMPL(TU_IMP(827F4280));
PPC_FUNC(TU_SUB(827F4280))
{
    static DecompStats stats{ "name hash" };
    DecompCheck(stats, ctx, base, TU_IMP(827F4280),
        [](PPCContext& ctx, uint8_t* base) { ctx.r3.u64 = DecompHashName(base, ctx.r3.u32); },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}
#endif
