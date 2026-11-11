#include "gpu/indices.h"
#include <initializer_list>
#include <cstdlib>

#include "gpu/regs.h"
#include "live_switch.h"
#include "memory.h"

namespace gpu
{
uint32_t ReadGuestIndex(uint32_t base, uint32_t i, bool is32, uint32_t endian)
{
    if (is32)
    {
        uint32_t v = *GuestPtr<uint32_t>(base + i * 4);
        if (endian == 2)
            v = __builtin_bswap32(v);
        else if (endian == 1)
            v = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
        return v;
    }
    uint16_t v = *GuestPtr<uint16_t>(base + i * 2);
    if (endian == 1 || endian == 2)
        v = __builtin_bswap16(v);
    return v;
}

// Expanded host vertex list: guest indices, or rectangle markers
// (bit 31 | role << 29 | list position of the rectangle's first vertex).
bool BuildIndexList(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize, std::vector<uint32_t>& out, bool listFallback)
{
    const uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    const bool is32 = (initiator >> 11) & 1;
    const uint32_t offset = gpureg::Get(gpureg::VGT_INDX_OFFSET);
    // Reused between draws (no allocation per draw).
    thread_local std::vector<uint32_t> g;
    thread_local std::vector<uint8_t> cut;
    g.resize(count);
    cut.clear();
    // Primitive restart: a raw index equal to the reset value ends the strip/fan.
    const bool restart = (gpureg::Get(gpureg::PA_SU_SC_MODE_CNTL) >> 21) & 1;
    const uint32_t resetIndex = gpureg::Get(gpureg::VGT_MULTI_PRIM_IB_RESET_INDX) & (is32 ? 0xFFFFFF : 0xFFFF);
    if (source == 0) // DMA
    {
        const uint32_t base = GpuPhysToGuest(dmaBase & ~(is32 ? 3u : 1u));
        const uint32_t endian = dmaSize >> 30;
        const uint32_t available = (dmaSize & 0xFFFFFF) * (is32 ? 1 : 2);
        for (uint32_t i = 0; i < count && i < available; i++)
        {
            const uint32_t raw = ReadGuestIndex(base, i, is32, endian);
            if (restart && (raw & (is32 ? 0xFFFFFF : 0xFFFF)) == resetIndex)
            {
                cut.resize(count);
                cut[i] = 1;
            }
            g[i] = raw + offset;
        }
        // A draw whose count runs past the index buffer (DMA size) reads zero indices there, as the
        // indexed path's buffer view does: the vector is reused between draws, so those entries kept
        // an earlier draw's indices (stray triangles). From the FM4 port. FM2_STALE_INDICES=1: as before (A/B).
        static LiveSwitch staleIndices("FM2_STALE_INDICES");
        if (count > available && !staleIndices)
            for (uint32_t i = available; i < count; i++)
                g[i] = offset;
    }
    else if (source == 2) // auto
    {
        for (uint32_t i = 0; i < count; i++)
            g[i] = i + offset;
    }
    else
        return false;

    out.clear();
    switch (prim)
    {
    case 1: // point list: a quad per point, corner in bits 29-30
    {
        out.resize(size_t(count) * 6);
        uint32_t* o = out.data();
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t corner : { 0u, 1u, 2u, 2u, 1u, 3u })
                *o++ = (corner << 29) | (g[i] & 0x1FFFFFFF);
        return true;
    }
    case 4: // triangle list
        out.assign(g.begin(), g.begin() + (count / 3) * 3);
        return true;
    case 6: // triangle strip(s), split at restart indices
    case 5: // triangle fan(s)
    {
        out.resize(count >= 3 ? size_t(count - 2) * 3 : 0); // upper bound
        uint32_t* o = out.data();
        for (uint32_t start = 0; start < count;)
        {
            uint32_t end = start;
            if (cut.empty())
                end = count;
            else
                while (end < count && !cut[end])
                    end++;
            for (uint32_t i = start; i + 2 < end; i++)
            {
                const uint32_t k = i - start;
                if (prim == 5)
                    o[0] = g[start], o[1] = g[i + 1], o[2] = g[i + 2];
                else if (k & 1)
                    o[0] = g[i + 1], o[1] = g[i], o[2] = g[i + 2];
                else
                    o[0] = g[i], o[1] = g[i + 1], o[2] = g[i + 2];
                o += 3;
            }
            start = end + 1;
        }
        out.resize(size_t(o - out.data()));
        return true;
    }
    case 13: // quad list
    {
        out.resize(size_t(count / 4) * 6);
        uint32_t* o = out.data();
        for (uint32_t i = 0; i + 3 < count; i += 4, o += 6)
            o[0] = g[i], o[1] = g[i + 1], o[2] = g[i + 2], o[3] = g[i], o[4] = g[i + 2], o[5] = g[i + 3];
        return true;
    }
    case 8: // rectangle list: first triangle as given, second from the
            // hypotenuse ends and the computed fourth corner
        for (uint32_t i = 0; i + 2 < count; i += 3)
        {
            const uint32_t pos = uint32_t(out.size());
            out.insert(out.end(), { g[i], g[i + 1], g[i + 2] });
            // The Xenos draws each rectangle as one primitive with the given triangle's facing. The second
            // triangle is ordered (first hypotenuse end, fourth corner, second end), which keeps that
            // winding: the other order faced the other way, so a draw with separate back-face stencil ops
            // applied them to half of every rectangle. From the FM4 port. FM2_OLD_RECT_WINDING=1 (read
            // at start): the old order (A/B).
            static const bool oldWinding = getenv("FM2_OLD_RECT_WINDING") != nullptr;
            for (uint32_t role : oldWinding ? std::initializer_list<uint32_t>{ 1u, 2u, 0u } : std::initializer_list<uint32_t>{ 1u, 0u, 2u })
                out.push_back(0x80000000u | (role << 29) | pos);
        }
        return true;
    default:
        if (!listFallback)
            return false;
        out.assign(g.begin(), g.begin() + (count / 3) * 3);
        return true;
    }
}
}
