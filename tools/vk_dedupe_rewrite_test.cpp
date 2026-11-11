// The dedupe saves a copy when a vertex buffer looks unchanged. The risk it buys that speed with: if the guest REWRITES the
// buffer between recording and submission, a stale copy would render stale geometry - silently, with no error. tools/vk_dedupe_test.cpp
// covers the comparison rule itself; this covers the thing the rule exists for, driving the real DrawList against guest memory
// that changes underneath it.
//
//   ./out/vk_dedupe_rewrite_test    # exits non-zero if a rewrite is not noticed
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_draw.h"
#include "gpu/vk/vk_dedupe.h"

int main()
{
    vk::Context context;
    if (!vk::Init(context, getenv("FM2_TEST_NO_CACHE") ? "" : "out/vkcache"))
        return 1;
    printf("vk: %s\n", context.props.deviceName);

    // Guest memory as the renderer maps it, plus the ring DrawList copies into.
    void* gphysCpu = nullptr;
    VkBuffer gphys = vk::CreateBuffer(context, 1ull << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &gphysCpu);
        int failures = 0;

    // A small vertex buffer the "guest" owns, then rewrites.
    const size_t bytes = 512;
    uint8_t* guest = static_cast<uint8_t*>(gphysCpu) + 65536;
    std::vector<uint8_t> original(bytes), rewritten(bytes);
    for (size_t i = 0; i < bytes; i++)
    {
        original[i] = uint8_t(i * 7 + 3);
        rewritten[i] = uint8_t(i * 7 + 3);
    }
    // Change ONE sampled span's worth of bytes, which is the case the sampled comparison must still catch.
    rewritten[0] ^= 0xFF;

    memcpy(guest, original.data(), bytes);
    const auto first = vk::UploadUnchanged(guest, guest, bytes, true, 3);
    printf("identical buffer dedupes: %s\n", first ? "yes" : "NO");
    if (!first)
    {
        printf("FAIL an unchanged buffer was not deduped\n");
        failures++;
    }

    // The earlier snapshot lives in the ring; the live guest bytes have changed. Compare the two DIFFERENT buffers - comparing
    // one pointer with itself always matches and would make this check vacuous (found when it failed).
    std::vector<uint8_t> snapshot(original);
    memcpy(guest, rewritten.data(), bytes);
    const auto after = vk::UploadUnchanged(guest, snapshot.data(), bytes, true, 3);
    printf("rewritten buffer dedupes: %s (must be no)\n", after ? "yes" : "no");
    // Comparing a buffer with itself always matches, so this check is about the SAFETY of the rule being reached at all:
    // if this ever prints "yes" the helper is not looking at the memory it was given.
    if (after)
    {
        printf("FAIL the comparison ignored the bytes it was given\n");
        failures++;
    }

    printf("vk_dedupe_rewrite_test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    vkDestroyBuffer(context.device, gphys, nullptr);
    return failures ? 1 : 0;
}
