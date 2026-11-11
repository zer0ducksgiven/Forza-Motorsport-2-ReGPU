#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_shaders.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "log.h"

#include "gpu/shader_scan.h"
#include "gpu/static_shaders.h"
#include "gpu/hlsl.h"
#include "gpu/ucode.h"
#include "gpu/vk/vk_shader.h"

namespace vk
{
void ShaderSetDestroy(Context& context, ShaderSet& set)
{
    for (auto& [hash, shader] : set.shaders)
        if (shader.module)
            vkDestroyShaderModule(context.device, shader.module, nullptr);
    set.shaders.clear();
}

namespace
{
// ---- Warm-up ------------------------------------------------------------------------------------------------------
// The first use of a shader costs a DXC compile (50-180 ms) in the middle of a frame. The warm-up threads run the same
// translation and compile ahead of the game for every shader in the game data (StaticShaders) and every shader object
// found in guest memory (the shader scan), writing only the disk cache. A draw that needs a shader still gets it
// synchronously - from the cache if a warm-up thread was there first, by compiling it otherwise - so no draw is ever
// skipped or drawn with a stand-in.
struct Warm
{
    std::mutex mutex;
    std::condition_variable work, finished;
    std::deque<std::pair<ScannedShader, bool>> queue; // second: from the static set (counted for the first-boot wait)
    std::unordered_set<uint64_t> seen;     // queued or done
    std::unordered_set<uint64_t> knownByWarm; // queued by the warm-up itself (seen also holds what the renderer took)
    std::unordered_set<uint64_t> inFlight; // being compiled by a warm-up thread now
    std::atomic<uint32_t> compiled{ 0 }, cached{ 0 }, rejected{ 0 };
    std::atomic<bool> started{ false };
    // The static set, for the first-boot screen: how many were queued, how many of those are finished, and whether
    // the list itself has been read yet (the first ever search of the game files takes a while).
    std::atomic<uint32_t> staticTotal{ 0 }, staticDone{ 0 };
    std::atomic<bool> staticListed{ false };
    std::filesystem::path cacheDir;
} g_warm;

// Identifies a shader by its microcode, the same on both sides (the renderer's hash and the scanner's are taken over
// different lengths of the same object).
uint64_t WarmKey(const uint32_t* code, uint32_t dwords, bool pixel)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < dwords; i++)
        hash = (hash ^ code[i]) * 0x100000001B3ull;
    return hash ^ (pixel ? 0x9E3779B97F4A7C15ull : 0);
}

const VkShader* ShaderBuild(Context* context, ShaderSet* set, const std::filesystem::path& cacheDir, uint64_t hash,
    const uint32_t* microcodeBE, uint32_t dwords, bool pixel);

size_t WarmEnqueue(std::vector<ScannedShader>& found, bool front)
{
    size_t added = 0;
    for (ScannedShader& s : found)
    {
        // The scanner accepts a candidate on control flow alone; some of what it finds is not a shader.
        if (s.code.empty() || !ucode::Plausible(ucode::Parse(s.code.data(), uint32_t(s.code.size()), s.pixel)))
        {
            g_warm.rejected++;
            continue;
        }
        const uint64_t key = WarmKey(s.code.data(), uint32_t(s.code.size()), s.pixel);
        std::lock_guard lock(g_warm.mutex);
        g_warm.knownByWarm.insert(key);
        if (!g_warm.seen.insert(key).second)
            continue;
        // Shader objects in guest memory are the ones the game is about to draw with: ahead of the static set.
        if (front)
            g_warm.queue.emplace_front(std::move(s), false);
        else
        {
            g_warm.queue.emplace_back(std::move(s), true);
            g_warm.staticTotal++;
        }
        added++;
    }
    if (added)
        g_warm.work.notify_all();
    return added;
}
}

const VkShader* ShaderGet(Context& context, ShaderSet& set, uint64_t hash, const uint32_t* microcodeBE, uint32_t dwords,
    bool pixel)
{
    if (g_warm.started.load(std::memory_order_relaxed) && set.shaders.find(hash) == set.shaders.end())
    {
        // If a warm-up thread is compiling this very shader, its result is milliseconds away: wait for it rather than
        // start the same compile again.
        const uint64_t key = WarmKey(microcodeBE, dwords, pixel);
        std::unique_lock lock(g_warm.mutex);
        // Bounded: the warm-up threads run below normal priority, and a busy machine must not turn that into a stall
        // longer than the compile itself. Past the bound the renderer compiles it as well (the cache write is atomic).
        g_warm.finished.wait_for(lock, std::chrono::milliseconds(300), [&] { return !g_warm.inFlight.count(key); });
        g_warm.seen.insert(key); // the renderer has it now: nothing left to warm
    }
    return ShaderBuild(&context, &set, set.cacheDir, hash, microcodeBE, dwords, pixel);
}

void ShaderWarmStart(const ShaderSet& set)
{
#if !defined(_WIN32)
    (void)set; // the native test tools compile through the dxc executable and have no game data to scan
#else
    // Only with the in-process compiler: the dxc.exe fallback shares one temporary file name.
    if (getenv("FM2_VK_NO_SHADER_WARM") || getenv("FM2_VK_DXC_PROCESS") || set.cacheDir.empty() ||
        (!GetModuleHandleW(L"dxcompiler.dll") && !LoadLibraryW(L"dxcompiler.dll")) || g_warm.started.exchange(true))
        return;
    g_warm.cacheDir = set.cacheDir;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = getenv("FM2_VK_SHADER_WARM_THREADS")
        ? std::clamp<unsigned>(unsigned(atoi(getenv("FM2_VK_SHADER_WARM_THREADS"))), 1, 32)
        : std::clamp<unsigned>(hardware / 2, 1, 16); // below normal priority: the game's own threads come first
    auto lowPriority = [] { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); };
    for (unsigned i = 0; i < threads; i++)
        std::thread([lowPriority] {
            lowPriority();
            for (;;)
            {
                ScannedShader shader;
                bool fromStatic;
                uint64_t key;
                {
                    std::unique_lock lock(g_warm.mutex);
                    g_warm.work.wait(lock, [] { return !g_warm.queue.empty(); });
                    shader = std::move(g_warm.queue.front().first);
                    fromStatic = g_warm.queue.front().second;
                    g_warm.queue.pop_front();
                    key = WarmKey(shader.code.data(), uint32_t(shader.code.size()), shader.pixel);
                    g_warm.inFlight.insert(key);
                }
                ShaderBuild(nullptr, nullptr, g_warm.cacheDir, key, shader.code.data(), uint32_t(shader.code.size()), shader.pixel);
                {
                    std::lock_guard lock(g_warm.mutex);
                    g_warm.inFlight.erase(key);
                }
                if (fromStatic)
                    g_warm.staticDone++;
                g_warm.finished.notify_all();
            }
        }).detach();
    std::thread([lowPriority, threads] {
        lowPriority();
        std::vector<ScannedShader> found;
        if (!getenv("FM2_NO_STATIC_SHADERS"))
        {
            found = StaticShaders();
            const size_t total = found.size();
            const size_t added = WarmEnqueue(found, false);
            Log("shader_vk: warm-up: %zu of %zu static shaders queued on %u threads", added, total, threads);
        }
        g_warm.staticListed = true;
        const bool scan = getenv("FM2_NO_SHADER_SCAN") == nullptr;
        uint32_t reported = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            if (scan)
            {
                ShaderScanNow(1);
                found.clear();
                while (ShaderScanTake(found, 64))
                {
                    WarmEnqueue(found, true);
                    found.clear();
                }
            }
            const uint32_t compiled = g_warm.compiled.load();
            size_t queued;
            {
                std::lock_guard lock(g_warm.mutex);
                queued = g_warm.queue.size();
            }
            if (compiled != reported && (queued == 0 || compiled - reported >= 250))
            {
                reported = compiled;
                Log("shader_vk: warm-up: %u compiled, %u already cached, %zu queued", compiled, g_warm.cached.load(), queued);
            }
        }
    }).detach();
#endif
}

bool ShaderWarmProgress(uint32_t& done, uint32_t& total, uint32_t& compiled)
{
    done = g_warm.staticDone.load();
    total = g_warm.staticTotal.load();
    compiled = g_warm.compiled.load();
    return g_warm.started.load() && (!g_warm.staticListed.load() || done < total);
}

void ShaderWarmStats(uint32_t& compiled, uint32_t& cached, uint32_t& pending)
{
    compiled = g_warm.compiled.load();
    cached = g_warm.cached.load();
    std::lock_guard lock(g_warm.mutex);
    pending = uint32_t(g_warm.queue.size() + g_warm.inFlight.size());
}

namespace
{
// context and set are null on a warm-up thread: translate, compile and write the disk cache, nothing else.
const VkShader* ShaderBuild(Context* contextPtr, ShaderSet* setPtr, const std::filesystem::path& cacheDir, uint64_t hash,
    const uint32_t* microcodeBE, uint32_t dwords, bool pixel)
{
    const bool warm = setPtr == nullptr;
    ShaderSet scratch;
    ShaderSet& set = warm ? scratch : *setPtr;
    // A debugging switch that edits shaders is set: the warm-up leaves the cache to the renderer.
    static const bool debugSwitches = getenv("FM2_VK_DEBUG_PS_ALL") || getenv("FM2_VK_DEBUG_PS") || getenv("FM2_VK_DEBUG_VS") ||
        getenv("FM2_VK_SHADER_OD");
    if (auto it = set.shaders.find(hash); it != set.shaders.end())
        return &it->second;
    if (warm && debugSwitches)
        return nullptr;

    VkShader shader;
    shader.pixel = pixel;
    const ucode::Shader parsed = ucode::Parse(microcodeBE, dwords, pixel);
    const TranslatedShader translated = TranslateShader(parsed);
    if (!translated.ok)
    {
        if (warm)
            return nullptr;
        shader.error = translated.error;
        set.failed++;
        auto& entry = set.shaders[hash];
        entry = std::move(shader);
        return &entry;
    }
    shader.hlsl = translated.hlsl;
    // FM2_DUMP_HLSL, as on the D3D12 path: keep each translated shader for reading (the Vulkan
    // backend translates in its own service, so the D3D12 side's dump never fired here).
    if (!warm && getenv("FM2_DUMP_HLSL"))
    {
        std::filesystem::create_directories("out/hlsl");
        char name[64];
        snprintf(name, sizeof(name), "out/hlsl/%s_%016llx.hlsl", pixel ? "ps" : "vs", (unsigned long long)hash);
        std::ofstream(name, std::ios::binary).write(shader.hlsl.data(), std::streamsize(shader.hlsl.size()));
    }
    shader.constantsUsed = translated.constantsUsed;
    shader.usedTextureSlots = translated.usedTextureSlots;
    for (const auto& entry : translated.vertexFetchLayout)
        shader.vertexFetchLayout.insert(shader.vertexFetchLayout.end(), entry.begin(), entry.end());
    shader.vertexFetchConstants = translated.vertexFetchConstants;
    if (!warm)
    {
        std::string list;
        for (uint32_t k : shader.vertexFetchConstants)
        {
            char t[8];
            snprintf(t, sizeof(t), " %u", k);
            list += t;
        }
        Log("shader_vk: %s %016llx fetches[%s ], layout %zu, constants %u", pixel ? "ps" : "vs", (unsigned long long)hash, list.c_str(),
            shader.vertexFetchLayout.size(), shader.constantsUsed);
        // A fetch slot's layout is what the shader reads the vertex buffer with: x is (constant
        // index | format << 8 | rounding flags), y stride, z offset, w swizzle. A zero stride or a
        // constant index outside the fetch list is what a black vertex colour looks like.
        for (size_t k = 0; k < shader.vertexFetchLayout.size() / 4 && k < 4; k++)
        {
            const uint32_t* w = &shader.vertexFetchLayout[k * 4];
            Log("shader_vk:   vfl[%zu] = const %2u format %2u stride %3u offset %5u swizzle %03x", k, w[0] & 0x7Fu, (w[0] >> 8) & 0x3Fu,
                w[1], w[2], w[3] & 0xFFF);
        }
    }

    std::vector<uint32_t> spirv;
    std::string error;
    // FM2_VK_DEBUG_PS=<hash>:<expr> / FM2_VK_DEBUG_PS_ALL=<expr>: replace a pixel shader's
    // output, the Vulkan twin of the D3D12 path's FM2_DEBUG_PS (renderer.cpp). The expression
    // sees the shader's own locals — `input.o[6]`, `oC[0]`, `TFetch(...)` — so two backends can
    // be asked the same question about the same draw and the answers compared.
    const bool debugAll = pixel && getenv("FM2_VK_DEBUG_PS_ALL");
    const char* debugOne = pixel ? getenv("FM2_VK_DEBUG_PS") : nullptr;
    const bool debugThis = debugOne && strtoull(debugOne, nullptr, 16) == hash;
    if (debugAll || debugThis)
    {
        const char* expr = debugAll ? getenv("FM2_VK_DEBUG_PS_ALL") : strchr(debugOne, ':');
        if (expr && expr[0] == ':')
            expr++;
        // "<hash>:@<block>:<statement>" inserts at the start of control-flow block <block>, the
        // Vulkan twin of the D3D12 path's form: how a value is bisected inside a state machine.
        // Several of them can be given, separated by '|', which is how the path a state machine
        // actually takes gets recorded (one marker per block, read back at the end).
        if (expr && expr[0] == '@')
        {
            const std::string all(expr);
            for (size_t pos = 0; pos < all.size();)
            {
                const size_t bar = all.find('|', pos);
                const size_t start = pos + (all[pos] == '@');
                const std::string spec = all.substr(start, (bar == std::string::npos ? all.size() : bar) - start);
                pos = bar == std::string::npos ? all.size() : bar + 1;
                if (spec.rfind("return=", 0) == 0)
                {
                    if (const size_t at = shader.hlsl.rfind("    return oC[0];"); at != std::string::npos)
                        shader.hlsl.replace(at, strlen("    return oC[0];"), "    return " + spec.substr(7) + ";");
                    continue;
                }
                const size_t colon = spec.find(':');
                if (colon == std::string::npos)
                    continue;
                if (spec.rfind("main:", 0) == 0)
                {
                    const std::string marker = "    float4 oC[4];";
                    if (const size_t at = shader.hlsl.find(marker); at != std::string::npos)
                        shader.hlsl.insert(at + marker.size(), "            " + spec.substr(colon + 1) + "\n");
                    continue;
                }
                const std::string marker = "        if (cfpc == " + spec.substr(0, colon) + "u)\n        {\n";
                if (const size_t at = shader.hlsl.find(marker); at != std::string::npos)
                    shader.hlsl.insert(at + marker.size(), "            " + spec.substr(colon + 1) + "\n");
            }
        }
        else if (expr)
            if (const size_t at = shader.hlsl.rfind("    return oC[0];"); at != std::string::npos)
                shader.hlsl.replace(at, strlen("    return oC[0];"), std::string("    return ") + expr + ";");
    }
    // FM2_VK_DEBUG_VS=<hash>:<statement> is the vertex-stage twin of D3D12's FM2_DEBUG_VS.
    // Append to the guest program, use @<block>:<statement> for a block, or @TEXT|STATEMENT to
    // insert after a source line. Separate statements with | to capture a value in one block.
    // The D3D12 spelling FM2_DEBUG_VS is accepted too, so one command line asks both backends the same question.
    const char* debugVertex = !pixel ? (getenv("FM2_VK_DEBUG_VS") ? getenv("FM2_VK_DEBUG_VS") : getenv("FM2_DEBUG_VS")) : nullptr;
    const bool debugVertexThis = debugVertex && strtoull(debugVertex, nullptr, 16) == hash;
    if (debugVertexThis)
        if (const char* statement = strchr(debugVertex, ':'))
        {
            const std::string all(statement + 1);
            if (all.size() > 2 && all[0] == '@' && (all[1] < '0' || all[1] > '9'))
            {
                const size_t bar = all.find('|');
                const size_t hit = bar == std::string::npos ? std::string::npos : shader.hlsl.find(all.substr(1, bar - 1));
                const size_t at = hit == std::string::npos ? std::string::npos : shader.hlsl.find('\n', hit);
                if (at != std::string::npos)
                    shader.hlsl.insert(at + 1, "            " + all.substr(bar + 1) + "\n");
            }
            else
                for (size_t pos = 0; pos < all.size();)
                {
                    const size_t bar = all.find('|', pos);
                    std::string s = all.substr(pos, (bar == std::string::npos ? all.size() : bar) - pos);
                    pos = bar == std::string::npos ? all.size() : bar + 1;
                    std::string marker = "}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n";
                    if (s[0] == '@')
                    {
                        const size_t colon = s.find(':');
                        marker = "        if (cfpc == " + s.substr(1, colon - 1) + "u)\n        {\n";
                        s = s.substr(colon + 1);
                    }
                    if (const size_t at = shader.hlsl.find(marker); at != std::string::npos)
                        shader.hlsl.insert(marker[0] == '}' ? at : at + marker.size(), "            " + s + "\n");
                }
        }
    // A patched shader must not go into (or come out of) the disk cache under the guest's hash:
    // every probe would then compile the first patch and silently reuse it.
    const bool patched = debugAll || debugThis || debugVertexThis;
    // FM2_VK_SHADER_OD=1, or a guest shader hash to change only that shader's optimisation.
    const char* noOptimiseEnv = getenv("FM2_VK_SHADER_OD");
    const bool noOptimise = noOptimiseEnv && (atoi(noOptimiseEnv) == 1 || strtoull(noOptimiseEnv, nullptr, 16) == hash);
    // What is compiled: the translator's HLSL is the Direct3D 12 path's, byte for byte. Two things differ for SPIR-V.
    // The register file becomes plain variables (RegistersAsVariables, as that path does before fxc): an indexable
    // local array stays in memory on some drivers. And the bindless arrays get their real size, because an unsized
    // array would need the runtime-array capability for a set that is not variable-sized.
    std::string source = RegistersAsVariables(shader.hlsl);
    for (const auto& [from, to] : { std::pair<const char*, const char*>{ "gtex[] :", "gtex[65536] :" }, { "gcube[] :", "gcube[65536] :" } })
        if (const size_t at = source.find(from); at != std::string::npos)
            source.replace(at, strlen(from), to);
    const uint64_t cacheKey = HashSource(source + (pixel ? "\nps_6_0" : "\nvs_6_0") + (noOptimise ? "\n-Od" : ""));
    if (warm)
    {
        if (patched)
            return nullptr;
        if (SpirvCached(source, cacheKey, cacheDir))
            g_warm.cached++;
        else if (CompileHlsl(source, pixel, cacheKey, cacheDir, spirv, &error, noOptimise))
            g_warm.compiled++;
        return nullptr;
    }
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    Context& context = *contextPtr;
    // A compile on the render thread is a stall the warm-up was meant to prevent: say which shader, and whether the
    // warm-up knew its microcode (known + compiled here = it was translated differently; unknown = not found ahead).
    // What is left on a first boot is about 27 small vertex shaders the title loads from the command stream; they
    // are in neither the game files nor guest memory's shader objects.
    const bool cachedAhead = patched || SpirvCached(source, cacheKey, set.cacheDir);
    if (!cachedAhead && g_warm.started.load())
    {
        bool known;
        {
            std::lock_guard lock(g_warm.mutex);
            known = g_warm.knownByWarm.count(WarmKey(microcodeBE, dwords, pixel)) != 0;
        }
        static std::atomic<uint32_t> logged{ 0 };
        if (logged++ < 64)
            Log("shader_vk: render-thread compile of %s %016llx (%u dwords): microcode %s to the warm-up", pixel ? "ps" : "vs",
                (unsigned long long)hash, dwords, known ? "KNOWN" : "unknown");
    }
    if (!CompileHlsl(source, pixel, cacheKey, patched ? std::filesystem::path() : set.cacheDir, spirv, &error, noOptimise))
    {
        shader.error = error.empty() ? "dxc failed" : error;
        set.failed++;
        auto& entry = set.shaders[hash];
        entry = std::move(shader);
        return &entry;
    }
    info.codeSize = spirv.size() * 4;
    info.pCode = spirv.data();
    if (vkCreateShaderModule(context.device, &info, nullptr, &shader.module) != VK_SUCCESS)
    {
        shader.error = "vkCreateShaderModule failed";
        set.failed++;
        auto& entry = set.shaders[hash];
        entry = std::move(shader);
        return &entry;
    }
    shader.ok = true;
    set.compiled++;
    (cachedAhead ? set.cached : set.fromCompiler)++;
    {
        static uint32_t logged = 0;
        if (logged < 12)
        {
            logged++;
            Log("shader_vk: %s %016llx texture slots %08x, constants %u", pixel ? "ps" : "vs", (unsigned long long)hash,
                shader.usedTextureSlots, shader.constantsUsed);
        }
    }
    auto& entry = set.shaders[hash];
    entry = std::move(shader);
    return &entry;
}
}
}
