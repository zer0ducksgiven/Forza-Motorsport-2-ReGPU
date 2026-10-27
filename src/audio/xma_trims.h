#pragma once
// PacketTrims, shared by the decoder (xma.cpp) and tools/xmadecode.cpp.
#include <cstdint>

constexpr uint32_t kXmaPacketBits = 2048 * 8;

// The trims in an XMA packet's frame headers. A WMA Pro frame header can say how many samples the
// decoder drops at the start (first frame) and at the end (last frame) of a stream; the hardware
// applies them. FFmpeg's XMA decoder reads them but only takes trim_start + trim_end - 192 off the
// end of the stream (wmaprodec.c, xma_decode_packet), so streams play their pre-roll at the start
// and lose real audio at the end, by different amounts per sound. The fields follow the frame
// length (15 bits), the tile header (subframe layout), the post-process transform and the DRC
// gain (8 bits); XMA's fixed setup (decode_flags 0x10d6): 512 samples a frame, up to 4 subframes.
// Each found value overwrites the last (first frame's start, last frame's end, as FFmpeg keeps them).
inline void PacketTrims(const uint8_t* packet, uint32_t channels, uint32_t& trimStart, uint32_t& trimEnd, uint32_t* frames = nullptr)
{
    uint32_t pos = 0;
    bool ok = true;
    const auto get = [&](uint32_t n) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++, pos++)
        {
            if (pos >= kXmaPacketBits)
            {
                ok = false;
                return 0u;
            }
            v = v << 1 | ((packet[pos >> 3] >> (7 - (pos & 7))) & 1);
        }
        return v;
    };
    const uint32_t header = uint32_t(packet[0]) << 24 | uint32_t(packet[1]) << 16 | uint32_t(packet[2]) << 8 | packet[3];
    const uint32_t first = (header >> 11) & 0x7FFF;
    if (first == 0x7FFF)
        return;
    const uint32_t nb = channels > 1 ? 2 : 1;
    for (uint32_t at = 32 + first; at + 15 <= kXmaPacketBits;)
    {
        pos = at;
        ok = true;
        const uint32_t len = get(15);
        if (len == 0 || len == 0x7FFF)
            return;
        if (frames)
            ++*frames;
        // tile header (decode_tilehdr / decode_subframe_length)
        uint32_t samples[2] = {}, contains[2] = {}, forCur = nb;
        int minLen = 0;
        const bool fixed = get(1) != 0;
        for (int guard = 0; ok && minLen < 512 && guard < 64; guard++)
        {
            for (uint32_t c = 0; c < nb; c++)
                contains[c] = int(samples[c]) == minLen ? (fixed || forCur == 1 || minLen == 512 - 128 ? 1 : get(1)) : 0;
            int sub;
            if (minLen == 512 - 128)
                sub = 128;
            else
                sub = get(1) ? 512 >> (1 + get(1)) : 512;
            minLen += sub;
            for (uint32_t c = 0; c < nb; c++)
            {
                if (contains[c])
                    samples[c] += uint32_t(sub);
                else if (int(samples[c]) <= minLen)
                {
                    if (int(samples[c]) < minLen)
                    {
                        forCur = 0;
                        minLen = int(samples[c]);
                    }
                    forCur++;
                }
            }
        }
        if (nb > 1 && get(1) && get(1))
            pos += nb * nb * 4;
        get(8); // DRC gain
        if (ok && get(1))
        {
            if (get(1))
            {
                const uint32_t v = get(10);
                if (ok && v)
                    trimStart = v;
            }
            if (get(1))
            {
                const uint32_t v = get(10);
                if (ok && v)
                    trimEnd = v;
            }
        }
        if (at + len > kXmaPacketBits || !((packet[(at + len - 1) >> 3] >> (7 - ((at + len - 1) & 7))) & 1))
            return; // spans into the next packet, or the last frame in this one
        at += len;
    }
}
