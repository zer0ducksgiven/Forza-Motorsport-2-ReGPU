#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_draw.h"
#include "gpu/vk/vk_dedupe.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "memory.h"
#include "log.h"

#include <cstring>

namespace vk
{
uint64_t g_cmdDrawCount = 0;

namespace
{
bool MakeRing(const Context& context, DrawRing& ring, VkDeviceSize size, VkBufferUsageFlags usage)
{
    ring.size = size;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context.device, &bi, nullptr, &ring.buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(context.device, ring.buffer, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    constexpr VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.memoryTypeIndex = FindMemory(context, req.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = FindMemory(context, req.memoryTypeBits, host);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(context.device, &ai, nullptr, &ring.memory) != VK_SUCCESS ||
        vkBindBufferMemory(context.device, ring.buffer, ring.memory, 0) != VK_SUCCESS)
        return false;
    if (vkMapMemory(context.device, ring.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&ring.cpu)) != VK_SUCCESS)
        return false;
    return true;
}

VkDeviceSize Align(VkDeviceSize value, VkDeviceSize alignment) { return (value + alignment - 1) & ~(alignment - 1); }
}

bool DrawInit(Context& context, DrawContext& draw, VkBuffer gphys, VkSampler samplers[4], VkDescriptorSet bindless2D,
    VkDescriptorSet bindlessCube, VkDeviceSize constantsBytes, VkDeviceSize listBytes)
{
    if (!MakeRing(context, draw.constants, constantsBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !MakeRing(context, draw.indices, listBytes / 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        !MakeRing(context, draw.guestMemory, listBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        return false;
    draw.constantsInfo = { draw.constants.buffer, 0, std::max(sizeof(gpu::VSConstants), sizeof(gpu::PSConstants)) };
    draw.bindless2D = bindless2D;
    draw.bindlessCube = bindlessCube;

    if (!CreateRendererLayout(context, draw.layout, &draw.set0Layout))
        return false;

    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 8 },
    };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &draw.pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = draw.pool;
    allocate.descriptorSetCount = 2;
    VkDescriptorSetLayout layouts[2] = { draw.set0Layout, draw.set0Layout };
    allocate.pSetLayouts = layouts;
    VkDescriptorSet allocated[2] = {};
    if (vkAllocateDescriptorSets(context.device, &allocate, allocated) != VK_SUCCESS)
        return false;
    draw.set0 = allocated[0];
    draw.set0Local = allocated[1];
    // Only this side knows whether the guest window exists; the draw path must not read in place without it.
    draw.hasGuestPhysical = gphys != VK_NULL_HANDLE;

    VkDescriptorBufferInfo guestMemory{ draw.guestMemory.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo guestPhysical{ gphys, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo samplerInfo[4]{};
    for (int i = 0; i < 4; i++)
        samplerInfo[i] = { samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

    VkWriteDescriptorSet writes[8]{};
    for (int i = 0; i < 8; i++)
    {
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[i].dstSet = draw.set0;
        writes[i].dstBinding = i < 2 ? uint32_t(i) : i < 4 ? uint32_t(100 + i - 2) : 200u;
        writes[i].dstArrayElement = i < 4 ? 0u : uint32_t(i - 4); // the samplers are one array binding
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
            : i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLER;
        if (i < 2)
            writes[i].pBufferInfo = &draw.constantsInfo;
        else if (i == 2)
            writes[i].pBufferInfo = &guestMemory;
        else if (i == 3)
            writes[i].pBufferInfo = &guestPhysical;
        else
            writes[i].pImageInfo = &samplerInfo[i - 4];
    }
    vkUpdateDescriptorSets(context.device, 8, writes, 0, nullptr);
    // The local set is the same chain with gphys aliasing the upload ring.
    for (VkWriteDescriptorSet& write : writes)
        write.dstSet = draw.set0Local;
    writes[3].pBufferInfo = &guestMemory;
    vkUpdateDescriptorSets(context.device, 8, writes, 0, nullptr);
    return true;
}

bool RingFetchOnly(const gpu::VSConstants& vs, const std::vector<uint32_t>* fetchConstants)
{
    if (vs.vte[1] & 1)
        return false;
    if (fetchConstants)
    {
        for (uint32_t k : *fetchConstants)
            if (vs.vf[k][2] && vs.vf[k][3])
                return false;
    }
    else
    {
        for (const auto& fetch : vs.vf)
            if (fetch[2] && fetch[3])
                return false;
    }
    return true;
}

void DrawDestroy(Context& context, DrawContext& draw)
{
    for (DrawRing* ring : { &draw.constants, &draw.indices, &draw.guestMemory })
    {
        if (ring->cpu)
            vkUnmapMemory(context.device, ring->memory);
        if (ring->buffer)
            vkDestroyBuffer(context.device, ring->buffer, nullptr);
        if (ring->memory)
            vkFreeMemory(context.device, ring->memory, nullptr);
    }
    if (draw.set0Layout)
        vkDestroyDescriptorSetLayout(context.device, draw.set0Layout, nullptr);
    if (draw.pool)
        vkDestroyDescriptorPool(context.device, draw.pool, nullptr);
    if (draw.layout)
        vkDestroyPipelineLayout(context.device, draw.layout, nullptr);
    draw = {};
}

void DrawResetState(DrawContext& draw)
{
    draw.pipeline = VK_NULL_HANDLE;
    draw.viewportValid = false;
    draw.scissorValid = false;
    draw.blendValid = false;
    draw.stencil = ~0u;
}

void DrawBeginFrame(DrawContext& draw)
{
    // g_cmdDrawCount is not reset here: the frame summary reads it before this frame's draws and reports a delta
    // against its own previous sample.
    draw.constants.used = 0;
    draw.indices.used = 0;
    draw.guestMemory.used = 0;
    draw.carSnapshotBytes = 0;
    draw.vertexUploads.clear();
    draw.perfPreFetchMs = draw.perfFetchLoopMs = draw.perfVertexMs = draw.perfConstantsMs = draw.perfBindMs = draw.perfCallMs = 0;
    DrawResetState(draw);
}

bool DrawList(const Context& context, DrawContext& draw, VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet textures,
    gpu::VSConstants& vs, const gpu::PSConstants& ps, const uint32_t* list, uint32_t count, const VkRect2D& scissor,
    const std::vector<uint32_t>* fetchConstants)
{
    static const bool profile = getenv("FM2_PERF") != nullptr;
    auto before = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto mark = [&](double& sink) {
        if (!profile)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (before != std::chrono::steady_clock::time_point{})
            sink += std::chrono::duration<double, std::milli>(now - before).count();
        before = now;
    };
    const VkDeviceSize alignment = 256;
    const uint32_t indexCount = (vs.vte[1] & 1) ? 0 : count;
    const VkDeviceSize listOffset = Align(draw.guestMemory.used, alignment);
    if (listOffset + VkDeviceSize(indexCount) * 4 > draw.guestMemory.size)
    {
        draw.guestOverflows++;
        if (draw.guestOverflows < 100 || draw.guestOverflows % 1000 == 0)
            Log("guest_overflow_vk: ps %016llx count %u offset %llu used %llu size %llu",
                (unsigned long long)draw.psHashDebug, indexCount, (unsigned long long)listOffset,
                (unsigned long long)draw.guestMemory.used, (unsigned long long)draw.guestMemory.size);
        return false;
    }
    if (indexCount)
        memcpy(draw.guestMemory.cpu + listOffset, list, size_t(indexCount) * 4);
    draw.guestMemory.used = listOffset + VkDeviceSize(indexCount) * 4;
    vs.info[0] = uint32_t(listOffset); // where the vertex shader finds the list

    // Small guest vertex buffers are copied into the ring here rather than read in place. This path
    // records a whole frame and submits at swap, so a buffer the guest rewrites after the draw is
    // recorded would otherwise be read at its new contents. The 1-1.4 MiB car body buffer also
    // needs a snapshot: reading it late gives stable positions with stale normals and extreme lighting.
    // FM2_VK_DIRECT_FETCH_KB: at or above this size a vertex buffer is read in place from guest memory
    // instead of being copied, apart from that car-buffer range. Set it very high to copy everything.
    static const uint32_t directFetchBytes = getenv("FM2_VK_DIRECT_FETCH_KB")
        ? uint32_t(atoi(getenv("FM2_VK_DIRECT_FETCH_KB"))) * 1024u
        : 16u * 1024u;
    // Without an imported guest window an in-place read would resolve against the upload ring and return zeros,
    // so everything is copied: the test at the copy site skips buffers of kDirectFetchBytes or more, hence a
    // threshold above every buffer rather than 0.
    const uint32_t kDirectFetchBytes = draw.hasGuestPhysical ? directFetchBytes : 0xFFFFFFFFu;
    const uint32_t fetchCount = fetchConstants ? uint32_t(fetchConstants->size()) : 96;
    mark(draw.perfPreFetchMs);
    for (uint32_t i = 0; i < fetchCount; i++)
    {
        const uint32_t k = fetchConstants ? (*fetchConstants)[i] : i;
        const uint32_t fetchBase = vs.vf[k][0], bytes = vs.vf[k][3]; // w: buffer size in bytes
        if (bytes == 0 || vs.vf[k][2] == 0)
            continue;
        const bool carLayout = (vs.vfl[0][0] & 0x7Fu) == 95u && vs.vfl[0][1] == 15u;
        const bool carSnapshot = directFetchBytes && carLayout && bytes >= (1u << 20) && bytes <= (1400u * 1024u);
        if (bytes >= kDirectFetchBytes && !carSnapshot)
            continue;
        const uint8_t* source = GuestPtr<const uint8_t>(0xA0000000 + (fetchBase & 0x1FFFFFFF));
        const uint64_t key = uint64_t(fetchBase) << 32 | bytes;
        // Small frames are cheaper to copy; larger composite frames need comparison to avoid overflowing the ring.
        // Car snapshots reuse the first copy of each buffer for the rest of the frame. Their bytes are excluded from
        // the small-buffer threshold so the car does not activate comparisons for every small fetch in the race.
        const bool dedupeActive = carSnapshot || draw.guestMemory.used - draw.carSnapshotBytes >= (8u << 20);
        const uint8_t* previous = nullptr;
        bool uploadMatches = false;
        if (auto it = draw.vertexUploads.find(key); it != draw.vertexUploads.end())
        {
            previous = draw.guestMemory.cpu + it->second;
            // Buffers over 128 bytes are compared by three sampled spans (see vk_dedupe.h): a full memcmp walks
            // guest memory on every draw and costs far more than the copies it saves.
            if (dedupeActive)
                uploadMatches = carSnapshot || UploadUnchanged(source, previous, bytes, true, 3);
        }
        if (uploadMatches && previous)
        {
            vs.vf[k][0] = uint32_t(previous - draw.guestMemory.cpu);
            vs.vf[k][2] = 0;
            continue;
        }
        const VkDeviceSize offset = Align(draw.guestMemory.used, alignment);
        if (offset + bytes > draw.guestMemory.size)
        {
            draw.guestOverflows++;
            break;
        }
        memcpy(draw.guestMemory.cpu + offset, source, bytes);
        if (carSnapshot)
            draw.carSnapshotBytes += bytes;
        draw.vertexUploads[key] = offset;
        vs.vf[k][0] = uint32_t(offset);
        vs.vf[k][2] = 0; // read this one out of the ring
        draw.guestMemory.used = offset + bytes;
    }

    mark(draw.perfFetchLoopMs);
    mark(draw.perfVertexMs);
    const VkDeviceSize vsOffset = Align(draw.constants.used, alignment);
    const VkDeviceSize psOffset = Align(vsOffset + sizeof(gpu::VSConstants), alignment);
    if (psOffset + sizeof(gpu::PSConstants) > draw.constants.size)
    {
        if (draw.constantOverflows++ < 4)
            fprintf(stderr, "vk: constant ring overflow: size %llu, used %llu, vs %llu, ps %llu, vertices %u\n",
                (unsigned long long)draw.constants.size, (unsigned long long)draw.constants.used, (unsigned long long)vsOffset,
                (unsigned long long)psOffset, count);
        return false;
    }
    if (&vs != reinterpret_cast<const gpu::VSConstants*>(draw.constants.cpu + vsOffset))
        memcpy(draw.constants.cpu + vsOffset, &vs, sizeof vs);
    if (&ps != reinterpret_cast<const gpu::PSConstants*>(draw.constants.cpu + psOffset))
        memcpy(draw.constants.cpu + psOffset, &ps, sizeof ps);
    draw.constants.used = psOffset + sizeof(gpu::PSConstants);

    if (draw.pipeline != pipeline)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        draw.pipeline = pipeline;
    }
    if (!draw.scissorValid || memcmp(&draw.scissor, &scissor, sizeof scissor))
    {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        draw.scissor = scissor;
        draw.scissorValid = true;
    }
    mark(draw.perfConstantsMs);
    // The constant blocks move every draw, so the chain is bound every draw with fresh dynamic offsets.
    const uint32_t offsets[2] = { uint32_t(vsOffset), uint32_t(psOffset) };
    const VkDescriptorSet cube = draw.bindlessCube ? draw.bindlessCube : textures;
    const VkDescriptorSet base = RingFetchOnly(vs, fetchConstants) ? draw.set0Local : draw.set0;
    const VkDescriptorSet sets[3] = { base, textures, cube };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, 0, 3, sets, 2, offsets);
    mark(draw.perfBindMs);
    g_cmdDrawCount++;
    vkCmdDraw(cmd, count, 1, 0, 0);
    mark(draw.perfCallMs);
    (void)context;
    return true;
}

bool DrawIndexed(const Context& context, DrawContext& draw, VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet textures,
    const gpu::VSConstants& vs, const gpu::PSConstants& ps, const uint32_t* indices, uint32_t count, bool indexed,
    const VkRect2D& scissor)
{
    const VkDeviceSize alignment = 256; // the alignment dynamic uniform offsets must respect on every driver
    const VkDeviceSize vsOffset = Align(draw.constants.used, alignment);
    const VkDeviceSize psOffset = Align(vsOffset + sizeof(gpu::VSConstants), alignment);
    const VkDeviceSize next = psOffset + sizeof(gpu::PSConstants);
    VkDeviceSize indexOffset = 0;
    if (indexed)
    {
        indexOffset = Align(draw.indices.used, alignment);
        if (indexOffset + VkDeviceSize(count) * 4 > draw.indices.size)
            return false;
    }
    if (next > draw.constants.size)
        return false;

    memcpy(draw.constants.cpu + vsOffset, &vs, sizeof vs);
    memcpy(draw.constants.cpu + psOffset, &ps, sizeof ps);
    draw.constants.used = next;
    if (indexed)
    {
        memcpy(draw.indices.cpu + indexOffset, indices, size_t(count) * 4);
        draw.indices.used = indexOffset + VkDeviceSize(count) * 4;
    }

    if (draw.pipeline != pipeline)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        draw.pipeline = pipeline;
    }
    if (!draw.scissorValid || memcmp(&draw.scissor, &scissor, sizeof scissor))
    {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        draw.scissor = scissor;
        draw.scissorValid = true;
    }
    const uint32_t offsets[2] = { uint32_t(vsOffset), uint32_t(psOffset) };
    // Set 2 is the cube array; a draw that has none still binds the 2D set there.
    VkDescriptorSet sets[3] = { draw.set0, textures, draw.bindlessCube ? draw.bindlessCube : textures };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, 0, 3, sets, 2, offsets);
    if (indexed)
    {
        vkCmdBindIndexBuffer(cmd, draw.indices.buffer, indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, count, 1, 0, 0, 0);
    }
    else
        vkCmdDraw(cmd, count, 1, 0, 0);
    g_cmdDrawCount++;
    (void)context;
    return true;
}

void DrawSetViewport(const Context& context, DrawContext& draw, VkCommandBuffer cmd, const VkViewport& viewport)
{
    if (draw.viewportValid && !memcmp(&draw.viewport, &viewport, sizeof viewport))
        return;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    draw.viewport = viewport;
    draw.viewportValid = true;
}

void DrawSetBlendConstants(const Context& context, DrawContext& draw, VkCommandBuffer cmd, const float rgba[4])
{
    if (draw.blendValid && !memcmp(draw.blend, rgba, sizeof draw.blend))
        return;
    vkCmdSetBlendConstants(cmd, rgba);
    memcpy(draw.blend, rgba, sizeof draw.blend);
    draw.blendValid = true;
}

void DrawSetStencilReference(const Context& context, DrawContext& draw, VkCommandBuffer cmd, uint32_t value)
{
    if (draw.stencil == value)
        return;
    vkCmdSetStencilReference(cmd, VK_STENCIL_FRONT_AND_BACK, value);
    draw.stencil = value;
}
}
