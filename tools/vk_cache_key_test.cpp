// Proves the SPIR-V cache key responds to the HLSL text, not just to the guest shader hash.
#include <cstdio>
#include <string>

#include "gpu/vk/vk_cache_hash.h"

int main()
{
    const std::string a = "float4 main() : SV_Target { return 1; }";
    const std::string b = a + " ";
    const std::string c = "float4 main() : SV_Target { return 2; }";

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { fprintf(stderr, "FAIL %s\n", what); failures++; }
        else printf("ok   %s\n", what);
    };

    check(HlslContentHash(a) == HlslContentHash(a), "same text hashes identically");
    check(HlslContentHash(a) != HlslContentHash(b), "a trailing space changes the hash");
    check(HlslContentHash(a) != HlslContentHash(c), "a different program hashes differently");
    check(HlslContentHash("") != HlslContentHash(a), "empty text differs from non-empty");

    if (failures) { printf("vk_cache_key_test: %d failure(s)\n", failures); return 1; }
    printf("vk_cache_key_test: ok (hash tracks the HLSL text)\n");
    return 0;
}
