// xmadecode <bank.fsb> <outdir>: each XMA sample of an FMOD (FSB3) sound bank decoded as the game's
// XMA decoder does (src/audio/xma.cpp: codec opened per stream, 128 dropped at the start, the first
// packet fed again until the hardware length, frames * 512 - trim_start), to
// <outdir>/<first packet hash>.raw (16-bit interleaved). One line per sample on stdout:
// name hash rate channels frames trim_start trim_end samples loop_start loop_end. For tools/mkloops.py.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

#include "../src/audio/xma_trims.h"

constexpr size_t kPacket = 2048;

static uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static std::vector<int16_t> Decode(const uint8_t* data, size_t packets, uint32_t ch, uint32_t rate, uint32_t& frames, uint32_t& ts, uint32_t& te)
{
    std::vector<int16_t> pcm;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->sample_rate = int(rate);
    av_channel_layout_default(&av->ch_layout, int(ch));
    av->block_align = kPacket;
    av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    av->extradata_size = 34;
    av->extradata[0] = 1;
    if (avcodec_open2(av, codec, nullptr) < 0)
        return pcm;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    frames = ts = te = 0;
    PacketTrims(data, ch, ts, te);
    uint32_t skip = std::min<uint32_t>(128, ts);
    auto receive = [&] {
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, fch = std::min<int>(frame->ch_layout.nb_channels, int(ch));
            const int s = std::min<int>(n, int(skip));
            skip -= uint32_t(s);
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n - s) * ch);
            for (int c = 0; c < int(ch); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, fch - 1)]);
                for (int i = s; i < n; i++)
                    pcm[base + size_t(i - s) * ch + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
    };
    auto send = [&](const uint8_t* p) {
        if (av_new_packet(packet, kPacket) < 0)
            return;
        memcpy(packet->data, p, kPacket);
        avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        receive();
    };
    for (size_t k = 0; k < packets; k++)
    {
        PacketTrims(data + k * kPacket, ch, ts, te, &frames);
        send(data + k * kPacket);
    }
    const size_t want = size_t(frames) * 512 - std::min<size_t>(size_t(frames) * 512, ts);
    for (int k = 0; k < 8; k++)
    {
        if (pcm.size() / ch >= want && k >= 2)
            break;
        send(data);
    }
    pcm.resize(std::min(pcm.size(), want * ch));
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&av);
    return pcm;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: xmadecode <bank.fsb> <outdir>\n");
        return 1;
    }
    av_log_set_level(AV_LOG_QUIET);
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    auto u16 = [&](size_t at) { return uint32_t(b[at] | b[at + 1] << 8); };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (b.size() < 24 || memcmp(b.data(), "FSB3", 4))
    {
        fprintf(stderr, "%s: not an FSB3 bank\n", argv[1]);
        return 1;
    }
    std::filesystem::create_directories(argv[2]);
    const uint32_t count = u32(4);
    size_t at = 24, data = 24 + u32(8);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t size = u16(at);
        const std::string name(reinterpret_cast<const char*>(&b[at + 2]), strnlen(reinterpret_cast<const char*>(&b[at + 2]), 30));
        const uint32_t csize = u32(at + 36), ls = u32(at + 40), le = u32(at + 44), rate = u32(at + 52), ch = u16(at + 62);
        if (data + csize <= b.size() && csize >= kPacket && ch && ch <= 2)
        {
            uint32_t frames, ts, te;
            const std::vector<int16_t> pcm = Decode(&b[data], csize / kPacket, ch, rate, frames, ts, te);
            char hash[17];
            snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)Fnv(&b[data], kPacket));
            std::ofstream((std::filesystem::path(argv[2]) / (std::string(hash) + ".raw")), std::ios::binary)
                .write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * 2));
            printf("%s %s %u %u %u %u %u %zu %u %u\n", name.c_str(), hash, rate, ch, frames, ts, te, pcm.size() / ch, ls, le);
        }
        at += size;
        data += csize;
    }
    return 0;
}
