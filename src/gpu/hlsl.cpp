#include <algorithm>
#include <bit>
#include <cmath>
#include "gpu/hlsl.h"
#include "live_switch.h"

#include <cstdio>

namespace
{
using namespace ucode;

const char kPreamble[] = R"(
cbuffer VSConstants : register(b0)
{
    float4 vc[256];
    uint4 vb[2];      // 256 bool constants
    uint4 vl[8];      // 32 loop constants: count | start << 8 | step << 16
    uint4 vf[96];     // vertex fetch: x byte offset (gmem, or gphys if z), y endian
    uint4 vinfo;      // x index list byte offset, y position mode (0 clip, 1 screen) | 2 points, z/w target size
    float4 vpoint;    // xy constant point diameter (pixels), z/w min/max diameter
    float4 vscale;    // PA_CL_VPORT_X/Y/ZSCALE
    float4 voffset;   // PA_CL_VPORT_X/Y/ZOFFSET
    uint4 vte;        // x PA_CL_VTE_CNTL
    // Vertex fetch k of this shader (see VFetchK): x fetch constant | format << 8 | signed << 14
    // | normalized << 15 | round << 16, y stride, z offset (dwords), w dest swizzle | exp << 16.
    uint4 vfl[32];
    // The "whysthisdealer" easter egg (hooks/dealer.cpp): every mesh of a car takes its squash in clip space,
    // opos += K (opos.xyw - o.xyw): rows 0-3 xyz = K's rows (clip x, y, z, w), rows 0-2 w = o.xyw, row 3 w = on.
    float4 vsquash[4];
    // x: polygon offset added to window-space depth (PA_SU_POLY_OFFSET, renderer.cpp)
    float4 vpoly;
    // xy: clip-space factors for perspective geometry in a wide pass (outputs wider than
    // 16:9; 1 otherwise): x narrows the view to the wider screen (Hor+), y zooms in when
    // FM2_WIDE_FOV limits how far the view widens; z keeps 2D (HUD, menus) at 16:9; w is 0
    // for draws that sample a resolved render target (screen layers, never squeezed).
    float4 vwide;
};
cbuffer PSConstants : register(b1)
{
    float4 pc[256];
    uint4 pb[2];
    uint4 pl[8];
    uint4 pinfo;     // x alpha test function (7 = always), y generated-parameter register (0xFF none)
    float4 pinfo2;    // x alpha reference, y/z 1 / render scale (pixel positions back to game pixels), w colour ceiling (0 none)
    uint4 tinfo[32];  // x sampler (0 lin wrap, 1 lin clamp, 2 point clamp, 3 point wrap), y/z size, w dimension (3 cube)
    float4 tbias[32]; // x fetch-constant LOD bias, y (as uint) bindless descriptor index
    float4 pforceLod; // x: forced absolute LOD (FM2_FORCE_LOD), -1 = implicit LOD
};
ByteAddressBuffer gmem : register(t0);
ByteAddressBuffer gphys : register(t1);   // guest physical memory (large vertex buffers, indices)
// Bindless textures: the fetch slot's heap index is in tbias[slot].y.
Texture2D<float4> gtex[] : register(t0, space1);
TextureCube<float4> gcube[] : register(t0, space2);
// The four guest samplers as one array (a descriptor table, renderer.cpp), indexed by the fetch's mode
// (0 linear wrap, 1 linear clamp, 2 point clamp, 3 point wrap): one sample instruction per fetch instead
// of a branch to each of four.
SamplerState gSamplers[4] : register(s0);
#define sLinearWrap gSamplers[0]
#define sLinearClamp gSamplers[1]
#define sPointClamp gSamplers[2]
#define sPointWrap gSamplers[3]

static const float FLT_MAX = 3.402823466e+38;

struct VSOut
{
    float4 pos : SV_Position;
    float4 o[16] : TEXCOORD0;
    float2 pcoord : TEXCOORD16; // point sprite coordinate
};

uint Swap(uint v, uint e)
{
    if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3) return (v << 16) | (v >> 16);
    return v;
}

uint LoadGuest(uint fetch, uint dword)
{
    uint a = vf[fetch].x + dword * 4;
    return Swap(vf[fetch].z ? gphys.Load(a) : gmem.Load(a), vf[fetch].y);
}

float SignExtend(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }

// Xenos 16-bit float: like IEEE half but exponent 31 is an ordinary exponent
// (no infinities or NaNs), so the range reaches 131008.
float XHalf(uint h)
{
    h &= 0xFFFF;
    if (((h >> 10) & 31) == 31)
    {
        float v = (1.0 + float(h & 1023) / 1024.0) * 65536.0;
        return (h & 0x8000) ? -v : v;
    }
    return f16tof32(h);
}

float4 Unpack(uint w0, uint w1, uint w2, uint w3, uint format, bool isSigned, bool norm)
{
    float4 r = float4(0, 0, 0, 1);
    switch (format)
    {
    case 6: // 8_8_8_8
    {
        uint4 u = uint4(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF, w0 >> 24);
        if (isSigned) { r = float4(SignExtend(u.x, 8), SignExtend(u.y, 8), SignExtend(u.z, 8), SignExtend(u.w, 8)); if (norm) r = max(r / 127.0, -1.0); }
        else { r = float4(u); if (norm) r /= 255.0; }
        break;
    }
    case 7: // 2_10_10_10
    {
        uint4 u = uint4(w0 & 0x3FF, (w0 >> 10) & 0x3FF, (w0 >> 20) & 0x3FF, w0 >> 30);
        if (isSigned) { r = float4(SignExtend(u.x, 10), SignExtend(u.y, 10), SignExtend(u.z, 10), SignExtend(u.w, 2)); if (norm) r = max(r / float4(511, 511, 511, 1), -1.0); }
        else { r = float4(u); if (norm) r /= float4(1023, 1023, 1023, 3); }
        break;
    }
    case 25: // 16_16
    case 26: // 16_16_16_16
    {
        uint4 u = uint4(w0 & 0xFFFF, w0 >> 16, w1 & 0xFFFF, w1 >> 16);
        if (isSigned) { r = float4(SignExtend(u.x, 16), SignExtend(u.y, 16), SignExtend(u.z, 16), SignExtend(u.w, 16)); if (norm) r = max(r / 32767.0, -1.0); }
        else { r = float4(u); if (norm) r /= 65535.0; }
        if (format == 25) r.zw = float2(0, 1);
        break;
    }
    case 31: r = float4(XHalf(w0), XHalf(w0 >> 16), 0, 1); break;
    case 32: r = float4(XHalf(w0), XHalf(w0 >> 16), XHalf(w1), XHalf(w1 >> 16)); break;
    case 33: r = float4(isSigned ? float(int(w0)) : float(w0), 0, 0, 1); break;
    case 34: r = float4(isSigned ? float2(int2(w0, w1)) : float2(w0, w1), 0, 1); break;
    case 35: r = isSigned ? float4(int4(w0, w1, w2, w3)) : float4(w0, w1, w2, w3); break;
    case 36: r = float4(asfloat(w0), 0, 0, 1); break;
    case 37: r = float4(asfloat(w0), asfloat(w1), 0, 1); break;
    case 57: r = float4(asfloat(w0), asfloat(w1), asfloat(w2), 1); break;
    case 38: r = asfloat(uint4(w0, w1, w2, w3)); break;
    case 16: // 10_11_11: X 11 bits, Y 11 bits, Z 10 bits from the bottom
    case 17: // 11_11_10: X 10 bits, Y 11 bits, Z 11 bits
    {
        uint3 bits = format == 16 ? uint3(11, 11, 10) : uint3(10, 11, 11);
        uint3 u = uint3(w0 & ((1u << bits.x) - 1), (w0 >> bits.x) & ((1u << bits.y) - 1), w0 >> (bits.x + bits.y));
        if (isSigned)
        {
            r.xyz = float3(SignExtend(u.x, bits.x), SignExtend(u.y, bits.y), SignExtend(u.z, bits.z));
            if (norm) r.xyz = max(r.xyz / float3((1u << (bits - 1)) - 1), -1.0);
        }
        else
        {
            r.xyz = float3(u);
            if (norm) r.xyz /= float3((1u << bits) - 1);
        }
        break;
    }
    }
    return r;
}

float4 VFetch(uint fetch, float index, uint stride, uint offset, uint format, bool isSigned, bool norm)
{
    uint d = uint(max(index, 0.0)) * stride + offset; // index already rounded or floored
    return Unpack(LoadGuest(fetch, d), LoadGuest(fetch, d + 1), LoadGuest(fetch, d + 2), LoadGuest(fetch, d + 3), format, isSigned, norm);
}

// The title binds each vertex shader to the vertex layout it draws by patching its fetch
// instructions (constant slot, format, stride, offset, swizzle). Reading those fields from
// vfl instead of baking them in makes every patched copy of a shader the same HLSL, so they
// share one compile.
float4 VFetchK(uint k, float i)
{
    uint4 p = vfl[k];
    float idx = (p.x & 0x10000u) ? round(i) : floor(i);
    return VFetch(p.x & 0x7Fu, idx, p.y, p.z, (p.x >> 8) & 0x3Fu, (p.x & 0x4000u) != 0, (p.x & 0x8000u) != 0) * exp2(float(int(p.w << 10) >> 26));
}

float4 VMerge(uint k, float4 old, float4 t)
{
    uint sw = vfl[k].w & 0xFFFu;
    float4 r = old;
    [unroll] for (uint c = 0; c < 4; c++)
    {
        uint s = (sw >> (3 * c)) & 7u;
        r[c] = s < 4 ? t[s] : (s == 4 ? 0.0 : (s == 5 ? 1.0 : old[c]));
    }
    return r;
}

// Cube fetch coordinates are (s, t, face) as produced by the CUBE instruction
// plus the usual 1.5 bias: s, t in [1, 2]. Back to a direction for the host.
float3 CubeDirection(float3 stf)
{
    float sc = (stf.x - 1.5) * 2.0, tc = (stf.y - 1.5) * 2.0;
    uint face = uint(round(stf.z));
    switch (face)
    {
    case 0: return float3(1, -tc, -sc);
    case 1: return float3(-1, -tc, sc);
    case 2: return float3(sc, 1, tc);
    case 3: return float3(sc, -1, -tc);
    case 4: return float3(sc, -tc, 1);
    default: return float3(-sc, -tc, -1);
    }
}

float4 TFetchRaw(uint slot, float3 coord, bool denorm, bool vertexStage, float bias, float2 texel, uint filt);

// Sampling with a LOD bias is a pixel-shader-only operation: a vertex shader cannot compute an
// implicit LOD, and dxc rejects the module outright ("sampling with implicit lod is only allowed
// in fragment and compute shaders"), where fxc merely ignored the dead branch. The translator
// emits the same source for both stages, so the stage picks the form here. A macro, not a
// function: a call in the fetch path keeps fxc from inlining and costs half the frame rate.
#ifdef FM2_VERTEX_STAGE
#define FM2_SAMPLE_BIAS_2D(t, s, uv, bias) ((t).SampleLevel((s), (uv), max((bias), 0)))
#define FM2_SAMPLE_BIAS_CUBE(t, s, dir, bias) ((t).SampleLevel((s), (dir), max((bias), 0)))
#else
#define FM2_SAMPLE_BIAS_2D(t, s, uv, bias) ((t).SampleBias((s), (uv), (bias)))
#define FM2_SAMPLE_BIAS_CUBE(t, s, dir, bias) ((t).SampleBias((s), (dir), (bias)))
#endif

// filt: the fetch instruction's filter (0 point, 1 linear), or 3 for the fetch constant's.
float4 TFetch(uint slot, float3 coord, bool denorm, bool vertexStage, float bias, float2 texel, uint filt)
{
    float4 v = TFetchRaw(slot, coord, denorm, vertexStage, bias, texel, filt);
    // Exponent adjust (fetch constant): texels scaled by 2^n, n in tinfo.x bits 8-13.
    int expAdjust = int(tinfo[slot].x << 18) >> 26;
    if (expAdjust != 0) v *= exp2(float(expAdjust));
    uint w = tinfo[slot].w;
    if (!(w >> 31))
        return v;
    float4 r;
    [unroll] for (uint c = 0; c < 4; c++)
    {
        uint sel = (w >> (8 + 3 * c)) & 7u;
        r[c] = sel < 4u ? v[sel] : (sel == 5u ? 1.0 : 0.0);
    }
    return r;
}

float4 TFetchRaw(uint slot, float3 coord, bool denorm, bool vertexStage, float bias, float2 texel, uint filt)
{
    uint index = asuint(tbias[slot].y);
    Texture2D<float4> t = gtex[index];
    TextureCube<float4> tc = gcube[index];
    if ((tinfo[slot].w & 0xFFu) == 3)
    {
        float3 dir = CubeDirection(coord);
        return vertexStage ? tc.SampleLevel(sLinearClamp, dir, max(bias, 0)) : FM2_SAMPLE_BIAS_CUBE(tc, sLinearClamp, dir, bias);
    }
    float2 uv = coord.xy;
    const float2 texelSize = float2(max(tinfo[slot].yz & 0xFFFFu, uint2(1, 1)));
    if (denorm) uv = (uv + texel) / texelSize;
    else uv += texel / texelSize;
    // Mirrored (tinfo.x bits 20 U, 21 V: repeating; 22 U, 23 V: once, then clamped below).
    if ((tinfo[slot].x >> 20) & 1u) uv.x = 1.0 - abs(frac(uv.x * 0.5) * 2.0 - 1.0);
    if ((tinfo[slot].x >> 21) & 1u) uv.y = 1.0 - abs(frac(uv.y * 0.5) * 2.0 - 1.0);
    if ((tinfo[slot].x >> 22) & 1u) uv.x = abs(uv.x);
    if ((tinfo[slot].x >> 23) & 1u) uv.y = abs(uv.y);
    // Clamp to border (tinfo.x bit 16; bit 17 white): outside the image the border colour, as
    // shadow maps rely on (white: nothing casts a shadow beyond the map).
    if (((tinfo[slot].x >> 16) & 1u) && any(uv < 0.0 || uv > 1.0))
        return ((tinfo[slot].x >> 17) & 1u) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    // One axis clamped, the other wrapping (tinfo.x bits 18 U, 19 V): clamp that axis here to
    // the edge texels' centres; the sampler wraps.
    if ((tinfo[slot].x >> 18) & 1u) uv.x = clamp(uv.x, 0.5 / texelSize.x, 1.0 - 0.5 / texelSize.x);
    if ((tinfo[slot].x >> 19) & 1u) uv.y = clamp(uv.y, 0.5 / texelSize.y, 1.0 - 0.5 / texelSize.y);
    if (tbias[slot].z > 0.0) uv *= tbias[slot].zw; // declared image inside a wider resolved texture (renderer.cpp)
    uint mode = tinfo[slot].x & 3u;
    // The instruction's filter overrides the fetch constant's (as on the hardware and in Xenia); clamp/wrap
    // stay the constant's. The showroom's shadow blur takes linear taps at half-texel offsets from a point
    // constant: point-sampled they landed on texel edges, the blur came out narrow, and the floor's variance
    // shadow broke into shards.
    if ((filt & 3u) <= 1u)
    {
        const bool clampMode = mode == 1u || mode == 2u;
        mode = (filt & 3u) == 0u ? (clampMode ? 2u : 3u) : (clampMode ? 1u : 0u);
    }
    if (vertexStage)
        return t.SampleLevel(sLinearClamp, uv, max(bias, 0));
    // FM2_FORCE_LOD: sample an absolute mip level instead of the implicit one. Diagnostic:
    // separates "mips never selected" (this fixes the distance noise) from "mips never
    // uploaded" (this stays noisy / reads the clamped tail).
    // filt bit 2: an explicit LOD in bias (setTextureLOD with the computed LOD off).
    if (pforceLod.x >= 0.0 || (filt & 4u))
    {
        const float lod = clamp((filt & 4u) ? bias : pforceLod.x, 0.0, 16.0);
        return t.SampleLevel(gSamplers[mode], uv, lod);
    }
    return FM2_SAMPLE_BIAS_2D(t, gSamplers[mode], uv, bias);
}

float4 CubeOp(float4 a, float4 b)
{
    float3 v = float3(a.z, a.w, a.x);
    float3 m = abs(v);
    float sc, tc, ma, face;
    if (m.z >= m.x && m.z >= m.y) { ma = v.z; face = v.z < 0 ? 5 : 4; sc = v.z < 0 ? -v.x : v.x; tc = -v.y; }
    else if (m.y >= m.x) { ma = v.y; face = v.y < 0 ? 3 : 2; sc = v.x; tc = v.y < 0 ? -v.z : v.z; }
    else { ma = v.x; face = v.x < 0 ? 1 : 0; sc = v.x < 0 ? v.z : -v.z; tc = -v.y; }
    return float4(tc, sc, 2.0 * abs(ma), face);
}

// Direct3D 9 multiplication, as the Xenos ALU does it: zero times anything (even
// infinity or NaN) is zero. Shaders rely on it to switch terms off by weight.
float4 Mul9(float4 a, float4 b) { return (a == 0 || b == 0) ? 0 : a * b; }
float Mul9(float a, float b) { return (a == 0 || b == 0) ? 0 : a * b; }
float Dot9(float4 a, float4 b) { float4 m = Mul9(a, b); return m.x + m.y + m.z + m.w; }

bool BoolConstant(uint4 b[2], uint index) { return (b[index >> 7][(index >> 5) & 3] >> (index & 31)) & 1; }
)";

struct Emitter
{
    const Shader& shader;
    bool pixel;
    std::string out;
    std::string error;
    TranslatedShader& result;
    bool layoutOnly = false;   // only collect the vertex fetch layout (no HLSL)
    bool literalFetch = false; // a fetch had to be baked into the code
    uint32_t lastStride = 0;
    uint32_t lastSrc = 0, lastSrcSwizzle = 0; // mini fetches reuse the full fetch's index
    uint32_t lastConst = 0; // and its fetch constant (a mini's own constant field is unused)
    bool saveFetchIndex = false; // minis read the index their full fetch kept (vfi), see ScanFetchIndex
    bool lastRound = false;
    int indent = 1;

    void Line(const std::string& s)
    {
        out.append(size_t(indent) * 4, ' ');
        out += s;
        out += '\n';
    }

    std::string Constant(uint32_t index, uint32_t relative = 0) const
    {
        result.constantsUsed = relative ? 256 : std::max(result.constantsUsed, (index & 255) + 1);
        if (relative)
            result.constantMask.fill(~0ull);
        else
            result.constantMask[(index & 255) >> 6] |= 1ull << (index & 63);
        char b[48];
        if (relative)
            snprintf(b, sizeof(b), "%s[(%u + %s) & 255]", pixel ? "pc" : "vc", index & 255, relative == 1 ? "a0" : "aL");
        else
            snprintf(b, sizeof(b), "%s[%u]", pixel ? "pc" : "vc", index & 255);
        return b;
    }

    std::string Src(const Source& s) const
    {
        static const char comp[] = "xyzw";
        std::string base = s.isConstant ? Constant(s.reg, s.relative) : "r[" + std::to_string(s.reg) + "]";
        if (s.abs)
            base = "abs(" + base + ")";
        std::string swz = ".";
        for (int c = 0; c < 4; c++)
            swz += comp[s.swizzle[c]];
        return std::string(s.negate ? "(-" : "(") + base + swz + ")";
    }

    // Export register to an HLSL lvalue, or empty if the write is ignored.
    std::string ExportTarget(uint32_t reg) const
    {
        if (pixel)
            return reg < 4 ? "oC[" + std::to_string(reg) + "]" : "";
        if (reg == 63)
            return "opts";
        if (reg == 62)
            return "opos";
        if (reg < 16)
        {
            result.interpolatorsWritten |= 1u << reg;
            return "o[" + std::to_string(reg) + "]";
        }
        return "";
    }

    static std::string Masked(const std::string& target, const std::string& value, uint32_t mask)
    {
        // One swizzled write for the components in order (r.xyz = v.xyz), unless FM2_FULL_ALU.
        if (!FullAlu() && std::popcount(mask & 15u) > 1) // (one component keeps its form: ApplyDirtSeamFix finds the car body by it)
        {
            std::string comps;
            for (int c = 0; c < 4; c++)
                if ((mask >> c) & 1)
                    comps += "xyzw"[c];
            return target + "." + comps + " = " + value + "." + comps + "; ";
        }
        std::string s;
        for (int c = 0; c < 4; c++)
        {
            if ((mask >> c) & 1)
            {
                const char comp = "xyzw"[c];
                s += target + "." + comp + " = " + value + "." + comp + "; ";
            }
        }
        return s;
    }

    std::string VectorExpr(const Alu& a, const std::string& A, const std::string& B, const std::string& C)
    {
        switch (a.vectorOp)
        {
        case 0: return A + " + " + B;
        case 1: return "Mul9(" + A + ", " + B + ")";
        case 2: return "max(" + A + ", " + B + ")";
        case 3: return "min(" + A + ", " + B + ")";
        case 4: return "float4(" + A + " == " + B + ")";
        case 5: return "float4(" + A + " > " + B + ")";
        case 6: return "float4(" + A + " >= " + B + ")";
        case 7: return "float4(" + A + " != " + B + ")";
        case 8: return "frac(" + A + ")";
        case 9: return "trunc(" + A + ")";
        case 10: return "floor(" + A + ")";
        case 11: return "Mul9(" + A + ", " + B + ") + " + C;
        case 12: return "(" + A + " == 0.0) ? " + B + " : " + C;
        case 13: return "(" + A + " >= 0.0) ? " + B + " : " + C;
        case 14: return "(" + A + " > 0.0) ? " + B + " : " + C;
        case 15: return "Dot9(" + A + ", " + B + ").xxxx";
        case 16: return "Dot9(float4((" + A + ").xyz, 0), float4((" + B + ").xyz, 0)).xxxx";
        case 17: return "(Dot9(float4((" + A + ").xy, 0, 0), float4((" + B + ").xy, 0, 0)) + (" + C + ").x).xxxx";
        case 18: // CUBE: operands are v.zzxy and v.yxzz; result (tc, sc, 2 * major axis, face)
            return "CubeOp(" + A + ", " + B + ")";
        case 19: { std::string t = A; return "max(max(" + t + ".x, " + t + ".y), max(" + t + ".z, " + t + ".w)).xxxx"; }
        case 28: return "float4(1.0, (" + A + ").y * (" + B + ").y, (" + A + ").z, (" + B + ").w)";
        case 29: return "max(" + A + ", " + B + ")"; // MAXAv: a0 is set by the caller
        default: return "float4(0, 0, 0, 0)";
        }
    }

    // Emits code computing the scalar result into `s`; may update p.
    std::string ScalarCode(const Alu& a)
    {
        const std::string sv = Src(a.src[2]);
        const std::string A = sv + ".w", B = sv + ".x";
        auto set = [](const std::string& e) { return "s = " + e + ";"; };
        switch (a.scalarOp)
        {
        case 0: return set(A + " + " + B);
        case 1: return set(A + " + ps");
        case 2: return set("Mul9(" + A + ", " + B + ")");
        case 3: case 4: return set("Mul9(" + A + ", ps)");
        case 5: return set("max(" + A + ", " + B + ")");
        case 6: return set("min(" + A + ", " + B + ")");
        case 7: return set("float(" + A + " == 0.0)");
        case 8: return set("float(" + A + " > 0.0)");
        case 9: return set("float(" + A + " >= 0.0)");
        case 10: return set("float(" + A + " != 0.0)");
        case 11: return set("frac(" + A + ")");
        case 12: return set("trunc(" + A + ")");
        case 13: return set("floor(" + A + ")");
        case 14: return set("exp2(" + A + ")");
        case 15: return "s = log2(" + A + "); if (isinf(s)) s = -FLT_MAX;";
        case 16: return set("log2(" + A + ")");
        case 17: return "s = 1.0 / " + A + "; s = clamp(s, -FLT_MAX, FLT_MAX);";
        case 18: return "s = 1.0 / " + A + "; if (isinf(s)) s = 0.0;";
        case 19: return set("1.0 / " + A);
        case 20: return "s = rsqrt(" + A + "); s = clamp(s, -FLT_MAX, FLT_MAX);";
        case 21: return "s = rsqrt(" + A + "); if (isinf(s)) s = 0.0;";
        case 22: return set("rsqrt(" + A + ")");
        // MAXAs / MAXAsf: also load the address register.
        case 23: return set("max(" + A + ", " + B + ")") + " a0 = clamp(int(floor(" + A + " + 0.5)), -256, 255);";
        case 24: return set("max(" + A + ", " + B + ")") + " a0 = clamp(int(floor(" + A + ")), -256, 255);";
        case 25: return set(A + " - " + B);
        case 26: return set(A + " - ps");
        case 27: return "p = (" + A + " == 0.0); s = p ? 0.0 : 1.0;";
        case 28: return "p = (" + A + " != 0.0); s = p ? 0.0 : 1.0;";
        case 29: return "p = (" + A + " > 0.0); s = p ? 0.0 : 1.0;";
        case 30: return "p = (" + A + " >= 0.0); s = p ? 0.0 : 1.0;";
        case 31: return "p = (" + A + " == 0.0); s = p ? 0.0 : ((" + A + " == 1.0) ? 0.0 : " + A + ");";
        case 32: return "p = (" + A + " - 1.0 <= 0.0); s = p ? 0.0 : " + A + " - 1.0;";
        case 33: return "p = false; s = FLT_MAX;";
        case 34: return "p = (" + A + " == 0.0); s = " + A + ";";
        case 35: case 36: case 37: case 38: case 39:
        {
            static const char* cmp[] = { "== 0.0", "> 0.0", ">= 0.0", "!= 0.0", "== 1.0" };
            const std::string c = A + " " + cmp[a.scalarOp - 35];
            return std::string(pixel ? "if (" + c + ") discard; " : "") + "s = float(" + c + ");";
        }
        case 40: return set("sqrt(" + A + ")");
        case 42: case 43: case 44: case 45: case 46: case 47:
        {
            // Constant operand (component W of src3's swizzle) with a temp
            // whose index is spread over the opcode and swizzle bits.
            const uint32_t temp = (a.scalarOp & 1) | (uint32_t(a.src3IsTemp) << 1) | (a.src3RawSwizzle & 0x3C);
            static const char comp[] = "xyzw";
            const std::string ca = Constant(a.src3RawReg) + "." + comp[((a.src3RawSwizzle >> 6) + 3) & 3];
            const std::string tb = "r[" + std::to_string(temp) + "]." + comp[a.src3RawSwizzle & 3];
            const char* op = a.scalarOp <= 43 ? " * " : (a.scalarOp <= 45 ? " + " : " - ");
            const char* neg = a.src[2].negate ? "-" : "";
            if (a.scalarOp <= 43)
                return set(std::string("Mul9(") + neg + ca + ", " + neg + tb + ")");
            static const int negMode = getenv("FM2_SC_NEG") ? atoi(getenv("FM2_SC_NEG")) : 0; // 0 both, 1 constant, 2 temp
            const char* negC = negMode == 2 ? "" : neg;
            const char* negT = negMode == 1 ? "" : neg;
            return set(std::string("(") + negC + ca + ")" + op + "(" + negT + tb + ")");
        }
        case 48: return set("sin(" + A + ")");
        case 49: return set("cos(" + A + ")");
        case 50: return "s = ps;";
        default:
            error = "scalar op " + std::to_string(a.scalarOp);
            return "s = 0.0;";
        }
    }

    // FM2_FULL_ALU=1: every instruction computes both halves through v and s as before (A/B).
    static bool FullAlu()
    {
        static const bool full = getenv("FM2_FULL_ALU") != nullptr;
        return full;
    }

    void EmitAlu(const Alu& a)
    {
        // Leaner code for the common cases, the same results: the scalar half "retain previous" (op 50,
        // unclamped) leaves ps as it is, and a vector result nothing reads (no component written, no
        // predicate, kill or address side effect) isn't computed. Compiled without optimisation, all of
        // it reached the GPU's compiler as written.
        const bool lean = !FullAlu();
        const bool scalarKept = lean && a.scalarOp == 50 && !a.scalarClamp && !a.scalarMask;
        const bool vectorSideEffect = (a.vectorOp >= 20 && a.vectorOp <= 27) || a.vectorOp == 29;
        const bool vectorUnused = lean && !a.vectorMask && !vectorSideEffect &&
            !(a.exportData && a.scalarDestRelative); // (a relative export zeroes the unwritten components)
        if (scalarKept && vectorUnused && !a.exportData)
            return; // a no-op
        std::string body = std::string("{ ") + (vectorUnused ? "" : "float4 v = float4(0, 0, 0, 0); ") + (scalarKept ? "" : "float s = ps; ");
        const std::string A = Src(a.src[0]), B = Src(a.src[1]), C = Src(a.src[2]);
        if (vectorUnused)
            ;
        else if (a.vectorOp >= 20 && a.vectorOp <= 23)
        {
            static const char* cmp[] = { "== 0.0", "!= 0.0", "> 0.0", ">= 0.0" };
            body += "{ bool c = (" + A + ".w " + cmp[a.vectorOp - 20] + ") && (" + B + ".w == 0.0); p = c; v = c ? 0.0 : (" + A +
                ".x + 1.0).xxxx; } ";
        }
        else if (a.vectorOp >= 24 && a.vectorOp <= 27)
        {
            static const char* cmp[] = { "==", ">", ">=", "!=" };
            body += "{ bool k = any(" + A + " " + cmp[a.vectorOp - 24] + " " + B + "); v = float(k).xxxx; ";
            if (pixel)
                body += "if (k) discard; ";
            body += "} ";
        }
        else
            body += "v = " + VectorExpr(a, A, B, C) + "; ";
        if (a.vectorClamp && !vectorUnused)
            body += "v = saturate(v); ";
        if (!scalarKept)
            body += ScalarCode(a) + " ";
        if (a.vectorOp == 29) // MAXAv: a0 from the first operand's W, after the reads
            body += "a0 = clamp(int(floor((" + A + ").w + 0.5)), -256, 255); ";
        if (a.scalarClamp)
            body += "s = saturate(s); ";
        if (!scalarKept)
            body += "ps = s; ";

        if (a.exportData)
        {
            const std::string target = ExportTarget(a.vectorDest);
            if (!target.empty())
            {
                const uint32_t vm = a.vectorMask & ~a.scalarMask, sm = a.scalarMask & ~a.vectorMask;
                const uint32_t one = a.vectorMask & a.scalarMask;
                const uint32_t zero = a.scalarDestRelative ? (~(a.vectorMask | a.scalarMask) & 15) : 0;
                body += Masked(target, "v", vm) + Masked(target, "s.xxxx", sm) + Masked(target, "float4(1, 1, 1, 1)", one) +
                    Masked(target, "float4(0, 0, 0, 0)", zero);
            }
        }
        else
        {
            body += Masked("r[" + std::to_string(a.vectorDest) + "]", "v", a.vectorMask);
            body += Masked("r[" + std::to_string(a.scalarDest) + "]", "s.xxxx", a.scalarMask);
        }
        body += "}";
        if (a.predicated)
            body = std::string("if (") + (a.predCondition ? "p" : "!p") + ") " + body;
        Line(body);
    }

    static std::string DestSwizzleWrite(uint32_t dest, uint32_t swizzle, const std::string& value)
    {
        std::string s;
        const std::string target = "r[" + std::to_string(dest) + "]";
        for (int c = 0; c < 4; c++)
        {
            const uint32_t sel = (swizzle >> (3 * c)) & 7;
            const char comp = "xyzw"[c];
            if (sel < 4)
                s += target + "." + comp + " = " + value + "." + "xyzw"[sel] + "; ";
            else if (sel == 4)
                s += target + "." + comp + " = 0.0; ";
            else if (sel == 5)
                s += target + "." + comp + " = 1.0; ";
        }
        return s;
    }

    void EmitFetch(const Fetch& f)
    {
        std::string body;
        if (f.opcode == 0)
        {
            if (pixel)
            {
                error = "vertex fetch in a pixel shader";
                return;
            }
            if (!f.isMini)
            {
                lastConst = f.constIndex;
                lastStride = f.stride;
                lastSrc = f.src;
                lastSrcSwizzle = f.srcSwizzle;
                lastRound = f.roundIndex;
            }
            if (getenv("FM2_TRACE_VFETCH"))
                fprintf(stderr, "vfetch: fmt %u signed %d norm %d exp %d stride %u offset %u mini %d round %d\n", f.format, f.isSigned, f.isNormalized,
                    f.expAdjust, lastStride, f.offset, f.isMini, lastRound);
            const uint32_t k = uint32_t(result.vertexFetchLayout.size());
            // A mini fetch (vfetch_mini) reads the vertex its full fetch addressed, through the full fetch's
            // constant and index: a mini carries only its format, offset and destination (FM4 follows a vf94
            // table fetch with a mini whose own constant field says vf95: every car drawn damaged). From the
            // FM4 port. FM2_NO_MINI_CONST=1 takes the mini's own field again (A/B; read at translation).
            static const bool miniOwnConst = getenv("FM2_NO_MINI_CONST") != nullptr;
            const uint32_t constIndex = f.isMini && !miniOwnConst ? lastConst : f.constIndex;
            // The index register, or (ScanFetchIndex) the value the full fetch kept before overwriting it.
            const std::string indexReg = "r[" + std::to_string(lastSrc) + "]." + "xyzw"[lastSrcSwizzle & 3];
            const std::string indexSrc = saveFetchIndex ? "vfi" : indexReg;
            const std::string keep = saveFetchIndex && !f.isMini ? "vfi = " + indexReg + "; " : "";
            const bool dynamic = !BakedVertexFetch() && k < 32 && (f.isMini || (f.src == lastSrc && f.srcSwizzle == lastSrcSwizzle));
            literalFetch = literalFetch || !dynamic;
            if (layoutOnly)
            {
                if (dynamic)
                    result.vertexFetchLayout.push_back({ constIndex | (f.format << 8) | (f.isSigned ? 0x4000u : 0) | (f.isNormalized ? 0x8000u : 0) |
                            (lastRound ? 0x10000u : 0),
                        lastStride, f.offset, f.destSwizzle | ((uint32_t(f.expAdjust) & 0x3F) << 16) });
                if (std::find(result.vertexFetchConstants.begin(), result.vertexFetchConstants.end(), constIndex) ==
                    result.vertexFetchConstants.end())
                    result.vertexFetchConstants.push_back(constIndex);
                return;
            }
            if (dynamic)
            {
                // Layout from vfl[k]; only the registers stay in the code.
                result.vertexFetchLayout.push_back({ constIndex | (f.format << 8) | (f.isSigned ? 0x4000u : 0) | (f.isNormalized ? 0x8000u : 0) |
                        (lastRound ? 0x10000u : 0),
                    lastStride, f.offset, f.destSwizzle | ((uint32_t(f.expAdjust) & 0x3F) << 16) });
                const std::string dest = "r[" + std::to_string(f.dest) + "]";
                body = "{ " + keep + "float4 t = VFetchK(" + std::to_string(k) + ", " + indexSrc + "); " + dest + " = VMerge(" + std::to_string(k) + ", " + dest +
                    ", t); }";
            }
            else
            {
                char call[320];
                snprintf(call, sizeof(call), "VFetch(%u, %s(%s), %u, %u, %u, %s, %s) * %.9g", constIndex, lastRound ? "round" : "floor",
                    indexSrc.c_str(), lastStride, f.offset, f.format, f.isSigned ? "true" : "false",
                    f.isNormalized ? "true" : "false", std::ldexp(1.0, f.expAdjust));
                body = std::string("{ ") + keep + "float4 t = " + call + "; " + DestSwizzleWrite(f.dest, f.destSwizzle, "t") + "}";
            }
            bool known = false;
            for (uint32_t c : result.vertexFetchConstants)
                known = known || c == constIndex;
            if (!known)
                result.vertexFetchConstants.push_back(constIndex);
        }
        else if (f.opcode == 1)
        {
            static const char comp[] = "xyzw";
            const std::string reg = "r[" + std::to_string(f.src) + "].";
            // A 1D fetch (dimension 0) reads x only: its texture is bound one row high (texture.cpp),
            // sampled at that row's centre whatever the other components hold (from the FM4 port).
            static const bool no1d = getenv("FM2_NO_1D_TEXTURES") != nullptr;
            const std::string uv = f.dimension == 0 && !no1d ? "float3(" + reg + comp[f.srcSwizzle & 3] + ", 0.5, 0)"
                : "float3(" + reg + comp[f.srcSwizzle & 3] + ", " + reg + comp[(f.srcSwizzle >> 2) & 3] + ", " + reg +
                comp[(f.srcSwizzle >> 4) & 3] + ")";
            char call[320];
            // Filter override: the mag filter decides (the host samplers don't mix point and linear); 2 (base
            // map) and 3 leave the fetch constant's. FM2_NO_FETCH_FILTER=1 ignores the instruction (A/B).
            static LiveSwitch noFilterOverride("FM2_NO_FETCH_FILTER");
            uint32_t filt = noFilterOverride || f.magFilter > 1 ? 3 : f.magFilter;
            // setTextureLOD's register LOD: the LOD itself with the computed LOD off, else added to it.
            const char* regLod = f.useRegisterLod ? "regLod + " : "";
            if (f.useRegisterLod && !f.useComputedLod)
                filt |= 4;
            snprintf(call, sizeof(call), "TFetch(%u, %s, %s, %s, %s%.4f + tbias[%u].x, float2(%.1f, %.1f), %uu)", f.constIndex, uv.c_str(),
                f.denormalized ? "true" : "false", pixel ? "false" : "true", regLod, f.lodBias, f.constIndex, f.offsetX, f.offsetY, filt);
            // Declared, then assigned: fxc drops the value when the fetch is the initializer of a
            // declaration in some shaders ("internal error: invalid access of unbound variable",
            // the Suzuka grandstand pixel shader 0xcc2c1b5affd62985 among them).
            body = std::string("{ float4 t; t = ") + call + "; " + DestSwizzleWrite(f.dest, f.destSwizzle, "t") + "}";
            result.usedTextureSlots |= 1u << f.constIndex;
        }
        else if (f.opcode == 18 && pixel)
        {
            // getGradients: screen-space derivatives of two source components,
            // (ddx(a), ddy(a), ddx(b), ddy(b)). Vector-font shaders use it for
            // anti-aliased curve edges.
            static const char comp[] = "xyzw";
            const std::string a = "r[" + std::to_string(f.src) + "]." + comp[f.srcSwizzle & 3];
            const std::string b = "r[" + std::to_string(f.src) + "]." + comp[(f.srcSwizzle >> 2) & 3];
            body = "{ float4 t = float4(ddx(" + a + "), ddy(" + a + "), ddx(" + b + "), ddy(" + b + ")); " +
                DestSwizzleWrite(f.dest, f.destSwizzle, "t") + "}";
        }
        else if (f.opcode == 24)
        {
            // setTextureLOD: the LOD later fetches with "use register LOD" take (source component x).
            static const char comp[] = "xyzw";
            body = std::string("regLod = r[") + std::to_string(f.src) + "]." + comp[f.srcSwizzle & 3] + ";";
        }
        else
        {
            // FM2_TRACE_FETCHOPS=1: log each fetch opcode the translator doesn't handle, once.
            static const bool trace = getenv("FM2_TRACE_FETCHOPS") != nullptr;
            static std::atomic<uint32_t> seen{ 0 };
            if (trace && !(seen.fetch_or(1u << (f.opcode & 31)) & (1u << (f.opcode & 31))))
                fprintf(stderr, "fetchops: unhandled fetch opcode %u (%s shader)\n", f.opcode, pixel ? "pixel" : "vertex");
            body = DestSwizzleWrite(f.dest, f.destSwizzle, "float4(0, 0, 0, 0)"); // other texture queries
        }
        if (f.predicated)
            body = std::string("if (") + (f.predCondition ? "p" : "!p") + ") { " + body + " }";
        Line(body);
    }

    void EmitExec(const Cf& cf)
    {
        for (uint32_t k = 0; k < cf.count(); k++)
        {
            const uint32_t index = cf.address() + k;
            if ((index + 1) * 3 > shader.words.size())
            {
                error = "instruction out of range";
                return;
            }
            const uint32_t* w = &shader.words[index * 3];
            if (layoutOnly)
            {
                if ((cf.sequence() >> (2 * k)) & 1)
                    if (const Fetch f = DecodeFetch(w); f.opcode == 0)
                        EmitFetch(f);
                continue;
            }
            if ((cf.sequence() >> (2 * k)) & 1)
                EmitFetch(DecodeFetch(w));
            else
                EmitAlu(DecodeAlu(w));
        }
    }

    // Control flow as a program-counter state machine: one block per CF
    // instruction, blocks run in order within an iteration, jumps set pc.
    // A mini fetch reads the vertex its full fetch addressed: the hardware computes the address once,
    // at the full fetch. A full fetch that writes its own index register (FM4's tyre morph: vfetch r0,
    // r0, then a mini) left its minis reading the fetched value as the index. Where that happens the
    // full fetch keeps its index in vfi for the minis after it. From the FM4 port.
    // FM2_NO_MINI_INDEX=1 re-reads the register (A/B; read at translation).
    void ScanFetchIndex()
    {
        if (pixel || getenv("FM2_NO_MINI_INDEX"))
            return;
        bool clobbered = false;
        for (const Cf& cf : shader.cf)
        {
            if (!cf.IsExec())
                continue;
            for (uint32_t k = 0; k < cf.count(); k++)
            {
                const uint32_t index = cf.address() + k;
                if ((index + 1) * 3 > shader.words.size() || !((cf.sequence() >> (2 * k)) & 1))
                    continue;
                const Fetch f = DecodeFetch(&shader.words[index * 3]);
                if (f.opcode != 0)
                    continue;
                // The index component is overwritten when the destination swizzle writes it (0-3 a
                // fetched component, 4 zero, 5 one; 6 and 7 keep the register's value).
                if (!f.isMini)
                    clobbered = f.dest == f.src && ((f.destSwizzle >> (3 * (f.srcSwizzle & 3))) & 7) <= 5;
                else if (clobbered)
                    saveFetchIndex = true;
            }
        }
    }

    void EmitProgram()
    {
        ScanFetchIndex();
        {
            static const bool trace = getenv("FM2_TRACE_FETCHOPS") != nullptr;
            static std::atomic<bool> exportSeen{ false };
            if (trace)
                for (const Cf& cf : shader.cf)
                    if (cf.op() == CfOp::Alloc && cf.allocType() == 3 && !exportSeen.exchange(true))
                        fprintf(stderr, "fetchops: memory export alloc in a %s shader\n", pixel ? "pixel" : "vertex");
        }
        bool linear = true;
        bool loops = false;
        for (const Cf& cf : shader.cf)
        {
            linear = linear && cf.op() != CfOp::CondJmp && cf.op() != CfOp::LoopStart && cf.op() != CfOp::LoopEnd &&
                cf.op() != CfOp::CondCall && cf.op() != CfOp::Return;
            loops = loops || cf.op() == CfOp::LoopStart || cf.op() == CfOp::LoopEnd;
        }
        // Only forward jumps (no loops, calls or returns): the blocks run once, in order, each checking
        // cfpc, with no loop around them. The loop was a dynamic loop with a branch per block to the GPU's
        // compiler, every register live across its iterations, for code that is a straight line with
        // skips. FM2_CF_LOOP=1: the loop for these too (A/B).
        static const bool cfLoop = getenv("FM2_CF_LOOP") != nullptr;
        bool forward = !linear && !cfLoop;
        for (size_t i = 0; i < shader.cf.size() && forward; i++)
        {
            const Cf& cf = shader.cf[i];
            const CfOp op = cf.op();
            forward = op != CfOp::LoopStart && op != CfOp::LoopEnd && op != CfOp::CondCall && op != CfOp::Return &&
                (op != CfOp::CondJmp || (cf.lo & 0x1FFF) > i);
        }
        if (!linear)
        {
            Line("uint cfpc = 0;");
            // Loop and call state for the control flow below (the hardware allows 4 levels of
            // each). aL is the loop index used by relative constant addressing: a loop's
            // parameters live in one constant register (count, start, step).
            if (loops)
            {
                Line("int loopIter[4] = {0, 0, 0, 0}, loopCount[4] = {0, 0, 0, 0}, loopStart[4] = {0, 0, 0, 0},");
                Line("    loopStep[4] = {0, 0, 0, 0}, loopAt[4] = {0, 0, 0, 0}; int loopDepth = 0;");
            }
            if (!forward)
                Line("int callRet[4] = {0, 0, 0, 0}; int callDepth = 0;");
            // The guard is a safety net against a program that never ends; it must be larger
            // than the longest legitimate run (a 255-iteration loop over several instructions).
            if (!forward)
            {
                Line(std::string("[loop] for (uint guard = 0; guard < ") + (loops ? "65536" : "1024") + "u && cfpc != 0xFFFFu; guard++)");
                Line("{");
                indent++;
            }
        }
        // The state machine gets a block only for the CF instructions a program can reach from its start:
        // through fall-through (none after an end, an unconditional jump or a return) and jump, call and
        // loop targets, subroutines after the end included. The rest of the CF area is padding or data
        // the compiler left there. From the FM4 port. FM2_NO_REACHABLE_CF=1: every block up to the end,
        // as before (read at translation).
        std::vector<bool> reachable(shader.cf.size(), true);
        if (!linear && !getenv("FM2_NO_REACHABLE_CF"))
        {
            reachable.assign(shader.cf.size(), false);
            std::vector<size_t> pending{ 0 };
            while (!pending.empty())
            {
                const size_t i = pending.back();
                pending.pop_back();
                if (i >= reachable.size() || reachable[i])
                    continue;
                reachable[i] = true;
                const Cf& cf = shader.cf[i];
                const CfOp op = cf.op();
                if (op == CfOp::CondJmp || op == CfOp::CondCall || op == CfOp::LoopStart || op == CfOp::LoopEnd)
                    pending.push_back(cf.lo & 0x1FFF);
                const bool unconditionalJump = op == CfOp::CondJmp && ((cf.lo >> 13) & 1);
                if (!cf.IsEnd() && op != CfOp::Return && !unconditionalJump)
                    pending.push_back(i + 1);
            }
        }
        for (size_t i = 0; i < shader.cf.size(); i++)
        {
            const Cf& cf = shader.cf[i];
            if (!reachable[i])
                continue;
            const std::string next = cf.IsEnd() ? "0xFFFFu" : std::to_string(i + 1);
            if (!linear)
            {
                Line("if (cfpc == " + std::to_string(i) + "u)");
                Line("{");
                indent++;
            }
            switch (cf.op())
            {
            case CfOp::Nop:
            case CfOp::Alloc:
            case CfOp::MarkVsFetchDone:
                break;
            case CfOp::Exec:
            case CfOp::ExecEnd:
                EmitExec(cf);
                break;
            case CfOp::CondExec:
            case CfOp::CondExecEnd:
            case CfOp::CondExecPredClean:
            case CfOp::CondExecPredCleanEnd:
            {
                char cond[96];
                snprintf(cond, sizeof(cond), "if (BoolConstant(%s, %u) == %s)", pixel ? "pb" : "vb", (cf.hi >> 2) & 0xFF,
                    cf.condition() ? "true" : "false");
                Line(std::string(cond) + " {");
                indent++;
                EmitExec(cf);
                indent--;
                Line("}");
                break;
            }
            case CfOp::CondExecPred:
            case CfOp::CondExecPredEnd:
                Line(std::string("if (") + (cf.condition() ? "p" : "!p") + ") {");
                indent++;
                EmitExec(cf);
                indent--;
                Line("}");
                break;
            case CfOp::CondJmp:
            {
                const uint32_t target = cf.lo & 0x1FFF;
                std::string cond;
                if ((cf.lo >> 13) & 1)
                    cond = "true";
                else if ((cf.lo >> 14) & 1)
                    cond = cf.condition() ? "p" : "!p";
                else
                    cond = std::string("BoolConstant(") + (pixel ? "pb" : "vb") + ", " + std::to_string((cf.hi >> 2) & 0xFF) + ") == " +
                        (cf.condition() ? "true" : "false");
                Line("cfpc = (" + cond + ") ? " + std::to_string(target) + "u : " + next + "u;");
                if (!forward)
                    Line("continue;");
                break;
            }
            case CfOp::LoopStart:
            {
                // loop_start aL, i#: the loop's count/start/step are in loop constant i# (count 8 bits,
                // start 8 bits, step signed 8 bits), the loop registers from 0x4908 (vl/pl), not a float
                // constant.
                const uint32_t skipTo = cf.lo & 0x1FFF;
                const bool repeat = (cf.lo >> 13) & 1;
                const uint32_t loopId = (cf.lo >> 16) & 0x1F;
                const std::string params = std::string(pixel ? "pl" : "vl") + "[" + std::to_string(loopId >> 2) + "][" + std::to_string(loopId & 3) + "]";
                Line("if (loopDepth < 4)");
                Line("{");
                indent++;
                Line("const uint loopParams = " + params + ";");
                if (!repeat)
                    Line("loopIter[loopDepth] = 0;");
                Line("loopCount[loopDepth] = int(loopParams & 0xFFu);");
                Line("loopStart[loopDepth] = int((loopParams >> 8) & 0xFFu);");
                Line("loopStep[loopDepth] = int(loopParams << 16) >> 24;");
                Line("loopAt[loopDepth] = " + std::to_string(i) + "u;");
                Line("aL = loopIter[loopDepth] * loopStep[loopDepth] + loopStart[loopDepth];");
                Line("if (loopCount[loopDepth] <= 0) { cfpc = " + std::to_string(skipTo) + "u; continue; }");
                Line("loopDepth++;");
                indent--;
                Line("}");
                Line("cfpc = " + next + ";");
                Line("continue;");
                break;
            }
            case CfOp::LoopEnd:
            {
                // loop_end: the iteration is incremented before the count and predicate checks;
                // the address is the start of the loop body to go back to.
                const uint32_t body = cf.lo & 0x1FFF;
                const bool predicatedBreak = (cf.lo >> 21) & 1;
                Line("if (loopDepth > 0)");
                Line("{");
                indent++;
                Line("const int depth = loopDepth - 1;");
                Line("loopIter[depth] = loopIter[depth] + 1;");
                Line("bool done = loopIter[depth] >= loopCount[depth];");
                if (predicatedBreak)
                    Line(std::string("done = done || (p == ") + (cf.condition() ? "true" : "false") + ");");
                Line("if (!done)");
                Line("{");
                indent++;
                Line("aL = loopIter[depth] * loopStep[depth] + loopStart[depth];");
                Line("cfpc = " + std::to_string(body) + "u;");
                Line("continue;");
                indent--;
                Line("}");
                Line("loopDepth = depth;");
                Line("if (depth > 0) aL = loopIter[depth - 1] * loopStep[depth - 1] + loopStart[depth - 1];");
                indent--;
                Line("}");
                Line("cfpc = " + next + ";");
                Line("continue;");
                break;
            }
            case CfOp::CondCall:
            {
                const uint32_t target = cf.lo & 0x1FFF;
                std::string cond;
                if ((cf.lo >> 13) & 1)
                    cond = "true";
                else if ((cf.lo >> 14) & 1)
                    cond = cf.condition() ? "p" : "!p";
                else
                    cond = std::string("BoolConstant(") + (pixel ? "pb" : "vb") + ", " + std::to_string((cf.hi >> 2) & 0xFF) + ") == " +
                        (cf.condition() ? "true" : "false");
                Line("if (" + cond + " && callDepth < 4)");
                Line("{");
                indent++;
                Line("callRet[callDepth++] = " + std::to_string(i + 1) + "u;"); // (next is 0xFFFFu after an end block)
                Line("cfpc = " + std::to_string(target) + "u;");
                Line("continue;");
                indent--;
                Line("}");
                Line("cfpc = " + next + ";");
                Line("continue;");
                break;
            }
            case CfOp::Return:
                Line("if (callDepth > 0) { cfpc = callRet[--callDepth]; continue; }");
                Line("cfpc = " + next + ";");
                Line("continue;");
                break;
            default:
                error = std::string("control flow op ") + std::to_string(unsigned(cf.op())) + " not supported yet";
                return;
            }
            if (!error.empty())
                return;
            if (!linear)
            {
                if (cf.op() != CfOp::CondJmp)
                    Line("cfpc = " + next + (cf.IsEnd() ? ";" : "u;"));
                indent--;
                Line("}");
            }
            if (cf.IsEnd() && (linear || getenv("FM2_NO_REACHABLE_CF")))
                break;
        }
        if (!linear && !forward)
        {
            indent--;
            Line("}");
        }
    }
};
}

const char* HlslPreamble()
{
    return kPreamble;
}

bool BakedVertexFetch()
{
    static const bool baked = getenv("FM2_DYNAMIC_VFETCH") == nullptr;
    return baked;
}

bool TranslateVertexFetchLayout(const Shader& shader, TranslatedShader& out)
{
    TranslatedShader result;
    if (!shader.valid || shader.pixel)
        return false;
    Emitter e{ shader, shader.pixel, {}, {}, result };
    e.layoutOnly = true;
    // The same walk as EmitProgram: control flow in order, each exec once, up to the end. (A program
    // with subroutines runs code after the end: translated in full instead.)
    for (const Cf& cf : shader.cf)
        if (cf.op() == CfOp::CondCall)
            return false;
    for (const Cf& cf : shader.cf)
    {
        if (cf.IsExec())
            e.EmitExec(cf);
        if (!e.error.empty() || e.literalFetch)
            return false;
        if (cf.IsEnd())
            break;
    }
    out.vertexFetchLayout = std::move(result.vertexFetchLayout);
    out.vertexFetchConstants = std::move(result.vertexFetchConstants);
    return true;
}

TranslatedShader TranslateShader(const Shader& shader)
{
    TranslatedShader result;
    if (!shader.valid)
    {
        result.error = "no end of program";
        return result;
    }
    Emitter e{ shader, shader.pixel, {}, {}, result };
    e.EmitProgram();
    if (!e.error.empty())
    {
        result.error = e.error;
        return result;
    }

    std::string h = kPreamble;
    if (!shader.pixel)
    {
        h += "void GuestVS(uint vindex, out float4 opos, out float4 o[16], out float4 opts)\n{\n"
             "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n"
             "    [unroll] for (int j = 0; j < 16; j++) o[j] = 0;\n"
             "    r[0].x = float(vindex); opos = float4(0, 0, 0, 1); opts = 0; float ps = 0; bool p = false; int a0 = 0, aL = 0; float regLod = 0;\n" +
             std::string(e.saveFetchIndex ? "    float vfi = 0; // the index of the last full vertex fetch, for its mini fetches\n" : "");
        h += e.out;
        h += "}\n"
             "void GuestVS(uint vindex, out float4 opos, out float4 o[16])\n{\n"
             "    float4 opts; GuestVS(vindex, opos, o, opts);\n}\n"
             "VSOut main(uint id : SV_VertexID)\n{\n"
             "    VSOut r;\n"
             "    r.pcoord = 0;\n"
             "    uint e;\n"
             "    if (vte.y & 1u)\n    {\n"
             "        // Direct indices (triangle lists and restart-free strips): read the\n"
             "        // guest index buffer in place. y: 1 direct, 2 strip, 4 32-bit, 8 auto, endian in bits 4-5.\n"
             "        uint i = id;\n"
             "        if (vte.y & 2u)\n        {\n"
             "            uint t = id / 3u, k = id % 3u;\n"
             "            i = t + ((t & 1u) ? (k == 0u ? 1u : (k == 1u ? 0u : 2u)) : k);\n"
             "        }\n"
             "        else if (vte.y & 64u)\n        {\n"
             "            // Quad list: two triangles (0 1 2, 0 2 3) per quad.\n"
             "            uint q = id / 6u, k = id % 6u;\n"
             "            i = q * 4u + (k < 3u ? k : (k == 3u ? 0u : k - 2u));\n"
             "        }\n"
             "        uint endian = (vte.y >> 4) & 3u;\n"
             "        if (vte.y & 8u)\n            e = i;\n"
             "        else if (vte.y & 4u)\n        {\n"
             "            e = gphys.Load(vte.z + i * 4u);\n"
             "            if (endian == 2u) e = (e >> 24) | ((e >> 8) & 0xFF00u) | ((e << 8) & 0xFF0000u) | (e << 24);\n"
             "            else if (endian == 1u) e = ((e & 0x00FF00FFu) << 8) | ((e >> 8) & 0x00FF00FFu);\n"
             "        }\n"
             "        else\n        {\n"
             "            uint a = vte.z + i * 2u;\n"
             "            uint w = gphys.Load(a & ~3u);\n"
             "            e = (a & 2u) ? (w >> 16) : (w & 0xFFFFu);\n"
             "            if (endian == 1u || endian == 2u) e = ((e & 0xFFu) << 8) | (e >> 8);\n"
             "        }\n"
             "        e += vte.w;\n"
             "    }\n"
             "    else\n"
             "        e = gmem.Load(vinfo.x + id * 4);\n"
             "    float2 pointOffset = 0;\n"
             "    // fxc inlines every GuestVS call; a large shader inlined once per path (point,\n"
             "    // three rectangle corners, plain) took 15 s to compile. Plain vertices keep their\n"
             "    // own call; points and rectangles share one in a loop.\n"
             "    const bool isPoint = (vinfo.y & 2u) != 0, rect = !isPoint && (e & 0x80000000u);\n"
             "    [branch] if (!isPoint && !rect)\n"
             "        GuestVS(e, r.pos, r.o);\n"
             "    else\n"
             "    {\n"
             "        uint vi[3];\n"
             "        vi[0] = isPoint ? (e & 0x1FFFFFFFu) : e; vi[1] = 0; vi[2] = 0;\n"
             "        if (rect)\n"
             "        {\n"
             "            uint b = vinfo.x + (e & 0x1FFFFFFFu) * 4;\n"
             "            vi[0] = gmem.Load(b); vi[1] = gmem.Load(b + 4); vi[2] = gmem.Load(b + 8);\n"
             "        }\n"
             "        float4 vp[3]; float4 vq[3][16]; float4 opts = 0;\n"
             "        [loop] for (uint k = 0; k < (rect ? 3u : 1u); k++)\n"
             "        {\n"
             "            float4 kp, ko[16], kpts;\n"
             "            GuestVS(vi[k], kp, ko, kpts);\n"
             "            vp[k] = kp; vq[k] = ko;\n"
             "            if (k == 0u) opts = kpts;\n"
             "        }\n"
             "        if (isPoint)\n"
             "        {\n"
             "            // Point sprite: corner in bits 29-30, expanded around the point in screen space.\n"
             "            uint corner = (e >> 29) & 3u;\n"
             "            r.pos = vp[0]; r.o = vq[0];\n"
             "            float2 d = opts.x > 0 ? opts.xx : vpoint.xy;\n"
             "            d = clamp(d, vpoint.zz, vpoint.ww);\n"
             "            float2 c = float2(corner & 1u, corner >> 1);\n"
             "            r.pcoord = c;\n"
             "            pointOffset = (c * 2.0 - 1.0) * d * 0.5;\n"
             "        }\n"
             "        else\n"
             "        {\n"
             "            // Rectangle second triangle: role 0 is the computed fourth corner, roles 1 and 2 the\n"
             "            // ends of the hypotenuse (the corners next to the right angle).\n"
             "            uint role = (e >> 29) & 3u;\n"
             "            float d01 = distance(vp[0].xy / vp[0].w, vp[1].xy / vp[1].w), d02 = distance(vp[0].xy / vp[0].w, vp[2].xy / vp[2].w);\n"
             "            float d12 = distance(vp[1].xy / vp[1].w, vp[2].xy / vp[2].w);\n"
             "            uint ra = (d12 >= d01 && d12 >= d02) ? 0u : ((d02 >= d01) ? 1u : 2u);\n"
             "            uint e1 = (ra + 1u) % 3u, e2 = (ra + 2u) % 3u;\n"
             "            if (role == 1u) { r.pos = vp[e1]; r.o = vq[e1]; }\n"
             "            else if (role == 2u) { r.pos = vp[e2]; r.o = vq[e2]; }\n"
             "            else { r.pos = vp[e1] + vp[e2] - vp[ra]; [unroll] for (int i = 0; i < 16; i++) r.o[i] = vq[e1][i] + vq[e2][i] - vq[ra][i]; }\n"
             "        }\n"
             "    }\n"
             "    float2 size = float2(vinfo.zw);\n"
             "    // The Xenos viewport transform (PA_CL_VTE_CNTL): optional divide by w, then\n"
             "    // per-axis scale and offset to screen pixels; back to D3D clip space after.\n"
             "    // Hor+: on a wider-than-16:9 output a full-width pass is drawn wider; perspective\n"
             "    // geometry is narrowed to match, so the view widens instead of stretching.\n"
             "    const bool flat = abs(r.pos.w - 1.0) <= 1e-4;\n"
             "    if (!flat) r.pos.xy *= vwide.xy;\n"
             "    float w = r.pos.w;\n"
             "    // (VTX_W0_FMT set means w is the true W0, the normal case.)\n"
             "    float3 p = r.pos.xyz;\n"
             "    if (!(vte.x & (1u << 8))) p.xy /= w;\n"
             "    if (!(vte.x & (1u << 9))) p.z /= w;\n"
             "    if (vte.x & 1u) p.x *= vscale.x;\n"
             "    if (vte.x & 2u) p.x += voffset.x;\n"
             "    if (vte.x & 4u) p.y *= vscale.y;\n"
             "    if (vte.x & 8u) p.y += voffset.y;\n"
             "    if (vte.x & 16u) p.z *= vscale.z;\n"
             "    if (vte.x & 32u) p.z += voffset.z;\n"
             "    p.z += vpoly.x;\n"
             "    // Positions the title gives in screen space (no viewport scale or offset: its\n"
             "    // rectangle clears and filter passes, authored for D3D9's integer pixel centres,\n"
             "    // e.g. -0.5 .. 255.5) must cover the last row and column too: shift by half a pixel.\n"
             "    if ((vte.x & 0x30Fu) == 0x300u) p.xy += 0.5;\n"
             "    else if (vte.x & 0x10000u) p.xy += 0.5;\n"
             "    // vscale.w/voffset.w: pixel scale of MSAA clear rectangles (renderer.cpp).\n"
             "    if (vscale.w > 0.0) p.xy *= float2(vscale.w, voffset.w);\n"
             "    p.xy += pointOffset;\n"
             "    if (vte.x & (1u << 8)) w = 1.0;\n"
             "    float2 ndc = float2(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0);\n"
             "    // Flat (2D) geometry in a wide pass keeps 16:9 proportions in a centred 16:9 area\n"
             "    // (HUD, menus, text); vertices on the left or right screen edge stay there, so\n"
             "    // full-screen layers (backgrounds, fades) still span the width. Screen-space passes\n"
             "    // (no viewport transform: the title's filter and post-processing passes) are left\n"
             "    // alone: squeezing their inner vertices warped the image outside 16:9.\n"
             "    if (flat && vwide.w > 0.5 && vwide.z < 1.0 && (vte.x & 0x30Fu) != 0x300u && abs(ndc.x) < 0.999) ndc.x *= vwide.z;\n"
             "    r.pos = float4(ndc * w, p.z * w, w);\n"
             "    return r;\n}\n";
    }
    else
    {
        h += "float4 main(VSOut input) : SV_Target0\n{\n"
             "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n"
             "    [unroll] for (int j = 0; j < 16; j++) r[j] = input.o[j];\n"
             "    float4 oC[4]; [unroll] for (int k = 0; k < 4; k++) oC[k] = 0;\n"
             "    float ps = 0; bool p = false; int a0 = 0, aL = 0; float regLod = 0;\n"
             "    // Generated parameter: pixel position, point sprite coordinate.\n"
             "    if (pinfo.y < 16u) r[pinfo.y] = float4(input.pos.xy * pinfo2.yz, input.pcoord);\n";
        h += e.out;
        h += "    switch (pinfo.x)\n    {\n"
             "    case 0: discard; break;\n"
             "    case 1: if (!(oC[0].a < pinfo2.x)) discard; break;\n"
             "    case 2: if (!(oC[0].a == pinfo2.x)) discard; break;\n"
             "    case 3: if (!(oC[0].a <= pinfo2.x)) discard; break;\n"
             "    case 4: if (!(oC[0].a > pinfo2.x)) discard; break;\n"
             "    case 5: if (!(oC[0].a != pinfo2.x)) discard; break;\n"
             "    case 6: if (!(oC[0].a >= pinfo2.x)) discard; break;\n"
             "    }\n"
             "    oC[0] *= exp2(float(int(pinfo.w))); // colour exponent bias (RB_COLOR_INFO), after the alpha test\n"
             "    if (pinfo.z & 8u) oC[0] = clamp(oC[0], -32.0, 32.0); // 16_16 fixed range\n"
             "    if (pinfo2.w > 0.0) oC[0].rgb = clamp(oC[0].rgb, 0.0, pinfo2.w); // 7e3 target range\n"
             "    return oC[0];\n}\n";
    }
    // The colour exports as separate variables, not an array (from the FM4 port): fxc declares a local
    // array as an indexable temp, and a driver can reorder writes to one ahead of an earlier read of
    // the same element (a predicated alpha-fade overwrote an export whose old value was still needed).
    // Every index is a constant here. FM2_COLOR_EXPORT_ARRAY=1 keeps the array (read at translation).
    static const bool exportArray = getenv("FM2_COLOR_EXPORT_ARRAY") != nullptr;
    if (shader.pixel && !exportArray)
    {
        const std::string declArray = "float4 oC[4]; [unroll] for (int k = 0; k < 4; k++) oC[k] = 0;";
        if (const size_t at = h.find(declArray); at != std::string::npos)
        {
            h.replace(at, declArray.size(), "float4 oC0 = 0, oC1 = 0, oC2 = 0, oC3 = 0;");
            for (char k = '0'; k <= '3'; k++)
                for (size_t p = 0; (p = h.find(std::string("oC[") + k + "]", p)) != std::string::npos;)
                    h.replace(p, 5, std::string("oC") + k);
            // The return keeps its form: debug edits and the motion-vector variant patch "return oC[0];"
            // (a one-element array written once and read once has nothing to reorder).
            const std::string ret = "    return oC0;\n}\n";
            if (const size_t r = h.rfind(ret); r != std::string::npos)
                h.replace(r, ret.size(), "    float4 oC[1] = { oC0 };\n    return oC[0];\n}\n");
        }
    }
    result.hlsl = h;
    result.ok = true;
    return result;
}

bool RegistersAsArray()
{
    static const bool registerArray = getenv("FM2_REGISTER_ARRAY") != nullptr;
    return registerArray;
}

std::string RegistersAsVariables(const std::string& hlsl)
{
    const std::string declRegisters = "float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n";
    const size_t declAt = RegistersAsArray() ? std::string::npos : hlsl.find(declRegisters);
    if (declAt == std::string::npos)
        return hlsl;
    // The guest function ends at its closing brace (the generated body is indented).
    const size_t start = declAt + declRegisters.size(), end = hlsl.find("\n}\n", start);
    if (end == std::string::npos)
        return hlsl;
    std::string body = hlsl.substr(start, end - start);
    const std::string inputs = "[unroll] for (int j = 0; j < 16; j++) r[j] = input.o[j];";
    if (const size_t at = body.find(inputs); at != std::string::npos)
    {
        std::string s;
        for (int j = 0; j < 16; j++)
            s += "r[" + std::to_string(j) + "] = input.o[" + std::to_string(j) + "]; ";
        body.replace(at, inputs.size(), s);
    }
    const std::string param = "if (pinfo.y < 16u) r[pinfo.y] = float4(input.pos.xy * pinfo2.yz, input.pcoord);";
    if (const size_t at = body.find(param); at != std::string::npos)
    {
        std::string s = "{ const float4 gp = float4(input.pos.xy * pinfo2.yz, input.pcoord); switch (pinfo.y) { ";
        for (int j = 0; j < 16; j++)
            s += "case " + std::to_string(j) + ": r[" + std::to_string(j) + "] = gp; break; ";
        body.replace(at, param.size(), s + "} }");
    }
    // r[N] -> rN (not inside a longer name such as loopIter[). Any other index keeps the array.
    std::string out;
    out.reserve(body.size());
    for (size_t i = 0; i < body.size();)
    {
        if (body[i] == 'r' && i + 1 < body.size() && body[i + 1] == '[' && (i == 0 || !(isalnum(uint8_t(body[i - 1])) || body[i - 1] == '_')))
        {
            size_t j = i + 2;
            while (j < body.size() && isdigit(uint8_t(body[j])))
                j++;
            if (j == i + 2 || j == body.size() || body[j] != ']')
                return hlsl;
            out += 'r';
            out.append(body, i + 2, j - i - 2);
            i = j + 1;
            continue;
        }
        out += body[i++];
    }
    std::string decl = "float4 r0 = 0";
    for (int i = 1; i < 64; i++)
        decl += ", r" + std::to_string(i) + " = 0";
    return hlsl.substr(0, declAt) + decl + ";\n" + out + hlsl.substr(end);
}
