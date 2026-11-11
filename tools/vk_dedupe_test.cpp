// The upload dedupe decides whether a vertex buffer's bytes can be reused from an earlier copy. Two failure modes matter:
// a MISSED rewrite renders stale geometry (silent corruption), and an over-eager miss just re-copies (slow but correct).
// This exercises the comparison rule directly, without a device, so the sampled mode can be judged on evidence.
//
//   ./out/vk_dedupe_test        # exits non-zero if a rewrite goes undetected
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu/vk/vk_dedupe.h"

// The rule itself lives in the header so this test exercises the CODE THE RENDERER RUNS, not a copy of it.
static bool Same(const uint8_t* now, const uint8_t* before, size_t bytes, bool sampled, long spans)
{
    return vk::UploadUnchanged(now, before, bytes, sampled, spans);
}

int main()
{
    int failures = 0;
    const size_t sizes[] = { 64, 128, 129, 256, 948, 4096 };
    for (size_t bytes : sizes)
    {
        std::vector<uint8_t> before(bytes), now;
        for (size_t i = 0; i < bytes; i++)
            before[i] = uint8_t(i * 7 + 3);
        now = before;

        // Unchanged data must dedupe in every mode - this is the path that saves the copy.
        for (bool sampled : { false, true })
            for (long spans : { 1L, 2L, 3L })
                if (!Same(now.data(), before.data(), bytes, sampled, spans))
                {
                    ::printf("FAIL unchanged not deduped  bytes %zu sampled %d spans %ld\n", bytes, int(sampled), spans);
                    failures++;
                }

        // A change in the HEAD span must be caught.
        now = before;
        now[0] ^= 0xFF;
        for (bool sampled : { false, true })
            if (Same(now.data(), before.data(), bytes, sampled, 3))
            {
                ::printf("FAIL head rewrite UNDETECTED bytes %zu sampled %d\n", bytes, int(sampled));
                failures++;
            }

        // A change in the TAIL span must be caught with 2+ spans; one span knowingly cannot see it.
        now = before;
        now[bytes - 1] ^= 0xFF;
        if (Same(now.data(), before.data(), bytes, true, 2) || Same(now.data(), before.data(), bytes, true, 3))
        {
            ::printf("FAIL tail rewrite UNDETECTED bytes %zu\n", bytes);
            failures++;
        }
        if (!Same(now.data(), before.data(), bytes, true, 1) && bytes > 128)
        {
            ::printf("FAIL tail rewrite unexpectedly caught by 1 span, bytes %zu\n", bytes);
            failures++;
        }

        // A change in the MIDDLE span must be caught with 3 spans.
        now = before;
        now[bytes / 2] ^= 0xFF;
        if (bytes > 128 && Same(now.data(), before.data(), bytes, true, 3))
        {
            ::printf("FAIL middle rewrite UNDETECTED bytes %zu\n", bytes);
            failures++;
        }
    }

    // The documented limitation, asserted so it cannot be forgotten: a rewrite confined to an UNsampled byte is missed.
    {
        const size_t bytes = 4096;
        std::vector<uint8_t> before(bytes, 0), now(bytes, 0);
        now[200] = 1; // between the head (0-63) and middle (1984-2047) spans
        if (Same(now.data(), before.data(), bytes, true, 3))
            ::printf("KNOWN: unsampled byte rewrite is missed by the 3-span sample (expected)\n");
        if (Same(now.data(), before.data(), bytes, false, 3))
        {
            ::printf("FAIL exact comparison MISSED a rewrite it must catch\n");
            failures++;
        }
    }

    ::printf("vk_dedupe_test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
