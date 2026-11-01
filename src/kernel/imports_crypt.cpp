// XeCryptSha*: SHA-1 on a guest XECRYPT_SHA_STATE
// { u32 count, u32 state[5], u8 buffer[64] }.
// XeKeys*: the console-bound key functions (below).
#include "kernel/imports.h"
#include "log.h"
#include <string>

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace
{
uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t(block[i * 4]) << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

struct Sha
{
    uint32_t count = 0;
    uint32_t state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buffer[64] = {};

    void Load(uint32_t g)
    {
        count = LoadBE32(g);
        for (int i = 0; i < 5; i++)
            state[i] = LoadBE32(g + 4 + i * 4);
        memcpy(buffer, GuestPtr(g + 0x18), 64);
    }
    void Store(uint32_t g) const
    {
        StoreBE32(g, count);
        for (int i = 0; i < 5; i++)
            StoreBE32(g + 4 + i * 4, state[i]);
        memcpy(GuestPtr(g + 0x18), buffer, 64);
    }
    void Update(const uint8_t* p, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
        {
            buffer[count % 64] = p[i];
            if (++count % 64 == 0)
                Transform(state, buffer);
        }
    }
    void Final(uint8_t out[20])
    {
        const uint64_t bits = uint64_t(count) * 8;
        const uint8_t pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (count % 64 != 56)
            Update(&zero, 1);
        for (int i = 7; i >= 0; i--)
        {
            const uint8_t b = uint8_t(bits >> (i * 8));
            Update(&b, 1);
        }
        for (int i = 0; i < 20; i++)
            out[i] = uint8_t(state[i / 4] >> (24 - (i % 4) * 8));
    }
};
}

GUEST_EXPORT(XeCryptShaInit)
{
    Sha().Store(ctx.r3.u32);
}

GUEST_EXPORT(XeCryptShaUpdate)
{
    Sha s;
    s.Load(ctx.r3.u32);
    s.Update(GuestPtr<uint8_t>(ctx.r4.u32), ctx.r5.u32);
    s.Store(ctx.r3.u32);
}

GUEST_EXPORT(XeCryptShaFinal)
{
    // (state, out, outSize)
    Sha s;
    s.Load(ctx.r3.u32);
    uint8_t digest[20];
    s.Final(digest);
    memcpy(GuestPtr(ctx.r4.u32), digest, std::min<uint32_t>(ctx.r5.u32, 20));
}

GUEST_EXPORT(XeCryptSha)
{
    // (in1, len1, in2, len2, in3, len3, out, outSize)
    Sha s;
    const uint32_t in[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
    for (auto& p : in)
    {
        if (p[0] && p[1])
            s.Update(GuestPtr<uint8_t>(p[0]), p[1]);
    }
    uint8_t digest[20];
    s.Final(digest);
    memcpy(GuestPtr(ctx.r9.u32), digest, std::min<uint32_t>(ctx.r10.u32, 20));
}

// ---- XeCryptSha256/384/512 --------------------------------------------------------------
// XECRYPT_SHA256_STATE { u32 count, u32 state[8], u8 buffer[64] } (0x64 bytes);
// XECRYPT_SHA512_STATE { u32 count, (pad), u64 state[8], u8 buffer[128] } (0xC8 bytes), also for SHA-384.
// count is in bytes. FM4 hashes with them before it lets a signed-in profile start the game.
namespace
{
constexpr uint32_t kK256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

constexpr uint64_t kK512[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
    0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
    0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
    0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
    0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
    0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
    0x5fcb6fab3ad6faec, 0x6c44198c4a475817 };

template <typename T> T Ror(T v, int n) { return (v >> n) | (v << (sizeof(T) * 8 - n)); }

// One SHA-2 variant: Word is the state word (u32 for 256, u64 for 384/512).
template <typename Word> struct Sha2
{
    static constexpr uint32_t kBlock = sizeof(Word) * 16;
    uint32_t count = 0;
    Word state[8] = {};
    uint8_t buffer[kBlock] = {};

    static constexpr uint32_t StateOffset() { return sizeof(Word) == 4 ? 4 : 8; }
    void Load(uint32_t g)
    {
        count = LoadBE32(g);
        for (int i = 0; i < 8; i++)
            state[i] = sizeof(Word) == 4 ? Word(LoadBE32(g + StateOffset() + i * 4)) : Word(LoadBE64(g + StateOffset() + i * 8));
        memcpy(buffer, GuestPtr(g + StateOffset() + 8 * sizeof(Word)), kBlock);
    }
    void Store(uint32_t g) const
    {
        StoreBE32(g, count);
        for (int i = 0; i < 8; i++)
        {
            if constexpr (sizeof(Word) == 4)
                StoreBE32(g + StateOffset() + i * 4, state[i]);
            else
                StoreBE64(g + StateOffset() + i * 8, state[i]);
        }
        memcpy(GuestPtr(g + StateOffset() + 8 * sizeof(Word)), buffer, kBlock);
    }
    void Transform()
    {
        Word w[80];
        constexpr int rounds = sizeof(Word) == 4 ? 64 : 80;
        for (int i = 0; i < 16; i++)
        {
            Word v = 0;
            for (size_t b = 0; b < sizeof(Word); b++)
                v = (v << 8) | buffer[i * sizeof(Word) + b];
            w[i] = v;
        }
        for (int i = 16; i < rounds; i++)
        {
            Word s0, s1;
            if constexpr (sizeof(Word) == 4)
            {
                s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
                s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            }
            else
            {
                s0 = Ror(w[i - 15], 1) ^ Ror(w[i - 15], 8) ^ (w[i - 15] >> 7);
                s1 = Ror(w[i - 2], 19) ^ Ror(w[i - 2], 61) ^ (w[i - 2] >> 6);
            }
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        Word a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < rounds; i++)
        {
            Word S1, S0, k;
            if constexpr (sizeof(Word) == 4)
            {
                S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
                S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
                k = kK256[i];
            }
            else
            {
                S1 = Ror(e, 14) ^ Ror(e, 18) ^ Ror(e, 41);
                S0 = Ror(a, 28) ^ Ror(a, 34) ^ Ror(a, 39);
                k = Word(kK512[i]);
            }
            const Word t1 = h + S1 + ((e & f) ^ (~e & g)) + k + w[i];
            const Word t2 = S0 + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    void Update(const uint8_t* p, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
        {
            buffer[count % kBlock] = p[i];
            if (++count % kBlock == 0)
                Transform();
        }
    }
    // The digest: digestBytes of the state, big-endian (32 for SHA-256, 48 for 384, 64 for 512).
    void Final(uint8_t* out, uint32_t digestBytes)
    {
        const uint64_t bits = uint64_t(count) * 8;
        const uint8_t pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (count % kBlock != kBlock - 2 * sizeof(Word))
            Update(&zero, 1);
        for (size_t i = 0; i < 2 * sizeof(Word); i++) // the bit length, in a 64- or 128-bit field
        {
            const size_t shift = (2 * sizeof(Word) - 1 - i) * 8;
            const uint8_t b = shift < 64 ? uint8_t(bits >> shift) : 0;
            Update(&b, 1);
        }
        for (uint32_t i = 0; i < digestBytes; i++)
            out[i] = uint8_t(state[i / sizeof(Word)] >> ((sizeof(Word) - 1 - i % sizeof(Word)) * 8));
    }
};

constexpr uint32_t kSha256Init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
constexpr uint64_t kSha384Init[8] = { 0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
    0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4 };
constexpr uint64_t kSha512Init[8] = { 0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
    0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179 };

template <typename Word, size_t N> void Sha2Init(uint32_t g, const Word (&init)[N])
{
    Sha2<Word> s;
    memcpy(s.state, init, sizeof(s.state));
    s.Store(g);
}

template <typename Word> void Sha2Update(uint32_t g, uint32_t data, uint32_t size)
{
    Sha2<Word> s;
    s.Load(g);
    s.Update(GuestPtr<uint8_t>(data), size);
    s.Store(g);
}

template <typename Word> void Sha2Final(uint32_t g, uint32_t out, uint32_t outSize, uint32_t digestBytes)
{
    Sha2<Word> s;
    s.Load(g);
    uint8_t digest[64];
    s.Final(digest, digestBytes);
    memcpy(GuestPtr(out), digest, std::min(outSize, digestBytes));
}
}

GUEST_EXPORT(XeCryptSha256Init) { Sha2Init<uint32_t>(ctx.r3.u32, kSha256Init); }
GUEST_EXPORT(XeCryptSha256Update) { Sha2Update<uint32_t>(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
GUEST_EXPORT(XeCryptSha256Final) { Sha2Final<uint32_t>(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 32); }
GUEST_EXPORT(XeCryptSha384Init) { Sha2Init<uint64_t>(ctx.r3.u32, kSha384Init); }
GUEST_EXPORT(XeCryptSha384Update) { Sha2Update<uint64_t>(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
GUEST_EXPORT(XeCryptSha384Final) { Sha2Final<uint64_t>(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 48); }
GUEST_EXPORT(XeCryptSha512Init) { Sha2Init<uint64_t>(ctx.r3.u32, kSha512Init); }
GUEST_EXPORT(XeCryptSha512Update) { Sha2Update<uint64_t>(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
GUEST_EXPORT(XeCryptSha512Final) { Sha2Final<uint64_t>(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 64); }

// ---- XeKeys*: keys bound to the console ------------------------------------------------
// The title update signs and encrypts the profile with keys the console "obscures" with a
// secret in its fuses. This port has no such secret, so it uses a fixed one: what
// XeKeysObscureKey obscures, XeKeysAesCbcUsingKey and XeKeysHmacShaUsingKey accept.
namespace
{
const uint8_t kConsoleSecret[16] = { 'F', 'M', '2', '-', 'p', 'o', 'r', 't', '-', 'c', 'o', 'n', 's', 'o', 'l', 'e' };

bool AesRun(const uint8_t key[16], bool cbc, const uint8_t* iv16, const uint8_t* in, uint8_t* out, uint32_t size, bool encrypt)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
    {
        const wchar_t* mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, PUCHAR(mode), ULONG((wcslen(mode) + 1) * sizeof(wchar_t)), 0) == 0 &&
            BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, PUCHAR(key), 16, 0) == 0)
        {
            uint8_t iv[16] = {};
            if (iv16)
                memcpy(iv, iv16, 16);
            std::vector<uint8_t> input(in, in + size), output(size);
            ULONG got = 0;
            const NTSTATUS status = encrypt ? BCryptEncrypt(handle, input.data(), size, nullptr, cbc ? iv : nullptr, cbc ? 16 : 0, output.data(), size, &got, 0)
                                            : BCryptDecrypt(handle, input.data(), size, nullptr, cbc ? iv : nullptr, cbc ? 16 : 0, output.data(), size, &got, 0);
            if (status == 0)
            {
                memcpy(out, output.data(), size);
                ok = true;
            }
        }
    }
    if (handle)
        BCryptDestroyKey(handle);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// The key for obscuredKey (16 bytes in guest memory), or false.
bool Unobscure(uint32_t guestKey, uint8_t key[16])
{
    return guestKey && AesRun(kConsoleSecret, false, nullptr, GuestPtr<uint8_t>(guestKey), key, 16, false);
}

void HmacSha1(const uint8_t key[16], const std::vector<std::pair<const uint8_t*, uint32_t>>& parts, uint8_t out[20])
{
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++)
    {
        const uint8_t k = i < 16 ? key[i] : 0;
        ipad[i] = k ^ 0x36;
        opad[i] = k ^ 0x5C;
    }
    uint8_t inner[20];
    {
        Sha s;
        s.Update(ipad, 64);
        for (const auto& p : parts)
            if (p.first && p.second)
                s.Update(p.first, p.second);
        s.Final(inner);
    }
    Sha s;
    s.Update(opad, 64);
    s.Update(inner, 20);
    s.Final(out);
}
}

GUEST_EXPORT(XeKeysObscureKey)
{
    // (input[16], output[16]) -> status
    uint8_t out[16];
    if (!ctx.r3.u32 || !ctx.r4.u32 || !AesRun(kConsoleSecret, false, nullptr, GuestPtr<uint8_t>(ctx.r3.u32), out, 16, true))
    {
        Return(ctx, 0xC0000001);
        return;
    }
    memcpy(GuestPtr(ctx.r4.u32), out, 16);
    Return(ctx, 0);
}

GUEST_EXPORT(XeKeysAesCbcUsingKey)
{
    // (obscuredKey[16], input, size, output, iv[16], encrypt) -> status
    uint8_t key[16];
    const uint32_t size = ctx.r5.u32 & ~15u; // whole blocks
    if (!Unobscure(ctx.r3.u32, key) || !ctx.r4.u32 || !ctx.r6.u32 || !ctx.r7.u32)
    {
        Return(ctx, 0xC0000001);
        return;
    }
    // FM2_TRACE_KEYS=1: each key in the clear, with the caller (how the save formats were worked out:
    // tools/decomp/savecrypt.py)
    if (static const bool trace = getenv("FM2_TRACE_KEYS") != nullptr; trace)
    {
        char hex[33];
        for (int i = 0; i < 16; i++)
            snprintf(hex + i * 2, 3, "%02x", key[i]);
        std::string CurrentHostStack(int maxFrames);
        Log("crypt: AES-CBC %s key %s, %u bytes: %s", ctx.r8.u32 ? "encrypt" : "decrypt", hex, size, CurrentHostStack(8).c_str());
    }
    std::vector<uint8_t> out(size);
    if (size && !AesRun(key, true, GuestPtr<uint8_t>(ctx.r7.u32), GuestPtr<uint8_t>(ctx.r4.u32), out.data(), size, ctx.r8.u32 != 0))
    {
        Return(ctx, 0xC0000001);
        return;
    }
    if (size)
        memcpy(GuestPtr(ctx.r6.u32), out.data(), size);
    Return(ctx, 0);
}

GUEST_EXPORT(XeKeysHmacShaUsingKey)
{
    // (obscuredKey[16], in1, len1, in2, len2, in3, len3, out, outSize) -> status
    uint8_t key[16];
    if (!Unobscure(ctx.r3.u32, key) || !ctx.r10.u32)
    {
        Return(ctx, 0xC0000001);
        return;
    }
    const uint32_t inputs[3][2] = { { ctx.r4.u32, ctx.r5.u32 }, { ctx.r6.u32, ctx.r7.u32 }, { ctx.r8.u32, ctx.r9.u32 } };
    std::vector<std::pair<const uint8_t*, uint32_t>> parts;
    for (const auto& in : inputs)
        if (in[0] && in[1])
            parts.emplace_back(GuestPtr<uint8_t>(in[0]), in[1]);
    uint8_t digest[20];
    HmacSha1(key, parts, digest);
    memcpy(GuestPtr(ctx.r10.u32), digest, std::min<uint32_t>(StackArg(ctx, 8), 20));
    Return(ctx, 0);
}
