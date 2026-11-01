// Maths in the Xbox SDK's Direct3D library (linked into the game), decompiled. Found hot on the render thread
// (guest thread 7) with FM2_SAMPLE_THREAD.
#include "decomp/decomp.h"

#if FM2_TITLE_UPDATE
static float LoadF(const uint8_t* base, uint32_t a)
{
    const uint32_t bits = PPC_LOAD_U32(a);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void StoreF(uint8_t* base, uint32_t a, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    PPC_STORE_U32(a, bits);
}

// out[i] = sum over j of v[j] * m[i][j] for an n x n row-major matrix of floats (TU 827500C0; its only caller is
// the 74 KB function at 82750DE0). Each step is a*b + c in double rounded to float, as XenonRecomp's fmadds, and
// out[i] is read back from memory each step, as the original does.
static void MatVec(uint8_t* base, uint32_t n, uint32_t m, uint32_t v, uint32_t out)
{
    for (uint32_t i = 0; i < n; i++, out += 4, m += n * 4)
    {
        StoreF(base, out, float(double(LoadF(base, v)) * double(LoadF(base, m))));
        for (uint32_t j = 1; j < n; j++)
            StoreF(base, out, float(double(LoadF(base, v + j * 4)) * double(LoadF(base, m + j * 4)) + double(LoadF(base, out))));
    }
}

PPC_FUNC_IMPL(TU_IMP(827500C0));
PPC_FUNC(TU_SUB(827500C0))
{
    static DecompStats stats{ "matvec" };
    DecompCheck(stats, ctx, base, TU_IMP(827500C0),
        [](PPCContext& ctx, uint8_t* base) { MatVec(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); },
        [](PPCContext& ctx) { return std::vector<DecompRegion>{ { ctx.r6.u32, ctx.r3.u32 * 4 } }; });
}
#endif
