// XMA decoder contexts. The title (FMOD) drives the console's hardware decoder
// through the kernel's XMA* exports, which edit a 64-byte context in guest
// memory: two input buffers of 2 KB XMA packets, an output ring of 256-byte
// blocks of 16-bit big-endian PCM, loop points. Each context gets an FFmpeg
// xma2 decoder fed one packet at a time. Like the hardware it runs ahead
// whenever there is output space; the consumer sets the pace.
//
// XMA_CONTEXT_DATA, big-endian dwords, bit fields counted from bit 0:
//   0: in0 packet count 0-11, loop count 12-19, in0 valid 20, in1 valid 21,
//      output block count 22-26, output write offset 27-31
//   1: in1 packet count 0-11, loop subframe end 14-16, skip 17-19,
//      subframe decode count 20-23, sample rate 27-28, stereo 29,
//      output valid 31
//   2: input read offset (bits) 0-25
//   3: loop start 0-25      4: loop end 0-25, current input buffer 31
//   5: in0 address  6: in1 address  7: output address  8: work buffer
//   9: output read offset 0-4
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <windows.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "audio/seamless_loops.h"
#include "audio/xma_trims.h"

bool DealerMutesMusic(); // hooks/dealer.cpp: the easter egg is playing

namespace
{
constexpr uint32_t kContextSize = 0x40;
constexpr uint32_t kPacketBits = 2048 * 8;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kPacketBytes = 2048;

const bool g_traceXma = getenv("FM2_TRACE_XMA") != nullptr;

uint32_t Field(uint32_t ctx, int dword, int lo, int bits)
{
    return (LoadBE32(ctx + dword * 4) >> lo) & ((1u << bits) - 1);
}

void SetField(uint32_t ctx, int dword, int lo, int bits, uint32_t value)
{
    const uint32_t mask = ((1u << bits) - 1) << lo;
    const uint32_t a = ctx + dword * 4;
    StoreBE32(a, (LoadBE32(a) & ~mask) | ((value << lo) & mask));
}

std::mutex g_mutex;

uint64_t StreamHash(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Seamless loops for the engine sounds whose own loop joins audibly (the rev hitch on the Supra,
// Evo and RX-7, and many more): audio/seamless_loops.h, made offline from the sound banks
// (tools/xmadecode.cpp, tools/mkloops.py), by a hash of each stream's first packet.
const SeamlessLoop* FindSeamlessLoop(uint64_t hash)
{
    const auto end = std::end(kSeamlessLoops);
    const auto it = std::lower_bound(std::begin(kSeamlessLoops), end, hash, [](const SeamlessLoop& l, uint64_t h) { return l.hash < h; });
    return it != end && it->hash == hash ? it : nullptr;
}

// A sound's first pass as decoded, by first packet hash: what its loop-start blend fades in from.
std::mutex g_firstPassMutex;
std::map<uint64_t, std::shared_ptr<const std::vector<int16_t>>> g_firstPasses;

double RaisedCosine(size_t i, uint32_t n)
{
    return 0.5 - 0.5 * std::cos(3.14159265358979 * double(i + 1) / double(n + 1));
}

// The gain that keeps a blend of a and b, alike by r (correlation), at their level: they mix to
// sqrt(a^2 + b^2 + 2rab), so the middle of an uncorrected blend dips (heard as a gap).
double BlendGain(double w, float r)
{
    const double c = std::clamp(double(r), 0.0, 1.0);
    return 1.0 / std::sqrt((1.0 - w) * (1.0 - w) + w * w + 2.0 * c * w * (1.0 - w));
}

int16_t Blend(double a, double b, double w, float r)
{
    return int16_t(std::clamp(std::round((a * (1.0 - w) + b * w) * BlendGain(w, r)), -32768.0, 32767.0));
}

// Music packets, by a hash of their first 64 bytes: the title reads its music files (Media/Audio/Music/*.xma)
// into a file buffer and copies packets on to the decoder's input, so a packet is recognised by
// its contents (AudioNoteMusicRead). The "whysthisdealer" easter egg silences music while it plays.
std::mutex g_musicMutex;
std::set<uint64_t> g_musicPackets;
uint64_t PacketHash(const uint8_t* p)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 64; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
bool MusicPacket(const uint8_t* p)
{
    std::lock_guard lock(g_musicMutex);
    return g_musicPackets.count(PacketHash(p)) != 0;
}
std::set<uint32_t> g_enabled;
std::once_flag g_started;

AVCodecContext* OpenCodec(uint32_t ch, uint32_t hz)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext* av = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!av)
        return nullptr;
    av->sample_rate = int(hz);
    av_channel_layout_default(&av->ch_layout, int(ch));
    av->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX (34 bytes): one stream; FFmpeg reads the stream count from it.
    av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    av->extradata_size = 34;
    av->extradata[0] = 1;
    if (avcodec_open2(av, codec, nullptr) < 0)
        avcodec_free_context(&av);
    return av;
}

// Codecs are opened and freed only by the decoder thread, outside g_mutex. An XMA2 codec takes about
// 4 MB of RAM and 1-3 ms to open (eight stream decoders and their transforms), and since the engine-sound
// decode every engine sound start needs a fresh one. Opened by whatever stepped the stream, that was
// the title's mixer thread (XMAEnableContext decodes the first blocks) or the decoder thread holding
// g_mutex, which the mixer waits on: at a race start (about 40 sounds at once) the mixer missed its
// 1024-sample deadline and the console's driver replayed the last block, heard on slower CPUs as
// stretched, crusty sound. A stream that needs a codec when none is ready waits for the next pass
// (2 ms; the hardware decodes in the background too), and the mixer polls it meanwhile. So each format
// keeps as many spare as it has taken between two refills at its busiest (a race start's engine
// sounds), from 2 up to 40: memory only once a format has needed it, about 4 MB each.
// FM2_XMA_NO_CODEC_POOL=1: opened in place.
constexpr size_t kMinPooledCodecs = 2, kMaxPooledCodecs = 40;
std::mutex g_poolMutex;
std::map<std::pair<uint32_t, uint32_t>, std::vector<AVCodecContext*>> g_codecPool;
std::map<std::pair<uint32_t, uint32_t>, size_t> g_codecsTaken, g_codecTarget; // taken since the last refill, spares kept
std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> g_codecsWanted; // contexts waiting for one
std::vector<AVCodecContext*> g_retiredCodecs;
std::atomic<uint32_t> g_codecWaits{ 0 }; // (FM2_TRACE_XMA_ENABLE)

bool NoCodecPool()
{
    static LiveSwitch noPool("FM2_XMA_NO_CODEC_POOL");
    return noPool;
}

// Whether a codec for ch/hz can be taken now for context ctx. If not, the decoder thread opens one
// before its next pass (one per waiting context: the title polls a waiting one thousands of times).
bool CodecReady(uint32_t ctx, uint32_t ch, uint32_t hz)
{
    if (NoCodecPool())
        return true;
    std::lock_guard lock(g_poolMutex);
    if (!g_codecPool[{ ch, hz }].empty())
        return true;
    g_codecsWanted[{ ch, hz }].insert(ctx);
    g_codecWaits++;
    return false;
}

AVCodecContext* TakeCodec(uint32_t ch, uint32_t hz)
{
    if (!NoCodecPool())
    {
        std::lock_guard lock(g_poolMutex);
        auto& pool = g_codecPool[{ ch, hz }]; // (a new entry: the decoder thread keeps it filled)
        g_codecsTaken[{ ch, hz }]++;
        if (!pool.empty())
        {
            AVCodecContext* av = pool.back();
            pool.pop_back();
            return av;
        }
    }
    return OpenCodec(ch, hz); // (with the pool: a first use, or a caller that didn't ask CodecReady)
}

void RetireCodec(AVCodecContext* av)
{
    if (!av)
        return;
    if (NoCodecPool())
        return avcodec_free_context(&av);
    std::lock_guard lock(g_poolMutex);
    g_retiredCodecs.push_back(av);
}

// On the decoder thread, without g_mutex: frees used codecs, then opens codecs for the streams waiting
// and up to its target of spares, all of them. A stream waiting for one is polled by the title's mixer
// (XMAEnableContext, over 100,000 times a second at a race start): opened one per pass, the 40 or so
// a race start wants kept it spinning for seconds.
void RefillCodecPool()
{
    std::vector<AVCodecContext*> retired;
    std::vector<std::pair<uint32_t, uint32_t>> wanted;
    {
        std::lock_guard lock(g_poolMutex);
        retired.swap(g_retiredCodecs);
        for (const auto& [key, pool] : g_codecPool)
        {
            size_t& target = g_codecTarget[key];
            const size_t taken = std::min(g_codecsTaken[key], kMaxPooledCodecs);
            if (taken > target && taken > kMinPooledCodecs)
                Log("xma: keeping %zu spare codecs for %u ch %u Hz (%zu taken between refills)", taken, key.first, key.second, g_codecsTaken[key]);
            target = std::max({ target, taken, kMinPooledCodecs });
            g_codecsTaken[key] = 0;
            const size_t waiting = g_codecsWanted.count(key) ? g_codecsWanted[key].size() : 0;
            for (size_t i = pool.size(); i < std::max(target, kMinPooledCodecs) + waiting; i++)
                wanted.push_back(key);
        }
        g_codecsWanted.clear();
    }
    for (AVCodecContext* av : retired)
        avcodec_free_context(&av);
    for (const auto& key : wanted)
        if (AVCodecContext* av = OpenCodec(key.first, key.second))
        {
            std::lock_guard lock(g_poolMutex);
            g_codecPool[key].push_back(av);
        }
}

struct Decoder
{
    AVCodecContext* av = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> pcm; // decoded, interleaved, not yet in the output ring
    size_t pcmRead = 0;
    bool flush = true; // start of a stream or a jump: reset before the next packet
    // The stream being decoded (from a reset to its drain), output as the hardware does (see Drain).
    uint32_t streamFrames = 0, trimStart = 0, trimEnd = 0, skipStart = 0;
    size_t streamOut = 0; // its samples so far
    uint8_t firstPacket[kPacketBytes] = {};
    uint64_t firstHash = 0;
    bool exact = false; // an engine sound: decoded as the hardware does
    std::vector<int16_t> head; // its first 512 output frames (the loop crossfade, see Drain)
    const SeamlessLoop* loop = nullptr; // its seamless loop, if it has one
    std::vector<int16_t> history; // with one: its samples as decoded
    std::shared_ptr<const std::vector<int16_t>> firstPass; // and its first pass (the loop-start blend)
    bool starved = false;
    size_t packetsDecoded = 0, framesWritten = 0, framesDecoded = 0;
    // An input buffer whose packets are all decoded stays valid for the title until their audio
    // has reached the output ring (the hardware has no backlog, so the title takes "consumed"
    // to mean "delivered"); it would loop or stop a stream while the tail was still queued here.
    bool consumed[2] = {};
    bool holdsTail = false; // packets were decoded since the codec was last drained or reset
    bool seeked = false; // the title set the read offset since the last packet was decoded
    bool music = false;  // its last packet came from a music file (MusicPacket)

    void Close()
    {
        RetireCodec(av);
        av = nullptr;
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    // A codec from the pool (see CodecReady): opening one takes milliseconds, too long for the title's
    // mixer thread, which steps streams through XMAEnableContext.
    bool Open(uint32_t ch, uint32_t hz)
    {
        RetireCodec(av);
        if (!(av = TakeCodec(ch, hz)))
            return false;
        if (!packet)
            packet = av_packet_alloc();
        if (!frame)
            frame = av_frame_alloc();
        channels = ch;
        rate = hz;
        return true;
    }

    // Appends what the codec has decoded, less the first skipStart samples of the stream.
    void Receive()
    {
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const int skip = std::min<int>(n, int(skipStart));
            skipStart -= uint32_t(skip);
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n - skip) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = skip; i < n; i++)
                    pcm[base + size_t(i - skip) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
    }

    // The stream's seamless loop over pcm[from..], sample index at (interleaved) of the stream on.
    // FMOD plays a stream x of L samples as the loop x[S:L], S = trim_start - 128 (it skips the
    // restart's pre-roll). Either its last n samples (pad early, before the silence some streams
    // end with) blend into x[from-n-pad:from-pad] and the pad is filled with x[from-pad:from],
    // where what follows x[from] is like what follows x[S]; or, from the second pass on, its first
    // n2 after S fade in from x[from2:], where what precedes x[from2] is like the loop's end.
    void PatchLoop(size_t at, size_t from)
    {
        if (!loop)
            return;
        const size_t ch = channels, S = size_t(trimStart > 128 ? trimStart - 128 : 0) * ch;
        const size_t tail = size_t(loop->length - loop->n - loop->pad) * ch, src = size_t(loop->from - loop->n - loop->pad) * ch;
        for (size_t i = from; i < pcm.size(); i++, at++)
        {
            if (at == history.size())
                history.push_back(pcm[i]);
            if (firstPass && at >= S && at < S + size_t(loop->n2) * ch && size_t(loop->from2) * ch + (at - S) < firstPass->size())
                pcm[i] = Blend((*firstPass)[size_t(loop->from2) * ch + (at - S)], history[at], RaisedCosine((at - S) / ch, loop->n2), loop->r2);
            else if (loop->n && at >= tail && at < size_t(loop->length) * ch && src + (at - tail) < history.size())
            {
                const size_t j = (at - tail) / ch;
                pcm[i] = Blend(history[at], history[src + (at - tail)], j >= loop->n ? 1.0 : RaisedCosine(j, loop->n), loop->r);
            }
        }
    }

    // The end of a stream. The hardware plays every frame of it, trim_start dropped at the start and
    // trim_end ignored (measured over XBDM on a console: 451 stream ends): frames * 512 - trim_start
    // samples. FFmpeg's end of stream takes trim_start + trim_end - 192 off the end instead, and holds
    // back about a packet until more input comes: the stream's first packet (its loop's natural
    // continuation) is fed again until the stream reaches its length, and it is cut there. Then
    // silence up to a whole output block; the codec is opened again at the next stream.
    void Drain(size_t block)
    {
        if (!av)
            return;
        const size_t pcmBefore = pcm.size();
        if (!exact)
        {
            avcodec_send_packet(av, nullptr);
            Receive();
        }
        else
            DrainExact(pcmBefore);
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        if (const size_t rest = (pcm.size() - pcmRead) % block)
            pcm.resize(pcm.size() + (block - rest), 0);
        if (g_traceXma)
            Log("xma: drained %zu frames at the end of a stream", (pcm.size() - pcmBefore) / channels);
        flush = true;
        holdsTail = false;
    }

    void DrainExact(size_t pcmBefore)
    {
        const size_t want = size_t(streamFrames) * 512 - std::min<size_t>(size_t(streamFrames) * 512, trimStart);
        for (int k = 0; k < 8; k++)
        {
            // Twice at least: once was not always enough to push out what FFmpeg holds back.
            if (streamOut + (pcm.size() - pcmBefore) / channels >= want && k >= 2)
                break;
            if (av_new_packet(packet, kPacketBytes) >= 0)
            {
                memcpy(packet->data, firstPacket, kPacketBytes);
                avcodec_send_packet(av, packet);
                av_packet_unref(packet);
            }
            Receive();
        }
        streamOut += (pcm.size() - pcmBefore) / channels;
        if (streamOut > want)
        {
            const size_t cut = std::min(streamOut - want, (pcm.size() - pcmRead) / channels);
            pcm.resize(pcm.size() - cut * channels);
            streamOut -= cut;
        }
        if (pcm.size() > pcmBefore)
            PatchLoop((streamOut - (pcm.size() - pcmBefore) / channels) * channels, pcmBefore);
        if (loop && loop->n2 && !firstPass && history.size() >= size_t(loop->length) * channels)
        {
            std::lock_guard lock(g_firstPassMutex);
            g_firstPasses.emplace(firstHash, std::make_shared<const std::vector<int16_t>>(history.begin(), history.begin() + ptrdiff_t(size_t(loop->length) * channels)));
        }
        // Engine sounds without a seamless loop: the last 384 samples crossfade into the 384 FMOD skips
        // after a restart, so a looped one at least joins without a step.
        if (!loop)
        {
            const size_t skip = trimStart > 128 ? trimStart - 128 : 0;
            const size_t n = std::min<size_t>({ 384, skip, (pcm.size() - pcmRead) / channels });
            if (n >= 8 && head.size() >= skip * channels)
            {
                const size_t end = pcm.size() / channels;
                for (uint32_t c = 0; c < channels; c++)
                    for (size_t i = 0; i < n; i++)
                    {
                        int16_t& t = pcm[(end - n + i) * channels + c];
                        t = Blend(t, head[(skip - n + i) * channels + c], RaisedCosine(i, uint32_t(n)), 1.0f); // continuous audio: no gain
                    }
            }
        }
    }

    // Decodes one 2 KB packet and appends its samples.
    void Decode(const uint8_t* data)
    {
        const bool wasFlush = flush;
        if (flush)
        {
            flush = false;
            // An engine sound (listed in audio/seamless_loops.h by its first packet) is decoded as the
            // hardware does, see Drain: the codec is opened again (FFmpeg's XMA decoder has no flush
            // callback: avcodec_flush_buffers left its overlap, its 4096-sample FIFO and its end of
            // stream in place), and of the trim_start the hardware drops at the start, FFmpeg drops
            // all but 128. Other sounds (menu sounds, music, a jump into a stream) as before: cut and
            // crossfaded like engine loops, menu sounds clipped.
            firstHash = StreamHash(data, kPacketBytes);
            const SeamlessLoop* listed = FindSeamlessLoop(firstHash);
            exact = listed != nullptr;
            loop = listed && (listed->n || listed->n2) ? listed : nullptr;
            streamFrames = trimStart = trimEnd = skipStart = 0;
            streamOut = 0;
            head.clear();
            history.clear();
            firstPass = nullptr;
            if (!exact)
                avcodec_flush_buffers(av);
            else
            {
                if (!Open(channels, rate))
                    return;
                memcpy(firstPacket, data, kPacketBytes);
                PacketTrims(data, channels, trimStart, trimEnd);
                skipStart = std::min<uint32_t>(128, trimStart);
                if (loop && loop->n2)
                {
                    std::lock_guard lock(g_firstPassMutex);
                    if (const auto it = g_firstPasses.find(firstHash); it != g_firstPasses.end())
                        firstPass = it->second;
                }
            }
        }
        if (exact)
            PacketTrims(data, channels, trimStart, trimEnd, &streamFrames); // later packets can carry the end trim
        const size_t pcmBefore = pcm.size();
        packetsDecoded++;
        if (av_new_packet(packet, kPacketBytes) < 0)
            return;
        memcpy(packet->data, data, kPacketBytes);
        const int sent = avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        if (sent < 0)
        {
            flush = true;
            return;
        }
        Receive();
        PatchLoop(streamOut * channels, pcmBefore);
        if (exact && head.size() < size_t(512) * channels && pcm.size() > pcmBefore)
            head.insert(head.end(), pcm.begin() + ptrdiff_t(pcmBefore),
                pcm.begin() + ptrdiff_t(std::min(pcm.size(), pcmBefore + size_t(512) * channels - head.size())));
        streamOut += (pcm.size() - pcmBefore) / channels;
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        holdsTail = true;
        if (g_traceXma)
            Log("xma: decoded a packet: %zu frames (flush %d)", (pcm.size() - pcmBefore) / channels, int(wasFlush));
    }
};

std::map<uint32_t, Decoder> g_decoders;

// The title refilled an input buffer: whatever "consumed" was still pending for it is over.
void ClearConsumed(uint32_t ctx, int dword, int bit)
{
    if (dword != 0 || (bit != 20 && bit != 21))
        return;
    if (auto it = g_decoders.find(ctx); it != g_decoders.end())
        it->second.consumed[bit - 20] = false;
}

// The output ring has a free block: fill it from decoded samples. Returns false when it
// has nothing decoded to give.
bool WriteBlock(uint32_t ctx, Decoder& d)
{
    const size_t perBlock = kBlockBytes / 2;
    if (d.pcm.size() - d.pcmRead < perBlock)
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5), write = Field(ctx, 0, 27, 5);
    uint16_t* out = GuestPtr<uint16_t>(LoadBE32(ctx + 7 * 4) + write * kBlockBytes);
    const bool mute = d.music && DealerMutesMusic();
    for (size_t i = 0; i < perBlock; i++)
        out[i] = mute ? 0 : __builtin_bswap16(uint16_t(d.pcm[d.pcmRead + i]));
    d.pcmRead += perBlock;
    d.framesWritten += perBlock / std::max<uint32_t>(d.channels, 1);
    if (d.pcmRead > (1u << 16))
    {
        d.pcm.erase(d.pcm.begin(), d.pcm.begin() + ptrdiff_t(d.pcmRead));
        d.pcmRead = 0;
    }
    SetField(ctx, 0, 27, 5, (write + 1) % blocks);
    return true;
}

// One decoder step for a context: fill one output block, decoding the next input packet
// when needed. Returns false when it has nothing to do.
bool Step(uint32_t ctx)
{
    if (!Field(ctx, 1, 31, 1)) // output buffer not valid
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5);
    if (!blocks)
        return false;
    const uint32_t write = Field(ctx, 0, 27, 5), read = Field(ctx, 9, 0, 5);
    // Keep one block free so a full ring never reads as empty.
    if ((read + blocks - write - 1) % blocks == 0)
        return false;

    static const uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
    const uint32_t channels = Field(ctx, 1, 29, 1) ? 2 : 1, rate = kRates[Field(ctx, 1, 27, 2)];
    Decoder& d = g_decoders[ctx];
    if (!d.av || d.channels != channels || d.rate != rate)
        if (!CodecReady(ctx, channels, rate) || !d.Open(channels, rate))
            return false;
    if (WriteBlock(ctx, d))
        return true;

    // Everything decoded so far has been handed over. If no input is waiting, this is the end of
    // the stream: deliver what the codec still holds first.
    auto valid = [&](uint32_t index) { return Field(ctx, 0, 20 + index, 1) && !d.consumed[index]; };
    if (d.holdsTail && !valid(0) && !valid(1))
    {
        d.Drain(kBlockBytes / 2);
        if (WriteBlock(ctx, d))
            return true;
    }
    for (uint32_t i = 0; i < 2; i++)
        if (d.consumed[i])
        {
            d.consumed[i] = false;
            SetField(ctx, 0, 20 + i, 1, 0); // now the title may refill (or loop) it
        }

    uint32_t current = Field(ctx, 4, 31, 1);
    if (!valid(current))
    {
        current ^= 1;
        if (!valid(current))
        {
            if (g_traceXma && !d.starved)
                Log("xma: %08X starved (both input buffers consumed)", ctx);
            d.starved = true;
            return false; // starved: both input buffers consumed
        }
        d.starved = false;
        SetField(ctx, 4, 31, 1, current);
        SetField(ctx, 2, 0, 26, 32);
    }
    const uint32_t packets = current ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12);
    const uint32_t buffer = LoadBE32(ctx + (5 + current) * 4);
    const uint32_t packet = Field(ctx, 2, 0, 26) / kPacketBits;
    if (packet < packets)
    {
        if (g_traceXma && d.seeked)
        {
            const uint32_t header = LoadBE32(buffer + packet * kPacketBytes), start = Field(ctx, 2, 0, 26) % kPacketBits;
            if (start != 32 + ((header >> 11) & 0x7FFF))
                Log("xma: %08X decode starts mid-packet: packet %u of %u, offset %u in it, the packet's first frame at %u", ctx, packet, packets, start,
                    32 + ((header >> 11) & 0x7FFF));
        }
        // An engine sound's start opens a fresh codec (Decode): only once one is ready.
        if (d.flush && FindSeamlessLoop(StreamHash(GuestPtr<uint8_t>(buffer + packet * kPacketBytes), kPacketBytes)) && !CodecReady(ctx, channels, rate))
            return false;
        d.seeked = false;
        d.music = MusicPacket(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
        if (d.music)
            LOG_ONCE("xma: %08X is music (the first stream recognised)", ctx);
        d.Decode(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
    }

    // Loop: when the packet holding the loop end is done and loops remain, continue from
    // the loop start (packet-accurate).
    const uint32_t loops = Field(ctx, 0, 12, 8), loopEnd = Field(ctx, 4, 0, 26);
    if (loops && loopEnd / kPacketBits == packet)
    {
        if (loops != 255)
            SetField(ctx, 0, 12, 8, loops - 1);
        SetField(ctx, 2, 0, 26, Field(ctx, 3, 0, 26));
        d.flush = true;
        return true;
    }
    const uint32_t offset = (packet + 1) * kPacketBits + 32;
    if (packet + 1 >= packets)
    {
        d.consumed[current] = true; // (valid for the title until its audio is delivered)
        SetField(ctx, 4, 31, 1, current ^ 1);
        SetField(ctx, 2, 0, 26, 32);
    }
    else
        SetField(ctx, 2, 0, 26, offset);
    return true;
}

void DecoderLoop()
{
    SetThreadDescription(GetCurrentThread(), L"xma decoder");
    auto next = Clock::now();
    for (;;)
    {
        next += std::chrono::milliseconds(2);
        std::this_thread::sleep_until(next);
        RefillCodecPool();
        std::lock_guard lock(g_mutex);
        for (uint32_t ctx : g_enabled)
            while (Step(ctx))
                ;
    }
}
}

GUEST_EXPORT(XMACreateContext)
{
    const uint32_t context = GuestAlloc(Heap::Physical, kContextSize, 0x100);
    StoreIfNonNull(ctx.r3.u32, context);
    std::call_once(g_started, [] {
        av_log_set_level(AV_LOG_ERROR);
        std::thread(DecoderLoop).detach();
    });
    Return(ctx, context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY);
}

GUEST_EXPORT(XMAReleaseContext)
{
    {
        std::lock_guard lock(g_mutex);
        g_enabled.erase(ctx.r3.u32);
        if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
        {
            it->second.Close();
            g_decoders.erase(it);
        }
    }
    GuestFree(ctx.r3.u32);
    Return(ctx, 0);
}

GUEST_EXPORT(XMAInitializeContext)
{
    // XMA_CONTEXT_INIT: in0, in0 packets, in1, in1 packets, read offset,
    // output, output blocks, work buffer, subframes, channels, sample rate,
    // loop start, loop end, loop count/subframe end/skip (bytes).
    const uint32_t c = ctx.r3.u32, init = ctx.r4.u32;
    std::lock_guard lock(g_mutex);
    memset(GuestPtr(c), 0, kContextSize);
    auto in = [&](uint32_t off) { return LoadBE32(init + off); };
    StoreBE32(c + 5 * 4, in(0x00));
    SetField(c, 0, 0, 12, in(0x04));
    StoreBE32(c + 6 * 4, in(0x08));
    SetField(c, 1, 0, 12, in(0x0C));
    SetField(c, 2, 0, 26, in(0x10));
    StoreBE32(c + 7 * 4, in(0x14));
    SetField(c, 0, 22, 5, in(0x18));
    StoreBE32(c + 8 * 4, in(0x1C));
    SetField(c, 1, 20, 4, in(0x20));
    SetField(c, 1, 29, 1, in(0x24) >= 1);
    SetField(c, 1, 27, 2, in(0x28));
    SetField(c, 3, 0, 26, in(0x2C));
    SetField(c, 4, 0, 26, in(0x30));
    SetField(c, 0, 12, 8, *GuestPtr<uint8_t>(init + 0x34));
    if (g_traceXma)
        Log("xma: init %08X in0 %08X x%u in1 %08X x%u offset %u out %08X blocks %u ch %u rate %u loop %u-%u x%u", c, in(0x00), in(0x04), in(0x08), in(0x0C),
            in(0x10), in(0x14), in(0x18), in(0x24), in(0x28), in(0x2C), in(0x30), *GuestPtr<uint8_t>(init + 0x34));
    if (auto it = g_decoders.find(c); it != g_decoders.end())
    {
        if (g_traceXma)
            Log("xma: re-init %08X: %zu packets decoded, %zu frames decoded, %zu delivered, %zu discarded unread", c, it->second.packetsDecoded,
                it->second.framesDecoded, it->second.framesWritten, (it->second.pcm.size() - it->second.pcmRead) / std::max<uint32_t>(it->second.channels, 1));
        it->second.packetsDecoded = it->second.framesDecoded = it->second.framesWritten = 0;
        it->second.consumed[0] = it->second.consumed[1] = false;
        it->second.holdsTail = false;
        it->second.flush = true;
        it->second.pcm.clear();
        it->second.pcmRead = 0;
    }
    Return(ctx, 0);
}

GUEST_EXPORT(XMAEnableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: enable %08X (read offset %u, in0 %u in1 %u valid, current %u) loops %u start %u end %u, subframes: end %u skip %u decode %u, packets %u/%u",
            ctx.r3.u32, Field(ctx.r3.u32, 2, 0, 26), Field(ctx.r3.u32, 0, 20, 1), Field(ctx.r3.u32, 0, 21, 1), Field(ctx.r3.u32, 4, 31, 1),
            Field(ctx.r3.u32, 0, 12, 8), Field(ctx.r3.u32, 3, 0, 26), Field(ctx.r3.u32, 4, 0, 26), Field(ctx.r3.u32, 1, 14, 3), Field(ctx.r3.u32, 1, 17, 3),
            Field(ctx.r3.u32, 1, 20, 4), Field(ctx.r3.u32, 0, 0, 12), Field(ctx.r3.u32, 1, 0, 12));
    g_enabled.insert(ctx.r3.u32);
    // FM2_TRACE_XMA_ENABLE=1: once a second, the time spent here, for streams starting and running.
    static LiveSwitch traceEnable("FM2_TRACE_XMA_ENABLE");
    const auto since = Clock::now();
    const bool starting = g_decoders[ctx.r3.u32].flush;
    while (Step(ctx.r3.u32))
        ;
    if (traceEnable)
    {
        static double ms[2], longest[2];
        static uint32_t calls[2];
        static Clock::time_point last = Clock::now();
        const double t = std::chrono::duration<double, std::milli>(Clock::now() - since).count();
        ms[starting] += t, longest[starting] = std::max(longest[starting], t), calls[starting]++;
        if (Clock::now() - last >= std::chrono::seconds(1))
        {
            Log("xma: 1 s of enables: running %u calls %.2f ms (longest %.2f), starting %u calls %.2f ms (longest %.2f), %u waits for a codec", calls[0], ms[0],
                longest[0], calls[1], ms[1], longest[1], g_codecWaits.exchange(0));
            last = Clock::now(), ms[0] = ms[1] = longest[0] = longest[1] = 0, calls[0] = calls[1] = 0;
        }
    }
    Return(ctx, 0);
}

GUEST_EXPORT(XMADisableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: disable %08X", ctx.r3.u32);
    g_enabled.erase(ctx.r3.u32);
    Return(ctx, 0);
}

#define XMA_FIELD_EXPORTS(Name, dword, lo, bits)                     \
    GUEST_EXPORT(XMAGet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, lo, bits));             \
    }                                                                \
    GUEST_EXPORT(XMASet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        SetField(ctx.r3.u32, dword, lo, bits, ctx.r4.u32);           \
        Return(ctx, 0);                                              \
    }

GUEST_EXPORT(XMAGetInputBufferReadOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 2, 0, 26));
}

GUEST_EXPORT(XMASetInputBufferReadOffset)
{
    // A seek: the decoder starts over at the new packet.
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: seek %08X to offset %u (was %u), in0 %08X x%u in1 %08X x%u, out %08X blocks %u write %u read %u, skip %u", ctx.r3.u32, ctx.r4.u32,
            Field(ctx.r3.u32, 2, 0, 26), LoadBE32(ctx.r3.u32 + 20), Field(ctx.r3.u32, 0, 0, 12), LoadBE32(ctx.r3.u32 + 24), Field(ctx.r3.u32, 1, 0, 12),
            LoadBE32(ctx.r3.u32 + 28), Field(ctx.r3.u32, 0, 22, 5), Field(ctx.r3.u32, 0, 27, 5), Field(ctx.r3.u32, 9, 0, 5), Field(ctx.r3.u32, 1, 17, 3));
    SetField(ctx.r3.u32, 2, 0, 26, ctx.r4.u32);
    // The title restarts the decoder at the first frame of each next pair of packets (a seek
    // within a stream it reads in order). The codec keeps its overlap and its unfinished frame
    // across that, as the hardware does; resetting it dropped most of every pair's audio.
    static LiveSwitch flushOnSeek("FM2_XMA_SEEK_FLUSH");
    if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
    {
        it->second.seeked = true;
        if (flushOnSeek)
            it->second.flush = true;
    }
    Return(ctx, 0);
}
XMA_FIELD_EXPORTS(OutputBufferReadOffset, 9, 0, 5)

GUEST_EXPORT(XMAGetOutputBufferWriteOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 0, 27, 5));
}

#define XMA_FLAG_EXPORTS(Name, dword, bit)                           \
    GUEST_EXPORT(XMAIs##Name##Valid)                                 \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, bit, 1));               \
    }                                                                \
    GUEST_EXPORT(XMASet##Name##Valid)                                \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        if (g_traceXma)                                                          Log("xma: %08X " #Name " valid", ctx.r3.u32);                    SetField(ctx.r3.u32, dword, bit, 1, 1);                              ClearConsumed(ctx.r3.u32, dword, bit);                       \
        Return(ctx, 0);                                              \
    }

XMA_FLAG_EXPORTS(InputBuffer0, 0, 20)
XMA_FLAG_EXPORTS(InputBuffer1, 0, 21)
XMA_FLAG_EXPORTS(OutputBuffer, 1, 31)

// A read from a music file (imports_io.cpp): its packets' hashes, located from the file's RIFF data chunk.
void AudioNoteMusicRead(const std::filesystem::path& file, uint64_t offset, const uint8_t* data, uint32_t bytes)
{
    static std::map<std::filesystem::path, uint64_t> dataStart;
    std::lock_guard lock(g_musicMutex);
    auto it = dataStart.find(file);
    if (it == dataStart.end())
    {
        uint64_t start = 0;
        if (FILE* f = _wfopen(file.c_str(), L"rb"))
        {
            uint8_t head[4096] = {};
            const size_t n = fread(head, 1, sizeof(head), f);
            fclose(f);
            for (size_t i = 12; i + 8 <= n; i++)
                if (!memcmp(head + i, "data", 4))
                {
                    start = i + 8;
                    break;
                }
        }
        it = dataStart.emplace(file, start).first;
    }
    if (!it->second)
        return;
    const uint64_t first = it->second + (offset > it->second ? (offset - it->second + kPacketBytes - 1) / kPacketBytes * kPacketBytes : 0);
    for (uint64_t p = first; p + 64 <= offset + bytes; p += kPacketBytes)
        g_musicPackets.insert(PacketHash(data + (p - offset)));
}
