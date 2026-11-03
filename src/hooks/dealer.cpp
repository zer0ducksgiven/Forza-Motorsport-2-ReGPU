// Easter egg: type "whysthisdealer" in the game window and every car bounces to Niko B's "Why's This
// Dealer?" (the bouncing-Yaris meme) until the game is closed, the song looping all the while.
//
// The song isn't ours to ship: put your own copy in Documents\fm2\easter\ (any format Windows decodes: mp3,
// m4a, wav, ...; the first file there), or point FM2_DEALER_SONG at one. It's decoded once with Media
// Foundation, played on a voice of its own, and its beats are found (Analyse: tempo and phase from the kick
// drum, each beat snapped to its kick). On every beat the car bodies flatten hard and ease back up
// (DealerSquash), timed from the voice's playback position each frame
// (FM2_DEALER_OFFSET_MS shifts it for audio/display latency, default 0). Every mesh of a car takes it in
// clip space (DealerCarDraw; renderer.cpp AddCarSquash, vsquash).
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel/runtime.h"
#include "log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "xaudio2.lib")

namespace fs = std::filesystem;

namespace
{
constexpr char kCode[] = "whysthisdealer";
constexpr double kSquash = 0.3;  // flattest 70% of the height on the beat, tallest 130% between
constexpr uint32_t kHopHz = 200; // bounce curve resolution (5 ms)

std::atomic<bool> g_on{ false };
uint32_t g_rate = 0, g_frames = 0;
IXAudio2SourceVoice* g_voice = nullptr;
double g_offset = 0; // seconds

fs::path FindSong()
{
    if (const char* p = getenv("FM2_DEALER_SONG"); p && *p)
        return p;
    std::error_code ec;
    const fs::path dir = UserDir() / "easter";
    fs::create_directories(dir, ec);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec))
            return e.path();
    return {};
}

// The whole song as interleaved float stereo.
bool Decode(const fs::path& file, std::vector<float>& pcm, uint32_t& rate)
{
    if (FAILED(MFStartup(MF_VERSION)))
        return false;
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader)))
        return false;
    IMFMediaType* type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    HRESULT hr = reader->SetCurrentMediaType(stream, nullptr, type);
    type->Release();
    IMFMediaType* actual = nullptr;
    if (SUCCEEDED(hr))
        hr = reader->GetCurrentMediaType(stream, &actual);
    UINT32 channels = 2;
    if (SUCCEEDED(hr))
    {
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        actual->Release();
    }
    while (SUCCEEDED(hr))
    {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;
        if (!sample)
            continue;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
        {
            BYTE* data = nullptr;
            DWORD bytes = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &bytes)))
            {
                const float* f = reinterpret_cast<const float*>(data);
                const size_t n = bytes / sizeof(float) / channels;
                for (size_t i = 0; i < n; i++)
                {
                    pcm.push_back(f[i * channels]);
                    pcm.push_back(channels > 1 ? f[i * channels + 1] : f[i * channels]);
                }
                buffer->Unlock();
            }
            buffer->Release();
        }
        sample->Release();
    }
    reader->Release();
    return rate && !pcm.empty();
}

// The beats, from the kick drum: a 40-150 Hz band's energy every 5 ms; its rises (onsets); the tempo from
// their autocorrelation (70-180 BPM) and the phase that lines a grid up with them; each grid beat moved to
// the strongest onset within 25 ms of it, and kept only where there's a kick (not in quiet intros or
// breaks). Strength 0.6-1 per beat (how hard it hits).
struct Beat
{
    double time;
    float strength;
};
std::vector<Beat> g_beats;

std::vector<Beat> Analyse(const std::vector<float>& pcm, uint32_t rate)
{
    const size_t frames = pcm.size() / 2, hop = std::max<size_t>(1, rate / kHopHz);
    auto alpha = [&](double hz) { return float(1.0 - std::exp(-2.0 * 3.14159265358979 * hz / rate)); };
    const float aHi = alpha(150), aLo = alpha(40);
    float lp1 = 0, lp2 = 0, lo = 0;
    std::vector<float> energy;
    double sum = 0;
    for (size_t i = 0; i < frames; i++)
    {
        const float x = 0.5f * (pcm[i * 2] + pcm[i * 2 + 1]);
        lp1 += aHi * (x - lp1);
        lp2 += aHi * (lp1 - lp2);
        lo += aLo * (lp2 - lo);
        const float band = lp2 - lo;
        sum += double(band) * band;
        if ((i + 1) % hop == 0)
        {
            energy.push_back(float(std::log(1e-7 + std::sqrt(sum / hop))));
            sum = 0;
        }
    }
    const size_t n = energy.size();
    std::vector<float> onset(n, 0.0f);
    for (size_t i = 4; i < n; i++)
    {
        const float before = 0.25f * (energy[i - 1] + energy[i - 2] + energy[i - 3] + energy[i - 4]);
        onset[i] = std::max(0.0f, energy[i] - before);
    }
    std::vector<Beat> beats;
    if (n < kHopHz * 4)
        return beats;
    // Tempo: the lag (in hops) whose autocorrelation is strongest.
    const size_t minLag = kHopHz * 60 / 180, maxLag = kHopHz * 60 / 70;
    double best = -1;
    size_t lag = minLag;
    for (size_t l = minLag; l <= maxLag; l++)
    {
        double r = 0;
        for (size_t i = l; i < n; i++)
            r += double(onset[i]) * onset[i - l];
        if (r > best)
            best = r, lag = l;
    }
    // Phase: the grid offset that lands on the most onset.
    best = -1;
    size_t phase = 0;
    for (size_t ph = 0; ph < lag; ph++)
    {
        double r = 0;
        for (size_t i = ph; i < n; i += lag)
            r += onset[i];
        if (r > best)
            best = r, phase = ph;
    }
    // Follow the song: each beat is predicted from the last (one period on), moved to the strongest onset
    // within 60 ms if there's a clear one, and the period eases towards what was found, so a tempo that is a
    // fraction off (or drifts) never slides off the kicks.
    std::vector<float> sortedOnset(onset);
    std::sort(sortedOnset.begin(), sortedOnset.end());
    const float clear = std::max(sortedOnset[sortedOnset.size() * 98 / 100], 1e-6f);
    const double window = 0.06 * kHopHz;
    double period = double(lag), at = double(phase);
    std::vector<float> peaks;
    while (at < double(n))
    {
        size_t peak = size_t(at);
        for (size_t k = size_t(std::max(0.0, at - window)); k < std::min(n, size_t(at + window) + 1); k++)
            if (onset[k] > onset[peak])
                peak = k;
        double beat = at;
        if (onset[peak] > 0.5f * clear)
        {
            if (!beats.empty())
                period += 0.15 * ((double(peak) - beats.back().time * kHopHz) - period);
            beat = double(peak);
        }
        period = std::clamp(period, double(lag) * 0.97, double(lag) * 1.03);
        beats.push_back({ beat / kHopHz, onset[std::min(n - 1, size_t(beat))] });
        peaks.push_back(beats.back().strength);
        at = beat + period;
    }
    std::sort(peaks.begin(), peaks.end());
    const float top = std::max(peaks[peaks.size() * 9 / 10], 1e-6f);
    // Every beat of the tracked grid squashes (a constant pulse, quiet passages included).
    std::vector<Beat> kept;
    for (const Beat& b : beats)
        kept.push_back({ b.time, 1.0f });
    if (getenv("FM2_DEALER_TRACE"))
        for (const Beat& b : beats)
            Log("dealer: beat %.3f strength %.2f%s", b.time, b.strength / top, b.strength > 0.3f * top ? "" : " (skipped)");
    Log("dealer: %.1f BPM, %zu beats", 60.0 * kHopHz / lag, kept.size());
    return kept;
}

void Start()
{
    const fs::path song = FindSong();
    if (song.empty())
    {
        Log("dealer: code entered, but there's no song: put your copy of \"Why's This Dealer?\" in %s",
            (UserDir() / "easter").string().c_str());
        return;
    }
    std::vector<float> pcm;
    uint32_t rate = 0;
    if (!Decode(song, pcm, rate))
    {
        Log("dealer: couldn't decode %s", song.string().c_str());
        return;
    }
    g_beats = Analyse(pcm, rate);
    g_rate = rate;
    g_frames = uint32_t(pcm.size() / 2);
    if (const char* o = getenv("FM2_DEALER_OFFSET_MS"))
        g_offset = atof(o) / 1000.0;
    // A voice of its own (an XAudio2 instance besides the game's), the song looping forever.
    static IXAudio2* xaudio = nullptr;
    static IXAudio2MasteringVoice* master = nullptr;
    static std::vector<float> keep;
    keep = std::move(pcm);
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = rate * 8;
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master)) ||
        FAILED(xaudio->CreateSourceVoice(&g_voice, &f)))
    {
        Log("dealer: no audio output");
        return;
    }
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(keep.size() * sizeof(float));
    b.pAudioData = reinterpret_cast<const BYTE*>(keep.data());
    b.LoopCount = XAUDIO2_LOOP_INFINITE;
    g_voice->SubmitSourceBuffer(&b);
    g_voice->Start();
    g_on = true;
    Log("dealer: why's this dealer? (%s, %.1f s)", song.filename().string().c_str(), double(g_frames) / rate);
}
}

// The game window's typed characters (presenter.cpp).
void DealerOnChar(wchar_t c)
{
    static size_t matched = 0;
    static std::atomic<bool> busy{ false };
    const char ch = char(towlower(c));
    matched = ch == kCode[matched] ? matched + 1 : (ch == kCode[0] ? 1 : 0);
    // Once it plays, that's it until the game closes; before that (no song yet) typing it again retries.
    if (matched == sizeof(kCode) - 1)
    {
        matched = 0;
        if (!g_on && !busy.exchange(true))
            std::thread([] {
                Start();
                busy = false;
            }).detach();
    }
}

// The car bodies' scale this frame (x width, y height, z length), 1, 1, 1 when the egg is off. A cycle per
// beat: flattest (low and wide) on the beat, squeezed in (tall and thin) halfway to the next, keeping the
// body's volume.
void DealerSquash(float scale[3])
{
    scale[0] = scale[1] = scale[2] = 1;
    if (!g_on.load(std::memory_order_relaxed) || g_beats.empty())
        return;
    XAUDIO2_VOICE_STATE st{};
    g_voice->GetState(&st, 0);
    const double t = double(st.SamplesPlayed % g_frames) / g_rate + g_offset;
    auto it = std::upper_bound(g_beats.begin(), g_beats.end(), t, [](double v, const Beat& b) { return v < b.time; });
    if (it == g_beats.begin())
        return;
    const auto beat = it - 1;
    const double length = it != g_beats.end() ? it->time - beat->time : 0.45;
    const double phase = std::clamp((t - beat->time) / length, 0.0, 1.0);
    double y = 1.0 - kSquash * std::cos(2.0 * 3.14159265358979 * phase);
    if (const char* hold = getenv("FM2_DEALER_HOLD"); hold && *hold) // testing: one height, held (0.7 flattest)
        y = atof(hold);
    scale[1] = float(y);
    scale[0] = scale[2] = float(1.0 / std::sqrt(y)); // flatter means wider and longer, taller means thinner
}

// The game's music is silent from the moment the song starts until the game closes (xma.cpp).
bool DealerMutesMusic() { return g_on.load(std::memory_order_relaxed); }

// One car-mesh draw (renderer.cpp): the squash its vertex shader adds to its clip position. A car-body mesh's
// clip position is p.x c1 + p.y c0 + p.z c2 + c3 (an ordinary matrix: c3 is the model origin on screen, c0-c2
// the model axes; c0 is up, c1 across, c2 along). Draws of the main body shader (the first body shader seen)
// are cars (origin o, axes A); the cars seen lately are kept, and every other mesh (tyres, brakes, number
// plates, the driver, each drawn with its own transform; other body shaders too, unless no car is near: a
// distant car's simpler body) follows the one it's nearest, so all of a car's parts squash together whatever
// the draw order: the game draws a car's tyres before its body, and its glass in a later pass after every
// other car's. A
// clip point p is l = A^-1 (p - o) in the car (from the x, y and w rows; z only carries depth), and moves by
// A (S - 1) l. That is linear in p: K (p.xyw - o.xyw), K = A (S - 1) A^-1, passed as `squash` (hlsl.cpp
// vsquash: rows 0-3 xyz K, rows 0-2 w o.xyw, row 3 w 1 = on).
// `body`: a car-body shader; `originRow`: 3 when c3 is the mesh's origin on screen, 31 for the driver (skinned:
// c8-c11 place it in the world, c28-c31 on screen, so its origin is c11.x c28 + c11.y c29 + c11.z c30 + c31).
void DealerCarDraw(uint64_t vs, const float c[256][4], float squash[4][4], bool body, int originRow)
{
    float s[3];
    DealerSquash(s);
    if (s[0] == 1 && s[1] == 1 && s[2] == 1)
        return;
    struct Car
    {
        float origin[4], axes[3][4];
        double inv[3][3];
        uint64_t seen;
    };
    static std::vector<Car> cars; // the cars drawn lately, at most kCars
    static uint64_t draws = 0, mainBody = 0;
    constexpr size_t kCars = 32;
    const int rows[3] = { 0, 1, 3 };
    // Where a point on screen sits in a car (model units).
    auto local = [&](const Car& car, const float p[4], double l[3]) {
        for (int j = 0; j < 3; j++)
        {
            l[j] = 0;
            for (int r = 0; r < 3; r++)
                l[j] += car.inv[j][r] * (p[rows[r]] - car.origin[rows[r]]);
        }
    };
    auto extent = [](const double l[3]) { return std::max({ std::abs(l[0]), std::abs(l[1]), std::abs(l[2]) }); };
    draws++;
    if (body && !mainBody)
        mainBody = vs;
    // The car this draw's origin is nearest to, if it's within one (4 model units of its origin).
    const Car* car = nullptr;
    float origin[4];
    for (int i = 0; i < 4; i++)
        origin[i] = originRow == 31 ? c[11][0] * c[28][i] + c[11][1] * c[29][i] + c[11][2] * c[30][i] + c[31][i] : c[3][i];
    if (!body || vs != mainBody)
    {
        double best = 1e30;
        for (const Car& k : cars)
        {
            double l[3];
            local(k, origin, l);
            if (extent(l) < best)
                best = extent(l), car = &k;
        }
        if (getenv("FM2_DEALER_TRACE"))
            Log("dealer: part %016llx %.2f from a car", (unsigned long long)vs, best);
        if (best > 4)
            car = nullptr;
        if (!car && !body)
            return;
    }
    if (body && !car)
    {
        Car b{};
        memcpy(b.origin, c[3], sizeof(b.origin));
        for (int j = 0; j < 3; j++)
            memcpy(b.axes[j], c[j], sizeof(b.axes[j]));
        double m[3][3];
        for (int r = 0; r < 3; r++)
            for (int j = 0; j < 3; j++)
                m[r][j] = b.axes[j][rows[r]];
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
            m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (std::abs(det) <= 1e-12)
            return;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
            {
                // inverse = adjugate / det: inv[j][i] is the cofactor of m[i][j]
                const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
                b.inv[j][i] = (m[i1][j1] * m[i2][j2] - m[i1][j2] * m[i2][j1]) / det;
            }
        b.seen = draws;
        // The same car as a kept one if that one's origin is within 1.2 model units of this one (as far as it
        // moves in a frame; cars side by side are about 2 apart); else a new one, in place of the one unseen longest.
        Car* same = nullptr;
        double nearest = 1.2;
        for (Car& k : cars)
        {
            double l[3];
            local(b, k.origin, l);
            if (extent(l) < nearest)
                nearest = extent(l), same = &k;
        }
        if (!same && cars.size() < kCars)
            same = &cars.emplace_back();
        else if (!same)
            same = &*std::min_element(cars.begin(), cars.end(), [](const Car& x, const Car& y) { return x.seen < y.seen; });
        *same = b;
        car = same;
    }
    // S per axis: c0 up (height), c1 across (width), c2 along (length).
    const double k[3] = { s[1] - 1.0, s[0] - 1.0, s[2] - 1.0 };
    for (int r = 0; r < 4; r++)
    {
        for (int q = 0; q < 3; q++)
        {
            double v = 0;
            for (int j = 0; j < 3; j++)
                v += car->axes[j][r] * k[j] * car->inv[j][q];
            squash[r][q] = float(v);
        }
    }
    squash[0][3] = car->origin[0], squash[1][3] = car->origin[1], squash[2][3] = car->origin[3];
    squash[3][3] = 1;
}
