#pragma once
// Bindless texture cache: the Vulkan counterpart of the renderer's SRV heap. Game textures are
// decoded on the CPU (texture.cpp), uploaded once, and kept in a descriptor array the shaders
// index with the fetch slot's index — the same index the D3D12 path writes into its heap.
//
// The array is one descriptor set (set 1 for 2D, set 2 for cube in the renderer's layout),
// fixed-size and partially bound, with UPDATE_AFTER_BIND so a texture can be added while
// commands that only use earlier slots are in flight.
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

#include "gpu/vk/vk_context.h"

namespace vk
{
struct TextureCache
{
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<Image> slots;      // the image behind each bindless index
    std::vector<uint32_t> freeSlots;
    uint32_t capacity = 0;
    uint32_t next = 0; // the next untouched slot
    bool cube = false; // images are cube-compatible, six layers
};

// capacity is the number of bindless slots (the renderer's heap is kBindless entries).
// cube creates cube-compatible images (six layers) instead of 2D ones, for the set the shaders
// sample gcube from.
bool TextureCacheInit(Context& context, TextureCache& cache, uint32_t capacity, bool cube = false);
void TextureCacheShutdown(Context& context, TextureCache& cache);

// Uploads RGBA8 mip 0 (tightly packed, top row first) and returns its bindless index, or
// UINT32_MAX if the cache is full. Reuses a freed slot when there is one.
uint32_t TextureAdd(Context& context, TextureCache& cache, uint32_t width, uint32_t height, const void* rgba,
    VkImageUsageFlags extraUsage = 0, uint32_t levels = 1);
// Six faces, each tightly packed RGBA8, in +X -X +Y -Y +Z -Z order.
uint32_t TextureAddCube(Context& context, TextureCache& cache, uint32_t width, uint32_t height, const uint8_t* const faces[6]);
// Copies GPU-resolved faces into an existing cube, outside a render pass. Null or mismatched faces
// keep their previous contents. All images are left ready for shader reads.
bool TextureCopyCube(Context& context, TextureCache& cache, uint32_t index, VkCommandBuffer cmd, const Image* const faces[6],
    uint32_t width, uint32_t height, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
// Copies separately resolved mip levels into one image, outside a render pass. All images are left
// ready for shader reads.
bool TextureCopyMipChain(Context& context, TextureCache& cache, uint32_t index, VkCommandBuffer cmd,
    const Image* const levels[6], uint32_t count, uint32_t width, uint32_t height, VkFormat format);
// Reserves an index whose image is drawn into rather than uploaded (the renderer's resolves).
// The image is created with the given usage and left in COLOR_ATTACHMENT_OPTIMAL.
uint32_t TextureReserve(Context& context, TextureCache& cache, uint32_t width, uint32_t height,
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
void TextureFree(Context& context, TextureCache& cache, uint32_t index);
// A block-compressed texture (BC1-BC4), uploaded as blocks so the GPU samples it exactly the way
// the Direct3D 12 backend does. Copies are tightly packed (VkBufferImageCopy::bufferRowLength is
// in texels, not blocks — the mistake that hung a GPU context once), so the base image must be a
// multiple of 4 in both directions. blockRowPitch describes the source level; the staging copy
// is repacked tightly. extraUsage lets a test read the image back.
uint32_t TextureAddBlocks(Context& context, TextureCache& cache, uint32_t width, uint32_t height, VkFormat format,
    const void* blocks, uint32_t blockRowPitch, VkImageUsageFlags extraUsage = 0, uint32_t levels = 1,
    uint32_t level = 0);
// Pass UNDEFINED as oldLayout when initially populating a non-zero mip of a new chain.
bool TextureReplaceBlocks(Context& context, TextureCache& cache, uint32_t index, uint32_t width, uint32_t height,
    VkFormat format, const void* blocks, uint32_t blockRowPitch, uint32_t levels = 1, uint32_t level = 0,
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
// Replaces one RGBA8 mip in an existing slot (the renderer reuses an index when a texture's
// contents change), so no descriptor has to be rewritten.
bool TextureReplace(Context& context, TextureCache& cache, uint32_t index, uint32_t width, uint32_t height, const void* rgba,
    uint32_t levels = 1, uint32_t level = 0, VkImageLayout oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
