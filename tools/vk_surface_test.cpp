// The EDRAM surface model: two passes into one surface (cleared, then loaded), then a resolve
// to a single-sampled image and a readback — the path a frame of the game takes. It also checks
// the thing MSAA is for: that a resolve blends a triangle's edge (1x cannot, 4x must).
//
//   ./out/vk_surface_test
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_shader.h"
#include "gpu/vk/vk_surface.h"
#include "gpu/vk/vk_resolve.h"
#include "gpu/vk/vk_texture.h"

namespace
{
const char kQuadVs[] = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

// A strip covering the left half, its edge deliberately off a pixel boundary so a multisampled
// resolve has something to blend.
VSOut main(uint id : SV_VertexID)
{
    float2 p = float2(id & 1, (id >> 1) & 1);
    VSOut o;
    o.pos = float4(p.x * 0.5f - 1.0f + 0.0015f, p.y * 2.0f - 1.0f, 0, 1);
    o.uv = float4(p, 0, 1);
    return o;
}
)";

const char kColorPs[] = R"(
cbuffer PSConstants : register(b1)
{
    float4 pc[256];
};

struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target0
{
    return pc[0];
}
)";

VkShaderModule Module(vk::Context& context, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(context.device, &ci, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

VkPipeline MakePipeline(vk::Context& context, VkShaderModule vs, VkShaderModule ps, VkPipelineLayout layout, VkRenderPass pass,
    VkSampleCountFlagBits samples)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = samples;
    // The render pass has a depth attachment, so the pipeline must describe depth state even
    // when the test does not use it.
    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;
    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = layout;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(context.device, context.pipelineCache, 1, &info, nullptr, &pipeline) == VK_SUCCESS ? pipeline
        : VK_NULL_HANDLE;
}
}

int main()
{
    const uint32_t width = 320, height = 200;
    vk::Context context;
    if (!vk::Init(context, "out/vkcache-test"))
        return 1;
    printf("vk: %s\n", context.props.deviceName);

    std::vector<uint32_t> vsCode, psCode;
    std::string error;
    if (!vk::CompileHlsl(kQuadVs, false, vk::HashSource(kQuadVs), "out/vkcache", vsCode, &error) ||
        !vk::CompileHlsl(kColorPs, true, vk::HashSource(kColorPs), "out/vkcache", psCode, &error))
    {
        fprintf(stderr, "vk: shader: %s\n", error.c_str());
        return 1;
    }
    VkShaderModule vs = Module(context, vsCode), ps = Module(context, psCode);

    vk::SurfaceCache surfaces;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE;
    if (!vk::CreateRendererLayout(context, layout, &set0Layout))
        return 1;

    // The pass reads its colour from the pixel constants, so a descriptor set with the two
    // dynamic constant buffers has to be bound (an unbound one is not a defined read).
    void* constantsCpu = nullptr;
    VkBuffer constants = vk::CreateBuffer(context, 65536,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &constantsCpu);
    const VkDeviceSize psOffset = 32768;
    float* pc = reinterpret_cast<float*>(static_cast<uint8_t*>(constantsCpu) + psOffset);
    pc[0] = 1.0f; // the colour the pixel shader returns
    pc[1] = 0.0f;
    pc[2] = 0.0f;
    pc[3] = 1.0f;
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(context.device, &samplerInfo, nullptr, &sampler);
    VkDescriptorPoolSize poolSizes[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 4 } };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &pool);
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set0Layout;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(context.device, &allocate, &set0);
    VkDescriptorBufferInfo bufferInfo{ constants, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo samplerDesc{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[8]{};
    for (int i = 0; i < 8; i++)
    {
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[i].dstSet = set0;
        writes[i].dstBinding = i < 2 ? uint32_t(i) : i < 4 ? uint32_t(100 + i - 2) : 200u;
        writes[i].dstArrayElement = i < 4 ? 0u : uint32_t(i - 4);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
            : i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLER;
        if (i < 4)
            writes[i].pBufferInfo = &bufferInfo;
        else
            writes[i].pImageInfo = &samplerDesc;
    }
    vkUpdateDescriptorSets(context.device, 8, writes, 0, nullptr);

    int failures = 0;
    for (VkSampleCountFlagBits samples : { VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_4_BIT })
    {
        vk::Surface* surface = vk::SurfaceAcquire(context, surfaces, 0xD0000000, width, height, samples);
        if (!surface)
        {
            fprintf(stderr, "vk: no surface\n");
            return 1;
        }
        // The pipeline must match the render pass's sample count; the test builds one per pass.
        VkPipeline pipeline = MakePipeline(context, vs, ps, layout, surface->pass, samples);
        if (!pipeline)
        {
            fprintf(stderr, "vk: no pipeline\n");
            return 1;
        }
        vk::Image resolved = vk::CreateImage(context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        void* readback = nullptr;
        VkBuffer copy = vk::CreateBuffer(context, VkDeviceSize(width) * height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);

        // Two passes: clear to blue, draw red over the left (1x) or a half triangle (MSAA).
        const bool ok = vk::Submit(context, [&](VkCommandBuffer cmd) {
            VkClearColorValue blue{};
            blue.float32[0] = 0.0f;
            blue.float32[1] = 0.0f;
            blue.float32[2] = 1.0f;
            blue.float32[3] = 1.0f;
            vk::SurfaceBegin(context, *surface, cmd, &blue);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkViewport v{ 0, 0, float(width), float(height), 0, 1 };
            VkRect2D s{ { 0, 0 }, { width, height } };
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &s);
            const uint32_t offsets[2] = { 0, uint32_t(psOffset) };
            VkDescriptorSet sets[3] = { set0, set0, set0 }; // set 1/2 unused by this shader
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 2, offsets);
            vkCmdDraw(cmd, 4, 1, 0, 0); // the strip: left half of the surface
            vk::SurfaceEnd(context, *surface, cmd);
            vk::SurfaceBegin(context, *surface, cmd, nullptr); // a resolve may reopen the pass: load, not clear
            vk::SurfaceEnd(context, *surface, cmd);
            vk::SurfaceShaderRead(context, *surface, cmd); // a guest resolve leaves the source sampled
            vk::SurfaceResolve(context, *surface, cmd, resolved.image, width, height);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(cmd, resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, 1, &region);
        });
        if (!ok)
        {
            fprintf(stderr, "vk: submission failed\n");
            return 1;
        }

        auto half = [](uint16_t h) {
            const uint32_t sign = uint32_t(h & 0x8000u) << 16;
            uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits = 0;
            if (exp == 0)
                bits = sign;
            else if (exp == 31)
                bits = sign | 0x7F800000u | (man << 13);
            else
                bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
            float f;
            memcpy(&f, &bits, 4);
            return f;
        };
        const uint16_t* px = static_cast<const uint16_t*>(readback);
        // The clear survives where the strip never covers: the top-right corner.
        const uint16_t* corner = px + (size_t(4) * width + (width - 5)) * 4;
        const float cornerBlue = half(corner[2]);
        // The drawn area: the bottom-left corner inside the triangle.
        const uint16_t* inside = px + (size_t(height - 5) * width + 4) * 4;
        const float insideRed = half(inside[0]);
        size_t blended = 0;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                const uint16_t* p = px + (size_t(y) * width + x) * 4;
                const float r = half(p[0]), b = half(p[2]);
                blended += r > 0.1f && b > 0.1f;
            }
        printf("vk: %ux: corner blue %.2f, inside red %.2f, %zu blended pixels\n", uint32_t(samples), cornerBlue, insideRed, blended);
        failures += !(cornerBlue > 0.9f && insideRed > 0.9f);
        // 1x has hard edges: a pixel is one colour or the other. 4x must blend along the edge.
        failures += samples == VK_SAMPLE_COUNT_4_BIT ? (blended == 0) : (blended > width);
        if (samples == VK_SAMPLE_COUNT_1_BIT)
        {
            vk::Resolve resolver;
            vk::Image hdrResolved = vk::CreateImage(context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            void* hdrReadback = nullptr;
            VkBuffer hdrCopy = vk::CreateBuffer(context, VkDeviceSize(width) * height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &hdrReadback);
            pc[0] = 8.0f;
            pc[1] = 4.0f;
            pc[2] = 2.0f;
            const bool hdrOk = vk::ResolveInit(context, resolver, {}) && hdrResolved.image && hdrCopy && vk::Submit(context, [&](VkCommandBuffer cmd) {
                VkClearColorValue black{};
                vk::SurfaceBegin(context, *surface, cmd, &black);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                VkViewport v{ 0, 0, float(width), float(height), 0, 1 };
                VkRect2D s{ { 0, 0 }, { width, height } };
                vkCmdSetViewport(cmd, 0, 1, &v);
                vkCmdSetScissor(cmd, 0, 1, &s);
                const uint32_t offsets[2] = { 0, uint32_t(psOffset) };
                VkDescriptorSet sets[3] = { set0, set0, set0 };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 2, offsets);
                vkCmdDraw(cmd, 4, 1, 0, 0);
                vk::SurfaceEnd(context, *surface, cmd);
                vk::SurfaceShaderRead(context, *surface, cmd);
                if (!vk::ResolveImage(context, resolver, cmd, surface->color, hdrResolved, width, height, s, false,
                        VK_IMAGE_LAYOUT_UNDEFINED, false, 0.25f, VK_FORMAT_R16G16B16A16_SFLOAT))
                    return;
                VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                read.image = hdrResolved.image;
                read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                    1, &read);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { width, height, 1 };
                vkCmdCopyImageToBuffer(cmd, hdrResolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdrCopy, 1, &region);
            });
            float hdrRed = 0, hdrGreen = 0, hdrBlue = 0;
            if (hdrOk)
            {
                const uint16_t* hdrPx = static_cast<const uint16_t*>(hdrReadback) + (size_t(height - 5) * width + 4) * 4;
                hdrRed = half(hdrPx[0]);
                hdrGreen = half(hdrPx[1]);
                hdrBlue = half(hdrPx[2]);
            }
            printf("vk: HDR resolve: %.3f %.3f %.3f (want 2 1 0.5)\n", hdrRed, hdrGreen, hdrBlue);
            failures += !(hdrOk && hdrRed == 2.0f && hdrGreen == 1.0f && hdrBlue == 0.5f);
            vk::ResolveFrameComplete(context, resolver);
            vkDeviceWaitIdle(context.device);
            vkDestroyBuffer(context.device, hdrCopy, nullptr);
            vk::DestroyImage(context, hdrResolved);
            vk::ResolveShutdown(context, resolver);
            pc[0] = 1.0f;
            pc[1] = 0.0f;
            pc[2] = 0.0f;
        }
        vkDeviceWaitIdle(context.device);
        vkDestroyBuffer(context.device, copy, nullptr);
        vkDestroyPipeline(context.device, pipeline, nullptr);
        vk::DestroyImage(context, resolved);
    }

    vk::Surface* depthSurface = vk::SurfaceAcquire(context, surfaces, 0xD0000000, width, height, VK_SAMPLE_COUNT_1_BIT);
    vk::Resolve depthResolver;
    vk::Image depthResolved = vk::CreateImage(context, width, height, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void* depthReadback = nullptr;
    VkBuffer depthCopy = vk::CreateBuffer(context, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &depthReadback);
    void* initialDepthReadback = nullptr;
    VkBuffer initialDepthCopy = vk::CreateBuffer(context, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &initialDepthReadback);
    if (!depthSurface || !vk::ResolveInit(context, depthResolver, {}) || !depthResolved.image || !depthCopy || !initialDepthCopy)
        return 1;
    const bool depthOk = vk::Submit(context, [&](VkCommandBuffer cmd) {
        depthSurface = vk::SurfaceAcquire(context, surfaces, 0xD0000000, width, height, VK_SAMPLE_COUNT_1_BIT, 0xABC);
        vk::SurfaceBegin(context, *depthSurface, cmd, nullptr);
        vk::SurfaceEnd(context, *depthSurface, cmd);
        vk::SurfaceDepthShaderRead(context, *depthSurface, cmd);
        const VkRect2D full{ { 0, 0 }, { width, height } };
        if (!vk::ResolveImage(context, depthResolver, cmd, depthSurface->depth, depthResolved, width, height, full,
                false, VK_IMAGE_LAYOUT_UNDEFINED, true))
            return;
        VkImageMemoryBarrier initialRead{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        initialRead.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        initialRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        initialRead.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        initialRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        initialRead.srcQueueFamilyIndex = initialRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initialRead.image = depthResolved.image;
        initialRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &initialRead);
        VkBufferImageCopy initialRegion{};
        initialRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        initialRegion.imageExtent = { 1, 1, 1 };
        vkCmdCopyImageToBuffer(cmd, depthResolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, initialDepthCopy, 1, &initialRegion);
        vk::SurfaceBegin(context, *depthSurface, cmd, nullptr);
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        clear.clearValue.depthStencil = { 0.375f, 0 };
        const VkClearRect clearRect{ full, 0, 1 };
        vkCmdClearAttachments(cmd, 1, &clear, 1, &clearRect);
        vk::SurfaceEnd(context, *depthSurface, cmd);
        vk::SurfaceDepthShaderRead(context, *depthSurface, cmd);
        if (!vk::ResolveImage(context, depthResolver, cmd, depthSurface->depth, depthResolved, width, height, full,
                false, VK_IMAGE_LAYOUT_UNDEFINED, true))
            return;
        VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        read.image = depthResolved.image;
        read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            1, &read);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(cmd, depthResolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthCopy, 1, &region);
    });
    vk::ResolveFrameComplete(context, depthResolver);
    size_t depthDiffer = 0;
    if (depthOk)
        for (uint32_t i = 0; i < width * height; i++)
        {
            float value = 0;
            memcpy(&value, static_cast<const float*>(depthReadback) + i, sizeof value);
            depthDiffer += value != 0.375f;
        }
    else
        depthDiffer = width * height;
    printf("vk: depth resolve: %zu of %u pixels differ (want 0.375)\n", depthDiffer, width * height);
    failures += depthDiffer != 0;
    float initialDepth = 0;
    memcpy(&initialDepth, initialDepthReadback, sizeof initialDepth);
    printf("vk: initial depth: %.3f (want 1)\n", initialDepth);
    failures += initialDepth != 1.0f;
    vkDeviceWaitIdle(context.device);
    vkDestroyBuffer(context.device, depthCopy, nullptr);
    vkDestroyBuffer(context.device, initialDepthCopy, nullptr);
    vk::DestroyImage(context, depthResolved);
    vk::ResolveShutdown(context, depthResolver);

    const VkImage originalColor = depthSurface->color.image, originalDepth = depthSurface->depth.image;
    vk::Surface* independent = vk::SurfaceAcquire(context, surfaces, 0xD0000000, width, height, VK_SAMPLE_COUNT_1_BIT, 0x400);
    const bool independentOk = independent && independent == depthSurface && independent->color.image == originalColor &&
        independent->depth.image != originalDepth && independent->depthBase == 0x400 && independent->depthState &&
        independent->depthState->image.image == independent->depth.image && surfaces.depths.size() == 3;
    printf("vk: independent depth base: %s\n", independentOk ? "ok" : "FAILED");
    failures += !independentOk;

    constexpr uint32_t copyWidth = 64, copyHeight = 64;
    vk::Resolve orientationResolver;
    vk::Image orientationImage = vk::CreateImage(context, copyWidth, copyHeight, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void* orientationReadback = nullptr;
    VkBuffer orientationCopy = vk::CreateBuffer(context, VkDeviceSize(copyWidth) * copyHeight * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &orientationReadback);
    bool framebufferReused = false;
    const bool orientationOk = vk::ResolveInit(context, orientationResolver, {}) && orientationImage.image && orientationCopy &&
        vk::Submit(context, [&](VkCommandBuffer cmd) {
            VkClearColorValue blue{};
            blue.float32[2] = blue.float32[3] = 1.0f;
            vk::SurfaceBegin(context, *depthSurface, cmd, &blue);
            VkClearAttachment red{};
            red.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            red.clearValue.color = { { 1.0f, 0.0f, 0.0f, 1.0f } };
            const VkRect2D top{ { 0, 0 }, { width, 32 } };
            const VkClearRect topRect{ top, 0, 1 };
            vkCmdClearAttachments(cmd, 1, &red, 1, &topRect);
            vk::SurfaceEnd(context, *depthSurface, cmd);
            vk::SurfaceShaderRead(context, *depthSurface, cmd);
            const VkRect2D full{ { 0, 0 }, { copyWidth, copyHeight } };
            if (!vk::ResolveImage(context, orientationResolver, cmd, depthSurface->color, orientationImage, copyWidth, copyHeight,
                    full, false, VK_IMAGE_LAYOUT_UNDEFINED, false, 1.0f, VK_FORMAT_R16G16B16A16_SFLOAT))
                return;
            if (!vk::ResolveImage(context, orientationResolver, cmd, depthSurface->color, orientationImage, copyWidth, copyHeight,
                    full, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 1.5f, VK_FORMAT_R16G16B16A16_SFLOAT))
                return;
            const VkFramebuffer first = orientationImage.resolveFramebuffer;
            if (!vk::ResolveImage(context, orientationResolver, cmd, depthSurface->color, orientationImage, copyWidth, copyHeight,
                    full, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 1.5f, VK_FORMAT_R16G16B16A16_SFLOAT))
                return;
            framebufferReused = first != VK_NULL_HANDLE && first == orientationImage.resolveFramebuffer;
            VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            read.image = orientationImage.image;
            read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                1, &read);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { copyWidth, copyHeight, 1 };
            vkCmdCopyImageToBuffer(cmd, orientationImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, orientationCopy, 1, &region);
        });
    auto half = [](uint16_t h) {
        const uint32_t sign = uint32_t(h & 0x8000u) << 16;
        const uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF;
        const uint32_t bits = exp == 0 ? sign : exp == 31 ? sign | 0x7F800000u | (man << 13) : sign | ((exp + 127 - 15) << 23) | (man << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    float topRed = 0, boundaryRed = 0, bottomBlue = 0;
    if (orientationOk)
    {
        const uint16_t* top = static_cast<const uint16_t*>(orientationReadback) + copyWidth * 4;
        const uint16_t* boundary = static_cast<const uint16_t*>(orientationReadback) + copyWidth * 16 * 4;
        const uint16_t* bottom = static_cast<const uint16_t*>(orientationReadback) + (size_t(copyHeight - 1) * copyWidth) * 4;
        topRed = half(top[0]);
        boundaryRed = half(boundary[0]);
        bottomBlue = half(bottom[2]);
    }
    printf("vk: resolve orientation and texel crop: top red %.2f, boundary red %.2f, bottom blue %.2f; framebuffer reused %d\n",
        topRed, boundaryRed, bottomBlue, int(framebufferReused));
    failures += !orientationOk || !framebufferReused || topRed < 0.9f || boundaryRed < 0.9f || bottomBlue < 0.9f;
    vk::ResolveFrameComplete(context, orientationResolver);
    vkDeviceWaitIdle(context.device);
    vkDestroyBuffer(context.device, orientationCopy, nullptr);
    vk::DestroyImage(context, orientationImage);
    vk::ResolveShutdown(context, orientationResolver);

    vk::Resolve conversionResolver;
    vk::Image conversionImage = vk::CreateImage(context, copyWidth, copyHeight, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void* conversionReadback = nullptr;
    VkBuffer conversionCopy = vk::CreateBuffer(context, VkDeviceSize(copyWidth) * copyHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        &conversionReadback);
    const bool conversionOk = vk::ResolveInit(context, conversionResolver, {}) && conversionImage.image && conversionCopy &&
        vk::Submit(context, [&](VkCommandBuffer cmd) {
            const VkRect2D full{ { 0, 0 }, { copyWidth, copyHeight } };
            if (!vk::ResolveImage(context, conversionResolver, cmd, depthSurface->color, conversionImage, copyWidth, copyHeight,
                    full, false, VK_IMAGE_LAYOUT_UNDEFINED, false, 1.0f, VK_FORMAT_R8G8B8A8_UNORM))
                return;
            VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            read.image = conversionImage.image;
            read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                1, &read);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { copyWidth, copyHeight, 1 };
            vkCmdCopyImageToBuffer(cmd, conversionImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, conversionCopy, 1, &region);
        });
    const uint8_t* conversionTop = static_cast<const uint8_t*>(conversionReadback) + (4 * copyWidth + 4) * 4;
    const uint8_t* conversionBottom = static_cast<const uint8_t*>(conversionReadback) + ((copyHeight - 1) * copyWidth + 4) * 4;
    const bool conversionCorrect = conversionOk && conversionTop[0] == 255 && conversionTop[2] == 0 && conversionBottom[0] == 0 &&
        conversionBottom[2] == 255;
    printf("vk: RGBA8 conversion: top %u %u %u, bottom %u %u %u\n", conversionTop[0], conversionTop[1], conversionTop[2],
        conversionBottom[0], conversionBottom[1], conversionBottom[2]);
    failures += !conversionCorrect;
    vk::ResolveFrameComplete(context, conversionResolver);
    vkDeviceWaitIdle(context.device);
    vkDestroyBuffer(context.device, conversionCopy, nullptr);
    vk::DestroyImage(context, conversionImage);
    vk::ResolveShutdown(context, conversionResolver);

    const uint8_t gammaInput[7] = { 98, 132, 159, 178, 189, 199, 227 };
    const uint8_t gammaExpected[7] = { 83, 119, 148, 169, 181, 192, 223 };
    uint8_t gammaPixels[7][4];
    for (uint32_t i = 0; i < 7; i++)
    {
        gammaPixels[i][0] = gammaPixels[i][1] = gammaPixels[i][2] = gammaInput[i];
        gammaPixels[i][3] = 255;
    }
    const float gammaPoints[17] = { 0.0f, 0x1.7e5f98p-6f, 0x1.06c1b0p-4f, 0x1.00c030p-3f, 0x1.806018p-3f,
        0x1.020080p-2f, 0x1.45715cp-2f, 0x1.88a228p-2f, 0x1.cc731cp-2f, 0x1.08a228p-1f, 0x1.2b0ac2p-1f,
        0x1.4e739cp-1f, 0x1.715c58p-1f, 0x1.94c532p-1f, 0x1.b82e0cp-1f, 0x1.db9ee8p-1f, 1.0f };
    vk::TextureCache gammaTextures;
    if (!vk::TextureCacheInit(context, gammaTextures, 1))
        return 1;
    const uint32_t gammaIndex = vk::TextureAdd(context, gammaTextures, 7, 1, gammaPixels);
    vk::Resolve gammaResolver;
    vk::Image gammaImage = vk::CreateImage(context, 7, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void* gammaReadback = nullptr;
    VkBuffer gammaCopy = vk::CreateBuffer(context, 28, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &gammaReadback);
    const bool gammaOk = gammaIndex != UINT32_MAX && gammaImage.image && gammaCopy && vk::ResolveInit(context, gammaResolver, {}) &&
        vk::Submit(context, [&](VkCommandBuffer cmd) {
            const VkRect2D full{ { 0, 0 }, { 7, 1 } };
            if (!vk::ResolveImage(context, gammaResolver, cmd, gammaTextures.slots[gammaIndex], gammaImage, 7, 1, full,
                    false, VK_IMAGE_LAYOUT_UNDEFINED, false, 1.0f, VK_FORMAT_R8G8B8A8_UNORM, gammaPoints))
                return;
            VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            read.image = gammaImage.image;
            read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                1, &read);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { 7, 1, 1 };
            vkCmdCopyImageToBuffer(cmd, gammaImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gammaCopy, 1, &region);
        });
    printf("vk: gamma resolve:");
    for (uint32_t i = 0; i < 7; i++)
        printf(" %u", gammaOk ? uint32_t(static_cast<const uint8_t*>(gammaReadback)[i * 4]) : 0);
    printf(" (want 83 119 148 169 181 192 223)\n");
    uint32_t gammaDiffer = 0;
    if (gammaOk)
        for (uint32_t i = 0; i < 7; i++)
            gammaDiffer += static_cast<const uint8_t*>(gammaReadback)[i * 4] != gammaExpected[i];
    failures += !gammaOk || gammaDiffer != 0;
    vk::ResolveFrameComplete(context, gammaResolver);
    vkDeviceWaitIdle(context.device);
    vkDestroyBuffer(context.device, gammaCopy, nullptr);
    vk::DestroyImage(context, gammaImage);
    vk::ResolveShutdown(context, gammaResolver);
    vk::TextureCacheShutdown(context, gammaTextures);

    // The per-draw surface cache re-validates (base, size, samples, depth) on every call. Guard the rules it
    // relies on, using a base of its own so nothing above can interfere:
    //   - the same request twice returns the same object,
    //   - a size change rebuilds it (read the width IMMEDIATELY: a per-base surface is a singleton, so a later acquire
    //     mutates the object an earlier pointer refers to - the first version of this test compared it with itself),
    //   - asking for the original size again restores it.
    {
        const uint32_t testBase = 0xD0001000;
        vk::Surface* first = vk::SurfaceAcquire(context, surfaces, testBase, 64, 32, VK_SAMPLE_COUNT_1_BIT, 0x11);
        vk::Surface* again = vk::SurfaceAcquire(context, surfaces, testBase, 64, 32, VK_SAMPLE_COUNT_1_BIT, 0x11);
        const bool sameObject = first && again && first == again && again->width == 64;
        vk::Surface* small = vk::SurfaceAcquire(context, surfaces, testBase, 32, 16, VK_SAMPLE_COUNT_1_BIT, 0x11);
        const uint32_t smallWidth = small ? small->width : 0;
        vk::Surface* back = vk::SurfaceAcquire(context, surfaces, testBase, 64, 32, VK_SAMPLE_COUNT_1_BIT, 0x11);
        const uint32_t backWidth = back ? back->width : 0;
        const bool cacheOk = sameObject && smallWidth == 32 && backWidth == 64;
        // The cache key includes depthBase, and the depth image comes from cache.depths[depthBase]. Two different
        // depth bases must therefore reach two DIFFERENT DepthSurface objects, and going back must reach the first
        // again - the part of the cache that earns its 12 ms but had no test (backlog item 1).
        vk::Surface* dA = vk::SurfaceAcquire(context, surfaces, testBase, 64, 32, VK_SAMPLE_COUNT_1_BIT, 0x21);
        void* depthA = dA ? static_cast<void*>(dA->depthState) : nullptr;
        vk::Surface* dB = vk::SurfaceAcquire(context, surfaces, testBase, 64, 32, VK_SAMPLE_COUNT_1_BIT, 0x22);
        void* depthB = dB ? static_cast<void*>(dB->depthState) : nullptr;
        vk::Surface* dA2 = vk::SurfaceAcquire(context, surfaces, testBase, 64, 32, VK_SAMPLE_COUNT_1_BIT, 0x21);
        void* depthA2 = dA2 ? static_cast<void*>(dA2->depthState) : nullptr;
        const bool depthOk = depthA && depthB && depthA2 && depthA != depthB && depthA == depthA2;
        if (!depthOk)
            printf("surface depth key: FAIL (A=%p B=%p A-again=%p)\n", depthA, depthB, depthA2);
        else
            printf("surface depth key: ok (different depth bases get different depth surfaces)\n");
        failures += !depthOk;

        if (!cacheOk)
            printf("surface cache: FAIL (same=%d small=%u back=%u)\n", int(sameObject), smallWidth, backWidth);
        else
            printf("surface cache: ok (repeat returns the same surface, size change rebuilds and restores)\n");
        failures += !cacheOk;
    }

    vk::SurfaceCacheDestroy(context, surfaces);
    vk::Shutdown(context);
    printf("vk: %s\n", failures ? "FAILED" : "ok");
    return failures ? 2 : 0;
}
