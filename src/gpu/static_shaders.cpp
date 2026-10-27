// Finds every shader in the game data before the game asks for one.
//
// The title keeps shader microcode raw inside its .rmb.bin mesh files (deflated in the
// track, car and UI zips), 16-byte aligned and with no packet header, plus a few hundred
// in the executable (dword aligned). The title patches vertex fetch layouts into its
// copies at load; the translator reads those from constants, so the copies share HLSL. A candidate is a shader when its control flow decodes cleanly:
// valid opcodes up to an end flag, the first exec address equal to the control-flow
// length, and exec ranges that tile the instruction slots without gaps. Only the used
// slots are kept (the loaded copy has a per-load stamp after them; see
// ucode::UsedDwords). The result is saved so later boots skip the scan.
#include "gpu/static_shaders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <windows.h>

#include "kernel/runtime.h"
#include "loader.h"
#include "zip.h"
#include "log.h"
#include "memory.h"

namespace
{
// ---- Finder ----------------------------------------------------------------------

bool IsExecOp(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }
bool IsEndOp(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }

uint32_t Be(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Used length in dwords of the shader at words[start] (0 if it isn't one); sets pixel.
uint32_t ShaderAt(const uint8_t* bytes, size_t words, size_t start, bool& pixel)
{
    struct Cf { uint32_t op, lo, hi; };
    Cf cfs[130];
    uint32_t ncf = 0, triples = 0;
    for (bool ended = false; !ended;)
    {
        if (triples > 64 || start + triples * 3 + 2 >= words)
            return 0;
        const uint8_t* p = bytes + (start + triples * 3) * 4;
        const uint32_t w0 = Be(p), w1 = Be(p + 4), w2 = Be(p + 8);
        const uint32_t pair[2][2] = { { w0, w1 & 0xFFFF }, { (w1 >> 16) | (w2 << 16), w2 >> 16 } };
        triples++;
        for (const auto& c : pair)
        {
            const uint32_t op = (c[1] >> 12) & 0xF;
            cfs[ncf++] = { op, c[0], c[1] };
            if (IsEndOp(op))
            {
                ended = true;
                break;
            }
        }
    }
    struct Exec { uint32_t addr, count, seq; };
    Exec execs[130];
    uint32_t nexec = 0;
    for (uint32_t i = 0; i < ncf; i++)
    {
        const Cf& c = cfs[i];
        if (c.op == 12 && ((c.lo & 0xFFFFFFF8) || (c.hi & 0x09FF)))
            return 0; // alloc with stray bits
        if (IsExecOp(c.op) && ((c.lo >> 12) & 7))
            execs[nexec++] = { c.lo & 0xFFF, (c.lo >> 12) & 7, (c.lo >> 16) & 0xFFF };
    }
    if (!nexec)
        return 0;
    std::sort(execs, execs + nexec, [](const Exec& a, const Exec& b) { return a.addr < b.addr; });
    uint32_t slot = triples;
    for (uint32_t i = 0; i < nexec; i++)
    {
        if (execs[i].addr != slot)
            return 0;
        slot += execs[i].count;
    }
    if (slot > 512 || start + slot * 3 > words)
        return 0;
    bool vertex = false;
    for (uint32_t i = 0; i < nexec && !vertex; i++)
        for (uint32_t k = 0; k < execs[i].count; k++)
            if ((execs[i].seq >> (2 * k)) & 1)
                if ((Be(bytes + (start + (execs[i].addr + k) * 3) * 4) & 0x1F) == 0)
                    vertex = true;
    bool position = false;
    for (uint32_t i = 0; i < ncf; i++)
        if (cfs[i].op == 12 && ((cfs[i].hi >> 9) & 3) == 1)
            position = true;
    pixel = !vertex && !position;
    return slot * 3;
}

uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

struct Found
{
    std::mutex mutex;
    std::unordered_set<uint64_t> seen;
    std::vector<ScannedShader> shaders;
};

// Data files: candidates every 16 bytes. Executable: every dword, and also shaders that start
// with a branch (in the data files that rule only adds false positives).
void ScanBytes(const uint8_t* bytes, size_t size, Found& found, bool executable = false)
{
    const size_t words = size / 4;
    const size_t stepWords = executable ? 1 : 4;
    for (size_t i = 0; i + 4 <= words; i += stepWords)
    {
        const uint32_t w0 = Be(bytes + i * 4), w1 = Be(bytes + i * 4 + 4);
        const uint32_t op = (w1 >> 12) & 0xF, addr = w0 & 0xFFF;
        // First instruction: an exec at the end of the control flow or an alloc; in the
        // executable also a loop, call or jump (its shaders with branches start with one).
        if (!((IsExecOp(op) && addr >= 1 && addr <= 64) || (op == 12 && !(w0 & 0xFFFFFFF8)) || (executable && (op == 7 || op == 9 || op == 11))))
            continue;
        bool pixel;
        const uint32_t used = ShaderAt(bytes, words, i, pixel);
        if (!used)
            continue;
        const uint8_t* code = bytes + i * 4;
        const uint64_t hash = Fnv(code, used * 4) ^ (pixel ? 1 : 0);
        std::lock_guard lock(found.mutex);
        if (!found.seen.insert(hash).second)
            continue;
        ScannedShader s{ pixel, std::vector<uint32_t>(used) };
        memcpy(s.code.data(), code, used * 4);
        found.shaders.push_back(std::move(s));
    }
}

constexpr uint32_t kFileMagic = 0x53534D46; // 'FMSS'
constexpr uint32_t kFileVersion = 5; // 2: executable at dword alignment; 4: its shaders that start with a branch; 5: data files signature

// The files to search: the game's own, then the downloadable content and the title update
// (their tracks and cars bring their own shaders).
std::vector<std::filesystem::path> SourceFiles()
{
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const std::filesystem::path& root : { GameDir(), DlcDir(), TitleUpdateDir() })
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(tolower(c)); });
            if (ext == ".zip" || ext == ".bin")
                files.push_back(it->path());
        }
    std::sort(files.begin(), files.end());
    return files;
}

// Which files (names and sizes) and which executable the saved list was made from: adding
// DLC or the title update makes the next boot search again.
uint32_t SourcesSignature(const std::vector<std::filesystem::path>& files)
{
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](const void* p, size_t n) { h = (h ^ Fnv(static_cast<const uint8_t*>(p), n)) * 0x100000001B3ull; };
#ifdef FM2_TITLE_UPDATE
    mix("tu", 2);
#endif
    for (const auto& f : files)
    {
        std::error_code ec;
        const std::string name = f.filename().string();
        const uint64_t size = std::filesystem::file_size(f, ec);
        mix(name.data(), name.size());
        mix(&size, sizeof(size));
    }
    return uint32_t(h ^ (h >> 32));
}

std::filesystem::path SavedPath() { return UserDir() / "static_shaders.bin"; }

bool LoadSaved(std::vector<ScannedShader>& out, uint32_t signature)
{
    std::ifstream in(SavedPath(), std::ios::binary);
    uint32_t head[4];
    if (!in.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] != kFileMagic || head[1] != kFileVersion || head[3] != signature)
        return false;
    out.resize(head[2]);
    for (auto& s : out)
    {
        uint32_t meta[2];
        if (!in.read(reinterpret_cast<char*>(meta), sizeof(meta)) || meta[1] > 0x2000)
            return false;
        s.pixel = meta[0] != 0;
        s.code.resize(meta[1]);
        if (!in.read(reinterpret_cast<char*>(s.code.data()), std::streamsize(meta[1] * 4)))
            return false;
    }
    return true;
}

void Save(const std::vector<ScannedShader>& shaders, uint32_t signature)
{
    std::ofstream out(SavedPath(), std::ios::binary);
    const uint32_t head[4] = { kFileMagic, kFileVersion, uint32_t(shaders.size()), signature };
    out.write(reinterpret_cast<const char*>(head), sizeof(head));
    for (const auto& s : shaders)
    {
        const uint32_t meta[2] = { s.pixel ? 1u : 0u, uint32_t(s.code.size()) };
        out.write(reinterpret_cast<const char*>(meta), sizeof(meta));
        out.write(reinterpret_cast<const char*>(s.code.data()), std::streamsize(s.code.size() * 4));
    }
}
}

bool StaticShadersSaved()
{
    std::vector<ScannedShader> unused;
    std::ifstream in(SavedPath(), std::ios::binary);
    uint32_t head[4];
    return in.read(reinterpret_cast<char*>(head), sizeof(head)) && head[0] == kFileMagic && head[1] == kFileVersion &&
        head[3] == SourcesSignature(SourceFiles());
}

std::vector<ScannedShader> StaticShaders()
{
    std::vector<ScannedShader> result;
    const std::vector<std::filesystem::path> files = SourceFiles();
    const uint32_t signature = SourcesSignature(files);
    if (LoadSaved(result, signature))
    {
        Log("static shaders: %zu loaded from %s", result.size(), SavedPath().string().c_str());
        return result;
    }
    const auto start = std::chrono::steady_clock::now();
    Found found;
    std::atomic<size_t> next{ 0 };
    std::vector<std::thread> workers;
    const unsigned n = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < n; t++)
        workers.emplace_back([&] {
            // Below the game's threads, its audio among them: on the first start this takes every core for
            // 20 s or more, and at normal priority the sound crackled the whole time (report #20).
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (size_t i; (i = next++) < files.size();)
            {
                std::ifstream in(files[i], std::ios::binary | std::ios::ate);
                if (!in)
                    continue;
                std::vector<uint8_t> data(size_t(in.tellg()));
                in.seekg(0);
                in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
                if (files[i].extension() == ".zip" || files[i].extension() == ".ZIP")
                    ForEachZipMember(data, ".rmb", [&](const std::string&, const std::vector<uint8_t>& member) { ScanBytes(member.data(), member.size(), found); });
                else
                    ScanBytes(data.data(), data.size(), found);
            }
        });
    for (auto& w : workers)
        w.join();
    const size_t fromFiles = found.shaders.size();
    // Shaders built into the executable (compressed on disc, so the loaded image): most of
    // the UI, title and effect shaders, only dword aligned.
    ScanBytes(GuestPtr<uint8_t>(0x82000000), 0x2000000, found, true);
    result = std::move(found.shaders);
    Save(result, signature);
    Log("static shaders: %zu found (%zu in %zu data files, %zu in the executable) in %.1f s", result.size(), fromFiles, files.size(),
        result.size() - fromFiles, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return result;
}
