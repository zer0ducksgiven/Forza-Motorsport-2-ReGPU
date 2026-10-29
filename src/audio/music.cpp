// In-race music: the player's MP3s (Documents\fm2\music, subfolders too) in shuffled order while
// they drive, like a radio. The race flows' HUD scene (Media\UI\Flows\*Race*.xml) says when: its
// onEnter (the race starts or comes back from pause) and onExit (pause, or the race is over) get a
// note each (MusicLuaSource), which fade the music in and out. A track picks up where it stopped,
// and the next starts when it ends. Decoded with FFmpeg's MP3 parser and decoder (avcodec, as the
// XMA audio is) to a stereo float XAudio2 voice of its own at the track's rate (XAudio2 resamples),
// apart from the game's 5.1 output (audio.cpp). Settings: race_music, music_volume. As a track starts, or
// the music comes back after a pause, its title and artist (the ID3 tags, else the filename) show in a
// toast at the bottom of the screen (MusicToast, drawn by gpu/renderer.cpp). On the pad, D-pad right and left
// skip forward and back, D-pad down pauses (MusicPad).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <xaudio2.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "log.h"
#include "settings.h"
#include "user_dir.h"

namespace
{
constexpr uint32_t kBuffers = 4;           // queued at the voice: each about a quarter of a second
constexpr uint32_t kBufferFrames = 12000;  // stereo frames per buffer
constexpr float kFadeSeconds = 0.8f;

std::atomic<bool> g_driving{ false }; // the HUD is up
std::once_flag g_start;
std::atomic<bool> g_started{ false };
std::atomic<bool> g_paused{ false }; // the player's pause (D-pad down), apart from the game's
std::atomic<int> g_skip{ 0 };        // tracks to move by (D-pad right +1, left -1), not yet done

std::mutex g_toastMutex;
std::wstring g_title, g_artist; // the playing track's
std::chrono::steady_clock::time_point g_toastSince{};

void ShowToast()
{
    std::lock_guard lock(g_toastMutex);
    g_toastSince = std::chrono::steady_clock::now();
}

// An ID3 text frame's bytes (encoding byte first) as text.
std::wstring Id3Text(const uint8_t* p, size_t n)
{
    std::wstring out;
    if (n == 0)
        return out;
    const uint8_t encoding = p[0];
    p++, n--;
    if (encoding == 1 || encoding == 2) // UTF-16 with a byte order mark, or big-endian
    {
        bool big = encoding == 2;
        if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)))
            big = p[0] == 0xFE, p += 2, n -= 2;
        for (size_t i = 0; i + 1 < n; i += 2)
            out += wchar_t(big ? p[i] << 8 | p[i + 1] : p[i + 1] << 8 | p[i]);
    }
    else if (encoding == 3) // UTF-8
    {
        const int w = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), int(n), nullptr, 0);
        out.resize(size_t(std::max(w, 0)));
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), int(n), out.data(), w);
    }
    else // Latin-1
        for (size_t i = 0; i < n; i++)
            out += wchar_t(p[i]);
    out.erase(std::find(out.begin(), out.end(), L'\0'), out.end());
    return out;
}

// Title and artist from an ID3v2.3/2.4 tag (TIT2, TPE1), or ID3v1 at the end.
void ReadTags(const std::vector<uint8_t>& d, std::wstring& title, std::wstring& artist)
{
    if (d.size() > 10 && memcmp(d.data(), "ID3", 3) == 0 && (d[3] == 3 || d[3] == 4))
    {
        const size_t end = std::min(d.size(), 10 + (size_t(d[6] & 0x7F) << 21 | size_t(d[7] & 0x7F) << 14 | size_t(d[8] & 0x7F) << 7 | (d[9] & 0x7F)));
        for (size_t at = 10; at + 10 <= end && d[at];)
        {
            const uint8_t* f = d.data() + at;
            const size_t size = d[3] == 4 ? (size_t(f[4] & 0x7F) << 21 | size_t(f[5] & 0x7F) << 14 | size_t(f[6] & 0x7F) << 7 | (f[7] & 0x7F))
                                          : (size_t(f[4]) << 24 | size_t(f[5]) << 16 | size_t(f[6]) << 8 | f[7]);
            if (at + 10 + size > end)
                break;
            if (memcmp(f, "TIT2", 4) == 0)
                title = Id3Text(f + 10, size);
            else if (memcmp(f, "TPE1", 4) == 0)
                artist = Id3Text(f + 10, size);
            at += 10 + size;
        }
    }
    if ((title.empty() || artist.empty()) && d.size() >= 128 && memcmp(d.data() + d.size() - 128, "TAG", 3) == 0)
    {
        auto field = [&](size_t off) {
            std::wstring w;
            for (size_t i = 0; i < 30 && d[d.size() - 128 + off + i]; i++)
                w += wchar_t(d[d.size() - 128 + off + i]);
            while (!w.empty() && w.back() == L' ')
                w.pop_back();
            return w;
        };
        if (title.empty())
            title = field(3);
        if (artist.empty())
            artist = field(33);
    }
}

struct Voice : IXAudio2VoiceCallback
{
    std::atomic<int> queued{ 0 };
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { queued--; }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { Log("music: voice error %08lX", (unsigned long)hr); }
};

// One MP3 decoded a buffer at a time: stereo float frames.
struct Track
{
    std::vector<uint8_t> data;
    size_t at = 0;
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::vector<float> pending; // decoded, not yet handed out
    bool flushed = false;
    uint32_t rate = 0;
    std::wstring title, artist;

    ~Track()
    {
        av_parser_close(parser);
        avcodec_free_context(&codec);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    bool Open(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), {});
        ReadTags(data, title, artist);
        if (title.empty()) // "Artist - Title.mp3"
        {
            title = path.stem().wstring();
            if (const size_t dash = title.find(L" - "); artist.empty() && dash != title.npos)
                artist = title.substr(0, dash), title = title.substr(dash + 3);
        }
        // an ID3v2 tag first ("ID3", version, flags, a 28-bit size in 7-bit bytes): skipped, so its bytes
        // aren't taken for frames
        if (data.size() > 10 && memcmp(data.data(), "ID3", 3) == 0)
            at = 10 + (size_t(data[6] & 0x7F) << 21 | size_t(data[7] & 0x7F) << 14 | size_t(data[8] & 0x7F) << 7 | (data[9] & 0x7F));
        const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MP3);
        parser = av_parser_init(AV_CODEC_ID_MP3);
        codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        return at < data.size() && parser && codec && packet && frame && avcodec_open2(codec, decoder, nullptr) == 0;
    }

    // Decoded frames appended to pending (as stereo); false once the file is done.
    bool DecodeSome()
    {
        while (true)
        {
            if (avcodec_receive_frame(codec, frame) == 0)
            {
                Append();
                return true;
            }
            if (flushed)
                return false;
            if (at >= data.size())
            {
                avcodec_send_packet(codec, nullptr); // the decoder's last frames
                flushed = true;
                continue;
            }
            uint8_t* out = nullptr;
            int outSize = 0;
            const int used = av_parser_parse2(parser, codec, &out, &outSize, data.data() + at, int(std::min<size_t>(data.size() - at, 1 << 16)),
                                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0)
                return false;
            at += size_t(used);
            if (outSize > 0)
            {
                packet->data = out;
                packet->size = outSize;
                avcodec_send_packet(codec, packet);
            }
        }
    }

    void Append()
    {
        const int n = frame->nb_samples, channels = frame->ch_layout.nb_channels;
        if (n <= 0 || channels <= 0)
            return;
        rate = uint32_t(frame->sample_rate);
        auto sample = [&](int ch, int i) -> float {
            ch = std::min(ch, channels - 1);
            switch (frame->format)
            {
            case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float*>(frame->extended_data[ch])[i];
            case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(frame->data[0])[i * channels + ch];
            case AV_SAMPLE_FMT_S16P: return reinterpret_cast<const int16_t*>(frame->extended_data[ch])[i] / 32768.0f;
            case AV_SAMPLE_FMT_S16: return reinterpret_cast<const int16_t*>(frame->data[0])[i * channels + ch] / 32768.0f;
            default: return 0.0f;
            }
        };
        for (int i = 0; i < n; i++)
        {
            pending.push_back(sample(0, i));
            pending.push_back(sample(1, i));
        }
    }
};

std::vector<std::filesystem::path> Playlist()
{
    std::vector<std::filesystem::path> tracks;
    std::error_code ec;
    const auto dir = DocumentsUserDir() / "music";
    std::filesystem::create_directories(dir, ec);
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
    {
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(tolower(c)); });
        if (it->is_regular_file(ec) && ext == ".mp3")
            tracks.push_back(it->path());
    }
    std::shuffle(tracks.begin(), tracks.end(), std::mt19937(std::random_device{}()));
    return tracks;
}

void Run()
{
    const Settings& s = GetSettings();
    const float volume = std::clamp(s.musicVolume, 0u, 100u) / 100.0f;
    std::vector<std::filesystem::path> list = Playlist();
    Log("music: %zu MP3%s in Documents\\fm2\\music (volume %u%%)", list.size(), list.size() == 1 ? "" : "s", s.musicVolume);
    if (list.empty())
        return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master)))
    {
        Log("music: no output device");
        return;
    }
    Voice callback;
    IXAudio2SourceVoice* voice = nullptr;
    uint32_t voiceRate = 0, next = 0, failed = 0;
    std::unique_ptr<Track> track;
    std::vector<std::vector<float>> buffers(kBuffers);
    uint32_t buffer = 0;
    float level = 0.0f; // the fade, 0..1
    bool running = false;
    auto last = std::chrono::steady_clock::now();
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        const bool driving = g_driving && !g_paused;
        if (const int skip = g_skip.exchange(0); skip && (track || voice))
        {
            // the next or previous track at once: what is queued of this one dropped
            const uint32_t n = uint32_t(list.size());
            if (skip < 0)
                next = (next % n + 2 * n - 2) % n; // back past the playing one to the one before it
            track.reset();
            if (voice)
            {
                voice->Stop();
                voice->FlushSourceBuffers();
                for (int i = 0; i < 50 && callback.queued > 0; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (running)
                    voice->Start();
            }
        }
        level = std::clamp(level + (driving ? dt : -dt) / kFadeSeconds, 0.0f, 1.0f);
        if (voice)
        {
            voice->SetVolume(level * level * volume);
            if (level == 0.0f && running)
            {
                voice->Stop();
                running = false;
                Log("music: stopped");
            }
            else if (level > 0.0f && !running)
            {
                voice->Start();
                running = true;
                Log("music: playing");
                ShowToast();
            }
        }
        if (!driving && level == 0.0f)
            continue;
        // keep the voice fed: the next buffer decoded while fewer than kBuffers are queued
        while (callback.queued < int(kBuffers))
        {
            if (!track)
            {
                if (failed >= list.size())
                    return; // nothing in the folder plays
                const std::filesystem::path path = list[next++ % list.size()];
                if (next % list.size() == 0)
                    std::shuffle(list.begin(), list.end(), std::mt19937(std::random_device{}()));
                track = std::make_unique<Track>();
                if (!track->Open(path))
                {
                    Log("music: can't play %s", path.filename().string().c_str());
                    track.reset();
                    failed++;
                    continue;
                }
                failed = 0;
                Log("music: %s", path.filename().string().c_str());
                {
                    std::lock_guard lock(g_toastMutex);
                    g_title = track->title;
                    g_artist = track->artist;
                }
                if (running)
                    ShowToast(); // the first track shows when its voice starts
            }
            std::vector<float>& out = buffers[buffer];
            while (track->pending.size() < size_t(kBufferFrames) * 2 && track->DecodeSome())
                ;
            const size_t take = std::min(track->pending.size(), size_t(kBufferFrames) * 2);
            if (take == 0)
            {
                track.reset(); // the track is over: the next one
                continue;
            }
            if (track->rate != voiceRate)
            {
                // a voice at the track's rate (XAudio2 resamples to the device), once nothing of the old one is queued
                if (voice && callback.queued > 0)
                    break;
                if (voice)
                    voice->DestroyVoice();
                voice = nullptr;
                WAVEFORMATEX f{};
                f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                f.nChannels = 2;
                f.nSamplesPerSec = track->rate;
                f.wBitsPerSample = 32;
                f.nBlockAlign = 8;
                f.nAvgBytesPerSec = track->rate * 8;
                if (FAILED(xaudio->CreateSourceVoice(&voice, &f, 0, 2.0f, &callback)))
                {
                    Log("music: no voice at %u Hz", track->rate);
                    return;
                }
                voiceRate = track->rate;
                voice->SetVolume(level * level * volume);
                running = false;
                if (level > 0.0f)
                {
                    voice->Start();
                    running = true;
                    ShowToast(); // the first track: its voice starts here, not at "playing"
                }
            }
            out.assign(track->pending.begin(), track->pending.begin() + std::ptrdiff_t(take));
            track->pending.erase(track->pending.begin(), track->pending.begin() + std::ptrdiff_t(take));
            XAUDIO2_BUFFER b{};
            b.AudioBytes = UINT32(out.size() * sizeof(float));
            b.pAudioData = reinterpret_cast<const BYTE*>(out.data());
            callback.queued++;
            if (FAILED(voice->SubmitSourceBuffer(&b)))
                callback.queued--;
            buffer = (buffer + 1) % kBuffers;
        }
    }
}
}

// The race flows' HUD scene notes (hooks/lua.cpp): "--music: drive" as it shows, "--music: stop" as it goes.
bool MusicLuaNote(const char* text, uint32_t size)
{
    static constexpr std::string_view kPrefix = "--music: ";
    const std::string_view note(text, size);
    if (note.rfind(kPrefix, 0) != 0)
        return false;
    if (!GetSettings().raceMusic)
        return true;
    const bool drive = note.substr(kPrefix.size()).rfind("drive", 0) == 0;
    g_driving = drive;
    if (drive)
        std::call_once(g_start, [] {
            g_started = true;
            std::thread(Run).detach();
        });
    return true;
}

// For hooks/lua.cpp, as a flow's Lua loads: the HUD scene's onEnter / onExit with a music note after them, or empty.
std::string MusicLuaSource(const char* text, uint32_t size)
{
    static constexpr std::string_view kIn = "UI.Storage.FadeHUDIn = true", kOut = "UI.Storage.FadeHUDOut = true";
    const std::string_view in(text, size);
    if (in.find(kIn) != in.npos)
        return std::string(in) + "\nloadstring(\"--music: drive\")\n";
    if (in.find(kOut) != in.npos)
        return std::string(in) + "\nloadstring(\"--music: stop\")\n";
    return {};
}

// For gpu/renderer.cpp: the playing track's title and artist while its toast is up (seconds since it showed in
// `age`), or false.
bool MusicToast(std::wstring& title, std::wstring& artist, float& age)
{
    std::lock_guard lock(g_toastMutex);
    if (g_toastSince == std::chrono::steady_clock::time_point{} || g_title.empty())
        return false;
    age = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_toastSince).count();
    if (age > 6.0f)
        return false;
    title = g_title;
    artist = g_artist;
    return true;
}

// For the pad reader (kernel/imports_xam.cpp), while the music plays in a race: D-pad right and left move to the
// next and previous track, D-pad down pauses it and starts it again. Those presses are kept from the game.
void MusicPad(uint16_t& buttons)
{
    constexpr uint16_t kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008; // XINPUT_GAMEPAD_DPAD_*
    static std::atomic<uint16_t> held{ 0 };
    if (!g_started || !g_driving)
    {
        held = 0;
        return;
    }
    const uint16_t now = buttons & (kDown | kLeft | kRight), pressed = now & ~held.exchange(now);
    if (pressed & kRight)
        g_skip++;
    if (pressed & kLeft)
        g_skip--;
    if (pressed & kDown)
        Log("music: %s", (g_paused = !g_paused) ? "paused" : "resumed");
    buttons &= ~(kDown | kLeft | kRight);
}
