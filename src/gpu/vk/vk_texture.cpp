#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_texture.h"

#include <cstdio>
#include <chrono>
#include <cstring>
#include <vector>

namespace vk
{
void DropSlotWrites(VkDescriptorSet set);

bool TextureCacheInit(Context& context, TextureCache& cache, uint32_t capacity, bool cube)
{
    cache.cube = cube;
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0; // set 1, binding 0: the 2D array (dxc's register space 1)
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = capacity;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    bindingFlags.bindingCount = 1;
    bindingFlags.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layout.pNext = &bindingFlags;
    layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(context.device, &layout, nullptr, &cache.layout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &cache.pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = cache.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &cache.layout;
    if (vkAllocateDescriptorSets(context.device, &allocate, &cache.set) != VK_SUCCESS)
        return false;

    cache.capacity = capacity;
    cache.slots.resize(capacity);
    return true;
}

void TextureCacheShutdown(Context& context, TextureCache& cache)
{
    DropSlotWrites(cache.set);
    for (Image& image : cache.slots)
        DestroyImage(context, image);
    if (cache.pool)
        vkDestroyDescriptorPool(context.device, cache.pool, nullptr);
    if (cache.layout)
        vkDestroyDescriptorSetLayout(context.device, cache.layout, nullptr);
    cache = {};
}

namespace
{
// Bindless slot writes are collected and applied in ONE vkUpdateDescriptorSets before the next queue submission
// (g_beforeSubmit): a call per new texture cost about 1.5 ms each on the NVIDIA driver against the 65536-entry
// update-after-bind set (23 first-use textures: 35 ms of one frame; 4.7 ms for the same frame on RADV).
// FM2_VK_IMMEDIATE_SLOT_WRITES=1 writes each slot at once instead.
struct PendingSlotWrite
{
    VkDevice device;
    VkDescriptorSet set;
    uint32_t index;
    VkImageView view;
};
std::vector<PendingSlotWrite> g_pendingSlots;

void FlushSlotWrites(const Context& context)
{
    if (g_pendingSlots.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    infos.reserve(g_pendingSlots.size());
    writes.reserve(g_pendingSlots.size());
    size_t kept = 0;
    for (const PendingSlotWrite& pending : g_pendingSlots)
    {
        if (pending.device != context.device)
        {
            g_pendingSlots[kept++] = pending; // another device's (the tests run several contexts)
            continue;
        }
        infos.push_back({ VK_NULL_HANDLE, pending.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pending.set;
        write.dstBinding = 0;
        write.dstArrayElement = pending.index;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &infos.back(); // (reserved above: no reallocation)
        writes.push_back(write);
    }
    g_pendingSlots.resize(kept);
    if (!writes.empty())
        vkUpdateDescriptorSets(context.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    g_slotWrites += writes.size();
    g_slotWriteMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool WriteSlot(Context& context, TextureCache& cache, uint32_t index, const Image& image)
{
    static const bool immediate = getenv("FM2_VK_IMMEDIATE_SLOT_WRITES") != nullptr;
    if (!immediate)
    {
        g_beforeSubmit = FlushSlotWrites;
        for (PendingSlotWrite& pending : g_pendingSlots)
            if (pending.set == cache.set && pending.index == index && pending.device == context.device)
            {
                pending.view = image.view; // the slot was rebuilt before its first write went out
                return true;
            }
        g_pendingSlots.push_back({ context.device, cache.set, index, image.view });
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    g_slotWrites++;
    struct Timed
    {
        std::chrono::steady_clock::time_point start;
        ~Timed() { g_slotWriteMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
    } timed{ start };
    VkDescriptorImageInfo info{ VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = cache.set;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
    return true;
}
}

void DropSlotWrites(VkDescriptorSet set)
{
    std::erase_if(g_pendingSlots, [set](const PendingSlotWrite& pending) { return pending.set == set; });
}

uint32_t TextureAdd(Context& context, TextureCache& cache, uint32_t width, uint32_t height, const void* rgba,
    VkImageUsageFlags extraUsage, uint32_t levels)
{
    if (!levels || levels > 32)
        return UINT32_MAX;
    uint32_t index;
    if (!cache.freeSlots.empty())
    {
        index = cache.freeSlots.back();
        cache.freeSlots.pop_back();
    }
    else if (cache.next < cache.capacity)
        index = cache.next++;
    else
        return UINT32_MAX;

    Image& slot = cache.slots[index];
    const bool create = !slot.image || slot.format != VK_FORMAT_R8G8B8A8_UNORM || slot.width != width ||
        slot.height != height || slot.levels != levels;
    if (create)
    {
        if (slot.image)
            DestroyImage(context, slot);
        slot = CreateImage(context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extraUsage,
            cache.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, cache.cube ? 6 : 1, VK_SAMPLE_COUNT_1_BIT, levels);
        if (!slot.image)
            return UINT32_MAX;
        WriteSlot(context, cache, index, slot);
    }
    UploadImage(context, slot, rgba, VkDeviceSize(width) * height * 4,
        create ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return index;
}

namespace
{
uint32_t BlockBytes(VkFormat format)
{
    return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format == VK_FORMAT_BC1_RGB_UNORM_BLOCK || format == VK_FORMAT_BC4_UNORM_BLOCK ? 8u : 16u;
}

void UploadBlocks(Context& context, const Image& image, uint32_t width, uint32_t height, const void* blocks,
    uint32_t blockRowPitch, VkImageLayout oldLayout, uint32_t level)
{
    if (level >= image.levels || level >= 32)
        return;
    const uint32_t shiftedWidth = width >> level;
    const uint32_t shiftedHeight = height >> level;
    const uint32_t levelWidth = shiftedWidth ? shiftedWidth : 1;
    const uint32_t levelHeight = shiftedHeight ? shiftedHeight : 1;
    const uint32_t rowBytes = ((levelWidth + 3) / 4) * BlockBytes(image.format);
    const uint32_t rows = (levelHeight + 3) / 4;
    if (blockRowPitch < rowBytes)
        return;
    const VkDeviceSize bytes = VkDeviceSize(rowBytes) * rows;
    // Tightly packed rows for the copy; repacked only when the source pitch carries padding.
    std::vector<uint8_t> packed;
    if (blockRowPitch != rowBytes)
    {
        packed.resize(size_t(bytes));
        for (uint32_t row = 0; row < rows; row++)
            memcpy(packed.data() + size_t(row) * rowBytes, static_cast<const uint8_t*>(blocks) + size_t(row) * blockRowPitch, rowBytes);
        blocks = packed.data();
    }
    // Non-blocking, with a pooled staging buffer (see SubmitUpload).
    const bool uploadOk = SubmitUpload(context, image.image, blocks, bytes, [&](VkCommandBuffer cb, VkBuffer staging, VkDeviceSize offset) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = oldLayout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
        b.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        copy.imageExtent = { levelWidth, levelHeight, 1 };
        copy.bufferOffset = offset;
        copy.bufferRowLength = 0;  // tightly packed: rows are imageExtent.width texels (whole blocks)
        copy.bufferImageHeight = 0;
        vkCmdCopyBufferToImage(cb, staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    static const bool trace = getenv("FM2_VK_TRACE_BC") != nullptr;
    if (trace)
    {
        fprintf(stderr, "vk: BC upload %ux%u level %u, submit %d\n", levelWidth, levelHeight, level, int(uploadOk));
    }
}

// A slot holding a `format` image of this size and mip count, replacing whatever was there before.
bool EnsureBlockSlot(Context& context, TextureCache& cache, uint32_t index, uint32_t width, uint32_t height, VkFormat format,
    uint32_t levels, VkImageUsageFlags extraUsage, bool& created)
{
    Image& slot = cache.slots[index];
    created = false;
    if (slot.image && slot.format == format && slot.width == width && slot.height == height && slot.levels == levels)
        return true;
    if (slot.image)
        DestroyImage(context, slot);
    slot = CreateImage(context, width, height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extraUsage,
        VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, levels,
        format == VK_FORMAT_BC4_UNORM_BLOCK ? VkComponentMapping{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
            VK_COMPONENT_SWIZZLE_R } : VkComponentMapping{});
    if (!slot.image)
        return false;
    WriteSlot(context, cache, index, slot);
    created = true;
    return true;
}
}

uint32_t TextureAddBlocks(Context& context, TextureCache& cache, uint32_t width, uint32_t height, VkFormat format,
    const void* blocks, uint32_t blockRowPitch, VkImageUsageFlags extraUsage, uint32_t levels, uint32_t level)
{
    if (width % 4 || height % 4 || !width || !height || !levels || levels > 32 || level >= levels)
        return UINT32_MAX;
    uint32_t index;
    if (!cache.freeSlots.empty())
    {
        index = cache.freeSlots.back();
        cache.freeSlots.pop_back();
    }
    else if (cache.next < cache.capacity)
        index = cache.next++;
    else
        return UINT32_MAX;
    bool created = false;
    if (!EnsureBlockSlot(context, cache, index, width, height, format, levels, extraUsage, created))
        return UINT32_MAX;
    UploadBlocks(context, cache.slots[index], width, height, blocks, blockRowPitch,
        created ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, level);
    return index;
}

bool TextureReplaceBlocks(Context& context, TextureCache& cache, uint32_t index, uint32_t width, uint32_t height,
    VkFormat format, const void* blocks, uint32_t blockRowPitch, uint32_t levels, uint32_t level, VkImageLayout oldLayout)
{
    if (index >= cache.capacity || width % 4 || height % 4 || !width || !height || !levels || levels > 32 || level >= levels)
        return false;
    bool created = false;
    if (!EnsureBlockSlot(context, cache, index, width, height, format, levels, 0, created))
        return false;
    UploadBlocks(context, cache.slots[index], width, height, blocks, blockRowPitch,
        created ? VK_IMAGE_LAYOUT_UNDEFINED : oldLayout, level);
    return true;
}

uint32_t TextureReserve(Context& context, TextureCache& cache, uint32_t width, uint32_t height, VkFormat format)
{
    uint32_t index;
    if (!cache.freeSlots.empty())
    {
        index = cache.freeSlots.back();
        cache.freeSlots.pop_back();
    }
    else if (cache.next < cache.capacity)
        index = cache.next++;
    else
        return UINT32_MAX;
    if (cache.slots[index].image)
        DestroyImage(context, cache.slots[index]);
    cache.slots[index] = CreateImage(context, width, height, format,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!cache.slots[index].image)
        return UINT32_MAX;
    WriteSlot(context, cache, index, cache.slots[index]);
    // The descriptor points at a SHADER_READ image; the resolve moves it there when it is done.
    return index;
}

uint32_t TextureAddCube(Context& context, TextureCache& cache, uint32_t width, uint32_t height, const uint8_t* const faces[6])
{
    uint32_t index;
    if (!cache.freeSlots.empty())
    {
        index = cache.freeSlots.back();
        cache.freeSlots.pop_back();
    }
    else if (cache.next < cache.capacity)
        index = cache.next++;
    else
        return UINT32_MAX;
    if (cache.slots[index].image)
        DestroyImage(context, cache.slots[index]);
    cache.slots[index] = CreateImage(context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 6);
    if (!cache.slots[index].image)
        return UINT32_MAX;
    WriteSlot(context, cache, index, cache.slots[index]);
    // One upload per face: cheap enough for the handful of cube maps a scene has.
    for (uint32_t face = 0; face < 6; face++)
    {
        void* staging = nullptr;
        const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
        VkBuffer buffer = CreateBuffer(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging);
        if (!buffer)
            return UINT32_MAX;
        memcpy(staging, faces[face], size_t(bytes));
        Submit(context, [&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy.image = cache.slots[index].image;
            toCopy.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1 };
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                &toCopy);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyBufferToImage(cmd, buffer, cache.slots[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            VkImageMemoryBarrier toSampled = toCopy;
            toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toSampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                nullptr, 1, &toSampled);
        });
        vkDeviceWaitIdle(context.device);
        vkDestroyBuffer(context.device, buffer, nullptr);
    }
    return index;
}

bool TextureCopyCube(Context& context, TextureCache& cache, uint32_t index, VkCommandBuffer cmd, const Image* const faces[6],
    uint32_t width, uint32_t height, VkFormat format)
{
    if (index >= cache.capacity || !cache.cube || !width || !height)
        return false;
    Image& cube = cache.slots[index];
    const bool create = !cube.image || cube.format != format || cube.width != width || cube.height != height;
    if (create)
    {
        if (cube.image)
            DestroyImage(context, cube);
        cube = CreateImage(context, width, height, format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 6);
        if (!cube.image)
            return false;
        WriteSlot(context, cache, index, cube);
    }

    VkImageMemoryBarrier destination{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    destination.srcAccessMask = create ? 0 : VK_ACCESS_SHADER_READ_BIT;
    destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    destination.oldLayout = create ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination.srcQueueFamilyIndex = destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destination.image = cube.image;
    destination.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    vkCmdPipelineBarrier(cmd, create ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &destination);

    for (uint32_t face = 0; face < 6; face++)
    {
        const Image* source = faces[face];
        if (!source || !source->image || source->format != format)
            continue;
        VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        read.image = source->image;
        read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &read);
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
        region.extent = { std::min(width, source->width), std::min(height, source->height), 1 };
        vkCmdCopyImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &region);
        read.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &read);
    }

    destination.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    destination.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &destination);
    return true;
}

bool TextureCopyMipChain(Context& context, TextureCache& cache, uint32_t index, VkCommandBuffer cmd,
    const Image* const levels[6], uint32_t count, uint32_t width, uint32_t height, VkFormat format)
{
    if (index >= cache.capacity || cache.cube || !count || count > 6 || !width || !height)
        return false;
    Image& chain = cache.slots[index];
    const bool create = !chain.image || chain.format != format || chain.width != width || chain.height != height || chain.levels != count;
    if (create)
    {
        if (chain.image)
            DestroyImage(context, chain);
        chain = CreateImage(context, width, height, format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, count);
        if (!chain.image)
            return false;
        WriteSlot(context, cache, index, chain);
    }

    VkImageMemoryBarrier destination{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    destination.srcAccessMask = create ? 0 : VK_ACCESS_SHADER_READ_BIT;
    destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    destination.oldLayout = create ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination.srcQueueFamilyIndex = destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destination.image = chain.image;
    destination.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, count, 0, 1 };
    vkCmdPipelineBarrier(cmd, create ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &destination);

    for (uint32_t level = 0; level < count; level++)
    {
        const Image* source = levels[level];
        if (!source || !source->image || source->format != format)
            return false;
        VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        read.image = source->image;
        read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &read);
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        region.extent = { std::max(1u, width >> level), std::max(1u, height >> level), 1 };
        region.extent.width = std::min(region.extent.width, source->width);
        region.extent.height = std::min(region.extent.height, source->height);
        vkCmdCopyImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &region);
        read.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &read);
    }

    destination.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    destination.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &destination);
    return true;
}

void TextureFree(Context& context, TextureCache& cache, uint32_t index)
{
    if (index >= cache.capacity || !cache.slots[index].image)
        return;
    // Keep the image: a freed slot is reused by the next add, and its descriptor already points
    // at it. Only the slot is returned to the free list.
    cache.freeSlots.push_back(index);
}

bool TextureReplace(Context& context, TextureCache& cache, uint32_t index, uint32_t width, uint32_t height, const void* rgba,
    uint32_t levels, uint32_t level, VkImageLayout oldLayout)
{
    if (index >= cache.capacity || !cache.slots[index].image || !levels || levels > 32 || level >= levels)
        return false;
    const Image& image = cache.slots[index];
    const bool recreate = image.format != VK_FORMAT_R8G8B8A8_UNORM || image.width != width || image.height != height ||
        image.levels != levels;
    if (recreate)
    {
        // Different image metadata needs a new image; the descriptor is rewritten, so shaders that
        // already have the index keep working.
        DestroyImage(context, cache.slots[index]);
        cache.slots[index] = CreateImage(context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, levels);
        if (!cache.slots[index].image)
            return false;
        WriteSlot(context, cache, index, cache.slots[index]);
    }
    const uint32_t levelWidth = std::max(width >> level, 1u), levelHeight = std::max(height >> level, 1u);
    UploadImage(context, cache.slots[index], rgba, VkDeviceSize(levelWidth) * levelHeight * 4,
        recreate ? VK_IMAGE_LAYOUT_UNDEFINED : oldLayout, level);
    return true;
}
}
