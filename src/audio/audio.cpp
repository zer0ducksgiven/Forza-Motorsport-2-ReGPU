// Audio output: the render driver the title's audio engine (FMOD) mixes into. The
// console calls the registered callback every 256 samples at 48 kHz (5.33 ms); the engine
// answers with XAudioSubmitRenderDriverFrame: 6 channels (5.1) of 256 big-endian floats,
// one channel after another. Frames go to an XAudio2 source voice (Windows downmixes for
// stereo devices). The callback runs on a 5.33 ms clock nudged by how many frames the
// voice has queued, so the title's audio clock follows the device instead of drifting.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <timeapi.h>
#include <xaudio2.h>

#include "kernel/imports.h"
#include "kernel/thread.h"
#include "live_switch.h"
#include "log.h"

namespace
{
constexpr uint32_t kChannels = 6, kFrameSamples = 256, kRate = 48000;
constexpr uint32_t kRingFrames = 32;
// Frames kept queued at the device: about 21 ms. Fewer risks underruns, more adds latency.
constexpr uint32_t kTargetQueued = 4, kMaxQueued = 8;

std::atomic<uint32_t> g_callback{ 0 };
std::atomic<uint32_t> g_callbackArg{ 0 };
std::shared_ptr<ThreadObject> g_audioThread;
// FM2_TRACE_AUDIO=1: once a second, the callbacks run, frames submitted, dropped and repeated.
std::atomic<uint32_t> g_statCallbacks{ 0 }, g_statSubmitted{ 0 }, g_statDropped{ 0 }, g_statSkipped{ 0 }, g_statRepeated{ 0 };

struct Output : IXAudio2VoiceCallback
{
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    std::vector<float> ring = std::vector<float>(size_t(kRingFrames) * kFrameSamples * kChannels);
    uint32_t next = 0;
    std::atomic<int> queued{ 0 };
    bool ok = false;

    void STDMETHODCALLTYPE OnBufferEnd(void*) override { queued--; }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { Log("audio: voice error %08lX", (unsigned long)hr); }

    void Init()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master)))
        {
            Log("audio: no output device; sound is discarded");
            return;
        }
        WAVEFORMATEXTENSIBLE f{};
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = kChannels;
        f.Format.nSamplesPerSec = kRate;
        f.Format.wBitsPerSample = 32;
        f.Format.nBlockAlign = kChannels * 4;
        f.Format.nAvgBytesPerSec = kRate * f.Format.nBlockAlign;
        f.Format.cbSize = sizeof(f) - sizeof(WAVEFORMATEX);
        f.Samples.wValidBitsPerSample = 32;
        f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT |
            SPEAKER_BACK_RIGHT;
        f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (FAILED(xaudio->CreateSourceVoice(&voice, &f.Format, 0, 2.0f, this)) || FAILED(voice->Start()))
        {
            Log("audio: could not create the output voice; sound is discarded");
            return;
        }
        ok = true;
        Log("audio: output 5.1 float at %u Hz", kRate);
    }

    // One title frame: planar big-endian floats to interleaved host floats.
    void Submit(uint32_t samples)
    {
        g_statSubmitted++;
        if (!ok || queued.load() >= int(kRingFrames) - 1)
        {
            g_statDropped++;
            return;
        }
        float* out = &ring[size_t(next) * kFrameSamples * kChannels];
        const uint32_t* in = GuestPtr<uint32_t>(samples);
        // A frame the same as 4 frames ago: FMOD's mixer was late and the driver replayed its last
        // 1024 samples (heard as stretched, crusty sound). FM2_TRACE_AUDIO counts them in its line;
        // FM2_TRACE_AUDIO_REPEAT=1 logs what every title thread is doing at the first 40.
        static LiveSwitch traceRepeat("FM2_TRACE_AUDIO_REPEAT"), traceAudio("FM2_TRACE_AUDIO");
        if (traceRepeat || traceAudio)
        {
            static std::vector<uint32_t> last(size_t(4) * kFrameSamples * kChannels);
            static uint32_t slot = 0, dumps = 0;
            static Clock::time_point lastDump;
            uint32_t* prev = &last[size_t(slot) * kFrameSamples * kChannels];
            bool sound = false;
            for (uint32_t i = 0; i < kFrameSamples * kChannels && !sound; i++)
                sound = in[i] != 0;
            const bool same = sound && memcmp(prev, in, size_t(kFrameSamples) * kChannels * 4) == 0;
            memcpy(prev, in, size_t(kFrameSamples) * kChannels * 4);
            slot = (slot + 1) % 4;
            g_statRepeated += same;
            if (traceRepeat && same && dumps < 40 && Clock::now() - lastDump > std::chrono::milliseconds(300))
            {
                dumps++, lastDump = Clock::now();
                Log("audio: frame repeated (mixer late), threads:");
                LogGuestThreads();
            }
        }
        for (uint32_t c = 0; c < kChannels; c++)
            for (uint32_t i = 0; i < kFrameSamples; i++)
            {
                const uint32_t bits = __builtin_bswap32(in[c * kFrameSamples + i]);
                memcpy(&out[i * kChannels + c], &bits, 4);
            }
        // FM2_DUMP_AUDIO=<file.wav>: everything the title submits, as 6-channel float WAV
        // (header sizes left at their maximum; players read to the end).
        static FILE* dump = [] {
            const char* path = getenv("FM2_DUMP_AUDIO");
            FILE* f = path ? fopen(path, "wb") : nullptr;
            if (f)
            {
                const uint32_t rate = kRate, bytesPerSec = kRate * kChannels * 4, size = 0xFFFFFFF0u;
                const uint16_t fmt = 3, ch = kChannels, align = kChannels * 4, bits = 32;
                const uint32_t fmtSize = 16;
                fwrite("RIFF", 1, 4, f), fwrite(&size, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmtSize, 4, 1, f);
                fwrite(&fmt, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&bytesPerSec, 4, 1, f);
                fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&size, 4, 1, f);
            }
            return f;
        }();
        if (dump)
        {
            fwrite(out, 4, size_t(kFrameSamples) * kChannels, dump);
            fflush(dump);
        }
        XAUDIO2_BUFFER b{};
        b.AudioBytes = kFrameSamples * kChannels * 4;
        b.pAudioData = reinterpret_cast<const BYTE*>(out);
        queued++;
        if (FAILED(voice->SubmitSourceBuffer(&b)))
            queued--;
        next = (next + 1) % kRingFrames;
    }
} g_output;

void AudioLoop(PPCContext& ctx)
{
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    auto next = Clock::now();
    static LiveSwitch trace("FM2_TRACE_AUDIO");
    auto lastStat = Clock::now();
    for (;;)
    {
        next += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(next);
        const int queued = g_output.queued.load();
        if (trace && Clock::now() - lastStat >= std::chrono::seconds(1))
        {
            lastStat = Clock::now();
            Log("audio: 1 s: %u callbacks, %u frames submitted, %u dropped, %u repeated, %u ticks skipped, queued %d, loop %.0f ms behind", g_statCallbacks.exchange(0),
                g_statSubmitted.exchange(0), g_statDropped.exchange(0), g_statRepeated.exchange(0), g_statSkipped.exchange(0), queued,
                std::chrono::duration<double, std::milli>(Clock::now() - next).count());
        }
        if (g_output.ok && queued > int(kMaxQueued))
        {
            g_statSkipped++;
            continue; // the device is behind: skip a tick
        }
        const uint32_t cb = g_callback;
        if (!cb)
            continue;
        ctx.r3.u64 = g_callbackArg;
        g_statCallbacks++;
        GuestCall(ctx, cb);
        // The device is about to run dry: one extra tick now.
        if (g_output.ok && queued < int(kTargetQueued) / 2)
        {
            ctx.r3.u64 = g_callbackArg;
            g_statCallbacks++;
            GuestCall(ctx, cb);
        }
    }
}
}

void AudioInit() {}

GUEST_EXPORT(XAudioRegisterRenderDriverClient)
{
    // (callback struct { u32 fn, u32 arg }*, driver*)
    const uint32_t callback = ctx.r3.u32;
    g_callback = LoadBE32(callback);
    g_callbackArg = LoadBE32(callback + 4);
    StoreIfNonNull(ctx.r4.u32, 0x41550000);
    if (!g_audioThread)
    {
        g_output.Init();
        g_audioThread = CreateHostGuestThread("audio driver", AudioLoop);
    }
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioSubmitRenderDriverFrame)
{
    // (driver, samples*)
    if (ctx.r4.u32)
        g_output.Submit(ctx.r4.u32);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    StoreIfNonNull(ctx.r4.u32, 0);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioGetVoiceCategoryVolume)
{
    const float one = 1.0f;
    uint32_t bits;
    memcpy(&bits, &one, 4);
    StoreIfNonNull(ctx.r4.u32, bits);
    Return(ctx, X_STATUS_SUCCESS);
}
