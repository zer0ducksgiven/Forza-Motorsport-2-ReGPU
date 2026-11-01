// The game's C runtime, decompiled: functions every part of the game calls, where a native version is also
// much faster than the translation (which moves one guest word at a time).
#include <cmath>

#include "decomp/decomp.h"

#if FM2_TITLE_UPDATE
// memset(dst r3, value r4, n r5) -> dst (TU 8242E050, 430 callers). The original stores bytes up to 4-byte
// alignment, then 16 bytes a turn, then the rest; it only reads the value's low byte.
PPC_FUNC_IMPL(TU_IMP(8242E050));
PPC_FUNC(TU_SUB(8242E050))
{
    static DecompStats stats{ "memset" };
    DecompCheck(stats, ctx, base, TU_IMP(8242E050),
        [](PPCContext& ctx, uint8_t* base) { memset(base + ctx.r3.u32, int(ctx.r4.u32 & 0xFF), ctx.r5.u32); },
        [](PPCContext& ctx) { return std::vector<DecompRegion>{ { ctx.r3.u32, ctx.r5.u32 } }; });
}

// memcpy(dst r3, src r4, n r5) -> dst (TU 8242D140, 840 callers). The original aligns dst to 8 bytes, then
// copies 8 bytes a turn (with cache touch hints), forwards. memmove gives the same result for every call
// that doesn't overlap, and a defined one for those that do.
PPC_FUNC_IMPL(TU_IMP(8242D140));
PPC_FUNC(TU_SUB(8242D140))
{
    static DecompStats stats{ "memcpy" };
    DecompCheck(stats, ctx, base, TU_IMP(8242D140),
        [](PPCContext& ctx, uint8_t* base) { memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32); },
        [](PPCContext& ctx) { return std::vector<DecompRegion>{ { ctx.r3.u32, ctx.r5.u32 } }; });
}

// _stricmp(a r3, b r4) -> a[i] - b[i] at the first difference, A-Z compared as a-z (TU 8242E0F0, 46 callers:
// file names, XML keys). At the end of b the raw difference is returned, a's character not lowered.
static int32_t StrICmp(const uint8_t* base, uint32_t a, uint32_t b)
{
    for (;; a++, b++)
    {
        int32_t ca = PPC_LOAD_U8(a), cb = PPC_LOAD_U8(b);
        if (cb == 0 || ca == cb)
        {
            if (cb == 0)
                return ca - cb;
            continue;
        }
        if (cb >= 'A' && cb <= 'Z')
            cb |= 32;
        if (ca >= 'A' && ca <= 'Z')
            ca |= 32;
        if (ca != cb)
            return ca - cb;
    }
}

// wcslen(s r3) -> characters before the 16-bit 0 (TU 8242FFD8, 57 callers: the game's UI text is UTF-16).
PPC_FUNC_IMPL(TU_IMP(8242FFD8));
PPC_FUNC(TU_SUB(8242FFD8))
{
    static DecompStats stats{ "wcslen" };
    DecompCheck(stats, ctx, base, TU_IMP(8242FFD8),
        [](PPCContext& ctx, uint8_t* base) {
            uint32_t s = ctx.r3.u32, n = 0;
            while (PPC_LOAD_U16(s + n * 2) != 0)
                n++;
            ctx.r3.u64 = n;
        },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

// strncpy(dst r3, src r4, n r5) -> dst (TU 8242F778, 21 callers): n bytes written in all, zero-padded after
// the copied string's terminator; no terminator if src is n long or more.
PPC_FUNC_IMPL(TU_IMP(8242F778));
PPC_FUNC(TU_SUB(8242F778))
{
    static DecompStats stats{ "strncpy" };
    DecompCheck(stats, ctx, base, TU_IMP(8242F778),
        [](PPCContext& ctx, uint8_t* base) {
            uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
            for (; n != 0; n--)
            {
                const uint8_t c = PPC_LOAD_U8(s++);
                PPC_STORE_U8(d++, c);
                if (c == 0)
                    break;
            }
            if (n > 1)
                memset(base + d, 0, n - 1);
        },
        [](PPCContext& ctx) { return std::vector<DecompRegion>{ { ctx.r3.u32, ctx.r5.u32 } }; });
}

// The second memcpy (TU 8242D560): bytes up to dst's 4-byte alignment, then a word at a time (each read whole
// before it's written), then the rest, forwards. A few start-up callers overlap source and destination and
// rely on exactly that, so overlapping copies follow it step for step; the rest are a plain memcpy.
static void Memcpy4(uint8_t* base, uint32_t d, uint32_t s, uint32_t n)
{
    if (d + n <= s || s + n <= d)
    {
        memcpy(base + d, base + s, n);
        return;
    }
    for (; n != 0 && (d & 3) != 0; n--)
        base[d++] = base[s++];
    for (uint32_t w = n >> 2; w != 0; w--, d += 4, s += 4)
    {
        uint32_t v;
        memcpy(&v, base + s, 4);
        memcpy(base + d, &v, 4);
    }
    for (n &= 3; n != 0; n--)
        base[d++] = base[s++];
}

PPC_FUNC_IMPL(TU_IMP(8242D560));
PPC_FUNC(TU_SUB(8242D560))
{
    static DecompStats stats{ "memcpy4" };
    DecompCheck(stats, ctx, base, TU_IMP(8242D560),
        [](PPCContext& ctx, uint8_t* base) { Memcpy4(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); },
        [](PPCContext& ctx) { return std::vector<DecompRegion>{ { ctx.r3.u32, ctx.r5.u32 } }; });
}

// memchr(s r3, c r4, n r5) -> the first byte equal to c, or 0 (TU 8242F9C0). c is compared as a 32-bit int.
PPC_FUNC_IMPL(TU_IMP(8242F9C0));
PPC_FUNC(TU_SUB(8242F9C0))
{
    static DecompStats stats{ "memchr" };
    DecompCheck(stats, ctx, base, TU_IMP(8242F9C0),
        [](PPCContext& ctx, uint8_t* base) {
            const int32_t c = ctx.r4.s32;
            uint32_t found = 0;
            for (uint32_t s = ctx.r3.u32, n = ctx.r5.u32; n != 0; s++, n--)
                if (int32_t(PPC_LOAD_U8(s)) == c)
                {
                    found = s;
                    break;
                }
            ctx.r3.u64 = found;
        },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

// strncmp(a r3, b r4, n r5) -> a[i] - b[i] at the first difference or end of a, within n (TU 8242FC70); 0 for
// n <= 0.
PPC_FUNC_IMPL(TU_IMP(8242FC70));
PPC_FUNC(TU_SUB(8242FC70))
{
    static DecompStats stats{ "strncmp" };
    DecompCheck(stats, ctx, base, TU_IMP(8242FC70),
        [](PPCContext& ctx, uint8_t* base) {
            int64_t n = ctx.r5.s64, d = 0;
            for (uint32_t a = ctx.r3.u32, b = ctx.r4.u32; n > 0; n--, a++, b++)
            {
                const uint8_t ca = PPC_LOAD_U8(a), cb = PPC_LOAD_U8(b);
                d = int64_t(ca) - int64_t(cb);
                if (d != 0 || ca == 0)
                    break;
            }
            ctx.r3.s64 = d;
        },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

// strstr(haystack r3, needle r4) -> the first match, or 0; an empty needle matches at once (TU 8242FF58).
PPC_FUNC_IMPL(TU_IMP(8242FF58));
PPC_FUNC(TU_SUB(8242FF58))
{
    static DecompStats stats{ "strstr" };
    DecompCheck(stats, ctx, base, TU_IMP(8242FF58),
        [](PPCContext& ctx, uint8_t* base) {
            const uint32_t needle = ctx.r4.u32;
            uint32_t found = 0;
            if (PPC_LOAD_U8(needle) == 0)
                found = ctx.r3.u32;
            else
                for (uint32_t h = ctx.r3.u32; PPC_LOAD_U8(h) != 0 && !found; h++)
                {
                    uint32_t i = 0;
                    while (PPC_LOAD_U8(needle + i) != 0 && PPC_LOAD_U8(h + i) == PPC_LOAD_U8(needle + i))
                        i++;
                    if (PPC_LOAD_U8(needle + i) == 0)
                        found = h;
                }
            ctx.r3.u64 = found;
        },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

// strrchr(s r3, c r4) -> the last byte equal to c, or 0; c = 0 finds the terminator (TU 82430510).
PPC_FUNC_IMPL(TU_IMP(82430510));
PPC_FUNC(TU_SUB(82430510))
{
    static DecompStats stats{ "strrchr" };
    DecompCheck(stats, ctx, base, TU_IMP(82430510),
        [](PPCContext& ctx, uint8_t* base) {
            const int32_t c = ctx.r4.s32;
            uint32_t s = ctx.r3.u32, found = 0;
            for (;; s++)
            {
                const int32_t b = PPC_LOAD_U8(s);
                if (b == c)
                    found = s;
                if (b == 0)
                    break;
            }
            ctx.r3.u64 = found;
        },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

// floor and ceil (TU 8242DBA0, 8242D9B8), step for step: truncate (fctidz/fcfid), step by 1 the right way, keep x
// itself when |x| >= 1e18, NaN or zero. Not the C library's: ceil(-0.5) is +0 here, -0 there.
static double Fsel(double a, double b, double c)
{
    return a >= 0.0 ? b : c; // NaN picks c, as fsel does
}

static double TruncD(double x)
{
    int64_t i;
    if (x != x)
        i = INT64_MIN;
    else if (x >= 9223372036854775808.0)
        i = INT64_MAX;
    else if (x < -9223372036854775808.0)
        i = INT64_MIN;
    else
        i = int64_t(x);
    return double(i);
}

static double Floor(double x)
{
    const double t = TruncD(x);
    double r = Fsel(x - t, t, t - 1.0);
    r = Fsel(1e18 - fabs(x), r, x);
    return Fsel(-fabs(x), x, r);
}

static double Ceil(double x)
{
    const double t = TruncD(x);
    double r = Fsel(t - x, t, t + 1.0);
    r = Fsel(1e18 - fabs(x), r, x);
    return Fsel(-fabs(x), x, r);
}

PPC_FUNC_IMPL(TU_IMP(8242DBA0));
PPC_FUNC(TU_SUB(8242DBA0))
{
    static DecompStats stats{ "floor" };
    DecompCheck(stats, ctx, base, TU_IMP(8242DBA0),
        [](PPCContext& ctx, uint8_t*) { ctx.f1.f64 = Floor(ctx.f1.f64); },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

PPC_FUNC_IMPL(TU_IMP(8242D9B8));
PPC_FUNC(TU_SUB(8242D9B8))
{
    static DecompStats stats{ "ceil" };
    DecompCheck(stats, ctx, base, TU_IMP(8242D9B8),
        [](PPCContext& ctx, uint8_t*) { ctx.f1.f64 = Ceil(ctx.f1.f64); },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}

PPC_FUNC_IMPL(TU_IMP(8242E0F0));
PPC_FUNC(TU_SUB(8242E0F0))
{
    static DecompStats stats{ "_stricmp" };
    DecompCheck(stats, ctx, base, TU_IMP(8242E0F0),
        [](PPCContext& ctx, uint8_t* base) { ctx.r3.s64 = StrICmp(base, ctx.r3.u32, ctx.r4.u32); },
        [](PPCContext&) { return std::vector<DecompRegion>{}; });
}
#endif
