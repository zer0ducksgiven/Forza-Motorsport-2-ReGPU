// Runs the Vulkan backend's shader service over the game's shader corpus: every shader the
// renderer's scan found in the disc, translated by hlsl.cpp and compiled by dxc, cached on
// disk. The number that matters for a first start.
//
//   ./out/vk_shaders_test <dir of vs_*.bin / ps_*.bin dumps> [limit]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "gpu/ucode.h"
#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_shaders.h"

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: vk_shaders_test <shader dump dir> [limit]\n");
        return 2;
    }
    const size_t limit = argc > 2 ? size_t(atoi(argv[2])) : SIZE_MAX;
    std::vector<std::string> files;
    if (DIR* dir = opendir(argv[1]))
    {
        while (dirent* entry = readdir(dir))
        {
            const std::string name = entry->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0 &&
                (name.rfind("vs_", 0) == 0 || name.rfind("ps_", 0) == 0))
                files.push_back(std::string(argv[1]) + "/" + name);
        }
        closedir(dir);
    }
    std::sort(files.begin(), files.end());
    if (files.size() > limit)
        files.resize(limit);

    vk::Context context;
    if (!vk::Init(context, "out/vkcache-test"))
        return 1;
    printf("vk: %s, %zu shaders\n", context.props.deviceName, files.size());

    vk::ShaderSet set;
    set.cacheDir = "out/vkcache";
    auto start = std::chrono::steady_clock::now();
    size_t ok = 0, failed = 0, translateFailed = 0, skipped = 0;
    for (const std::string& path : files)
    {
        std::vector<char> bytes;
        if (FILE* f = fopen(path.c_str(), "rb"))
        {
            fseek(f, 0, SEEK_END);
            bytes.resize(size_t(ftell(f)));
            fseek(f, 0, SEEK_SET);
            if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                bytes.clear();
            fclose(f);
        }
        if (bytes.empty() || (bytes.size() & 3))
            continue;
        const bool pixel = path.find("/ps_") != std::string::npos;
        std::vector<uint32_t> microcode(bytes.size() / 4);
        memcpy(microcode.data(), bytes.data(), bytes.size());
        // The renderer's key: Fnv (1B3) over the used dwords, xor the type, as GetShader does.
        const uint32_t used = ucode::UsedDwords(microcode.data(), uint32_t(microcode.size()));
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < used; i++)
            hash = (hash ^ microcode[i]) * 0x100000001B3ull;
        hash ^= pixel ? 1 : 0;
        const std::string filename = path.substr(path.rfind('/') + 1);
        if (filename.size() >= 23)
            hash = std::stoull(filename.substr(3, 16), nullptr, 16);
        // The renderer filters the scanner's false positives before translating (ucode::Plausible).
        if (!ucode::Plausible(ucode::Parse(microcode.data(), used, pixel)))
        {
            skipped++;
            continue;
        }
        const vk::VkShader* shader = vk::ShaderGet(context, set, hash, microcode.data(), used, pixel);
        if (shader->ok)
            ok++;
        else
        {
            failed++;
            fprintf(stderr, "%s: %s\n", path.c_str(), shader->error.c_str());
            translateFailed += shader->error.find("vkCreateShaderModule") == std::string::npos &&
                shader->error.find("dxc") == std::string::npos;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    printf("vk: %zu ok, %zu failed (%zu translation, %zu dxc), %zu not shaders, %.1f s total, %.1f ms each\n", ok, failed,
        translateFailed, failed - translateFailed, skipped, ms / 1000.0, ok ? ms / ok : 0.0);

    // A second pass is all cache: this is a start where nothing changed.
    auto warm = std::chrono::steady_clock::now();
    size_t warmOk = 0;
    for (const std::string& path : files)
    {
        std::vector<char> bytes;
        if (FILE* f = fopen(path.c_str(), "rb"))
        {
            fseek(f, 0, SEEK_END);
            bytes.resize(size_t(ftell(f)));
            fseek(f, 0, SEEK_SET);
            if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                bytes.clear();
            fclose(f);
        }
        if (bytes.empty())
            continue;
        const bool pixel = path.find("/ps_") != std::string::npos;
        std::vector<uint32_t> microcode(bytes.size() / 4);
        memcpy(microcode.data(), bytes.data(), bytes.size());
        const uint32_t used = ucode::UsedDwords(microcode.data(), uint32_t(microcode.size()));
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < used; i++)
            hash = (hash ^ microcode[i]) * 0x100000001B3ull;
        hash ^= pixel ? 1 : 0;
        const std::string filename = path.substr(path.rfind('/') + 1);
        if (filename.size() >= 23)
            hash = std::stoull(filename.substr(3, 16), nullptr, 16);
        const vk::VkShader* shader = vk::ShaderGet(context, set, hash, microcode.data(), used, pixel);
        warmOk += shader->ok;
    }
    printf("vk: second pass %zu shaders from memory in %.0f ms\n", warmOk,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warm).count());

    vk::ShaderSetDestroy(context, set);
    vk::Shutdown(context);
    return failed && !getenv("FM2_ALLOW_FAILURES") ? 2 : 0;
}
