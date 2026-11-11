// Proves the block-compressed (BC1) upload path end to end against the real driver, without the
// game: upload a known mip chain through the same texture calls the renderer uses, copy every
// level back, and require the bytes to match exactly. A byte-exact round trip pins down the two things
// that are easy to get wrong and expensive to get wrong — the extent/row semantics of
// VkBufferImageCopy for block-compressed formats (a wrong bufferRowLength once hung a GPU
// context, Xid 109) and the image's layout transitions.
//
//   linux/vk.sh            # builds and runs it with the other native tests
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>

#include "gpu/texture.h"

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_texture.h"

namespace
{
// A BC1 block is 8 bytes: two RGB565 endpoints and 4 bytes of 2-bit indices. These are arbitrary
// but distinct, so a shifted or truncated copy cannot pass by accident.
uint16_t Rgb565(uint32_t r, uint32_t g, uint32_t b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

void MakeBlocks(std::vector<uint8_t>& out, uint32_t width, uint32_t height)
{
    out.clear();
    for (uint32_t by = 0; by < (height + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
        {
            const uint16_t c0 = Rgb565(bx * 40, by * 60, 0xFF), c1 = Rgb565(0xFF, 0xFF - by * 30, bx * 50);
            const uint8_t block[8] = { uint8_t(c0), uint8_t(c0 >> 8), uint8_t(c1), uint8_t(c1 >> 8),
                uint8_t(0xE4 + bx), uint8_t(0x1B * (by + 1)), uint8_t(0x55 ^ bx), uint8_t(0xAA ^ by) };
            out.insert(out.end(), block, block + 8);
        }
}
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const std::streamoff bytesLeft = file.tellg();
    if (bytesLeft < 0)
        return false;
    bytes.resize(size_t(bytesLeft));
    file.seekg(0);
    return !bytes.empty() && bool(file.read(reinterpret_cast<char*>(bytes.data()), bytesLeft));
}

int TestFixture(vk::Context& context, const char* directory)
{
    int result = 0;
    for (uint32_t slot = 2; slot <= 4; slot++)
    {
        char name[32];
        snprintf(name, sizeof name, "tex_%u.fetch", slot);
        std::vector<uint8_t> rawFetch;
        if (!ReadFile(std::filesystem::path(directory) / name, rawFetch) || rawFetch.size() != sizeof(TextureFetch))
            return 1;
        TextureFetch base;
        memcpy(&base, rawFetch.data(), sizeof base);
        const uint32_t kind = TextureBCKind(base.Format());
        if (kind < 1 || kind > 4)
        {
            printf("vk_bc_test: slot %u uses CPU decoding\n", slot);
            continue;
        }
        snprintf(name, sizeof name, "tex_%u.bin", slot);
        std::vector<uint8_t> source;
        if (!ReadFile(std::filesystem::path(directory) / name, source))
            return 1;
        const VkFormat format = kind == 1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : kind == 2 ? VK_FORMAT_BC2_UNORM_BLOCK : kind == 3 ? VK_FORMAT_BC3_UNORM_BLOCK
            : VK_FORMAT_BC4_UNORM_BLOCK;
        const uint32_t blockBytes = kind == 1 || kind == 4 ? 8 : 16;
        std::vector<TextureFetch> chain{ base };
        for (uint32_t level = 1;; level++)
        {
            TextureFetch next;
            if (!TextureMipLevel(base, level, next))
                break;
            chain.push_back(next);
        }
        std::vector<std::vector<uint8_t>> mips(chain.size());
        for (uint32_t level = 0; level < chain.size(); level++)
        {
            const uint32_t rowPitch = chain[level].Width() / 4 * blockBytes;
            mips[level].resize(size_t(rowPitch) * (chain[level].Height() / 4));
            if (!CopyTextureBlocks(chain[level], source.data(), mips[level].data(), rowPitch))
            {
                printf("vk_bc_test: slot %u mip %u block copy failed\n", slot, level);
                return 2;
            }
        }
        vk::TextureCache cache;
        if (!vk::TextureCacheInit(context, cache, 8))
            return 1;
        const uint32_t index = vk::TextureAddBlocks(context, cache, base.Width(), base.Height(), format, mips[0].data(),
            base.Width() / 4 * blockBytes, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, uint32_t(chain.size()));
        if (index == UINT32_MAX)
            return 1;
        for (uint32_t level = 1; level < chain.size(); level++)
            if (!vk::TextureReplaceBlocks(context, cache, index, base.Width(), base.Height(), format, mips[level].data(),
                    chain[level].Width() / 4 * blockBytes, uint32_t(chain.size()), level, VK_IMAGE_LAYOUT_UNDEFINED))
            {
                printf("vk_bc_test: slot %u mip %u upload failed\n", slot, level);
                return 1;
            }
        for (uint32_t level = 0; level < chain.size(); level++)
        {
            const uint32_t width = chain[level].Width(), height = chain[level].Height();
            void* readback = nullptr;
            VkBuffer out = vk::CreateBuffer(context, mips[level].size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
            if (!out)
                return 1;
            const bool ok = vk::Submit(context, [&](VkCommandBuffer cmd) {
                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
                copy.imageExtent = { width, height, 1 };
                vkCmdCopyImageToBuffer(cmd, cache.slots[index].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, out, 1, &copy);
            });
            if (!ok)
                return 1;
            size_t differing = 0;
            for (size_t i = 0; i < mips[level].size(); i++)
                differing += static_cast<const uint8_t*>(readback)[i] != mips[level][i];
            printf("vk_bc_test: slot %u mip %u, %ux%u BC%u, %zu bytes, %zu differ\n", slot, level, width, height, kind,
                mips[level].size(), differing);
            result |= differing != 0;
            vkDestroyBuffer(context.device, out, nullptr);
        }
        vk::TextureCacheShutdown(context, cache);
    }
    return result ? 2 : 0;
}

int main(int argc, char** argv)
{
    vk::Context context;
    if (!vk::Init(context, "out/vkcache-test"))
    {
        fprintf(stderr, "vk_bc_test: no Vulkan device\n");
        return 1;
    }
    printf("vk_bc_test: %s\n", context.props.deviceName);

    VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    vkGetPhysicalDeviceFeatures2(context.physical, &query);
    if (!query.features.textureCompressionBC)
    {
        printf("vk_bc_test: SKIP (no textureCompressionBC)\n");
        vk::Shutdown(context);
        return 0;
    }

    if (argc > 1)
    {
        const int result = TestFixture(context, argv[1]);
        vk::Shutdown(context);
        return result;
    }

    const uint32_t width = 16, height = 8, mipCount = 4;
    std::vector<std::vector<uint8_t>> mips(mipCount);
    for (uint32_t level = 0; level < mipCount; level++)
    {
        const uint32_t shiftedWidth = width >> level, shiftedHeight = height >> level;
        MakeBlocks(mips[level], shiftedWidth ? shiftedWidth : 1, shiftedHeight ? shiftedHeight : 1);
    }

    vk::TextureCache cache;
    if (!vk::TextureCacheInit(context, cache, 8))
    {
        fprintf(stderr, "vk_bc_test: cache init failed\n");
        return 1;
    }
    const uint32_t index = vk::TextureAddBlocks(context, cache, width, height, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, mips[0].data(),
        ((width + 3) / 4) * 8, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipCount);
    if (index == UINT32_MAX || cache.slots[index].levels != mipCount)
    {
        fprintf(stderr, "vk_bc_test: TextureAddBlocks failed\n");
        return 1;
    }
    for (uint32_t level = 1; level < mipCount; level++)
    {
        const uint32_t levelWidth = width >> level;
        const uint32_t blockRowPitch = ((levelWidth + 3) / 4) * 8;
        if (!vk::TextureReplaceBlocks(context, cache, index, width, height, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, mips[level].data(),
                blockRowPitch, mipCount, level, VK_IMAGE_LAYOUT_UNDEFINED))
        {
            fprintf(stderr, "vk_bc_test: initial mip %u upload failed\n", level);
            return 1;
        }
    }
    if (!vk::TextureReplaceBlocks(context, cache, index, width, height, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, mips.back().data(),
            8, mipCount, mipCount - 1))
    {
        fprintf(stderr, "vk_bc_test: replacement mip upload failed\n");
        return 1;
    }

    size_t differing = 0;
    for (uint32_t level = 0; level < mipCount; level++)
    {
        const uint32_t shiftedWidth = width >> level, shiftedHeight = height >> level;
        const uint32_t levelWidth = shiftedWidth ? shiftedWidth : 1;
        const uint32_t levelHeight = shiftedHeight ? shiftedHeight : 1;
        void* readback = nullptr;
        VkBuffer out = vk::CreateBuffer(context, mips[level].size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
        if (!out)
        {
            fprintf(stderr, "vk_bc_test: readback buffer failed\n");
            return 1;
        }
        const bool ok = vk::Submit(context, [&](VkCommandBuffer cmd) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            copy.imageExtent = { levelWidth, levelHeight, 1 };
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            vkCmdCopyImageToBuffer(cmd, cache.slots[index].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, out, 1, &copy);
        });
        if (!ok)
        {
            fprintf(stderr, "vk_bc_test: readback submit failed\n");
            return 1;
        }
        size_t levelDiffering = 0;
        for (size_t i = 0; i < mips[level].size(); i++)
            levelDiffering += static_cast<const uint8_t*>(readback)[i] != mips[level][i];
        differing += levelDiffering;
        printf("vk_bc_test: mip %u, %ux%u BC1, %zu bytes, %zu differ\n", level, levelWidth, levelHeight, mips[level].size(),
            levelDiffering);
        vkDestroyBuffer(context.device, out, nullptr);
    }
    vk::TextureCacheShutdown(context, cache);
    vk::Shutdown(context);
    return differing ? 2 : 0;
}
