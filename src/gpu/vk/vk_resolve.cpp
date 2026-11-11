#include "gpu/vk/vk_resolve.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "gpu/vk/vk_shader.h"

namespace vk
{
namespace
{
const char kResolveVs[] = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(p * 2 - 1, 0, 1);
    o.uv = float4(p.x, p.y, 0, 1);
    return o;
}
)";

const char kResolvePs[] = R"(
Texture2D<float4> src : register(t0);
SamplerState sPoint : register(s0);
// sourceScale: source texels per destination pixel (the render scale: 1.5 at 1920x1080). The source surface is the
// host-size target while the destination is the guest-size texture, so the fetch position must be scaled - a plain
// Load at the destination position copied the top-left 1280x720 of a larger surface. nearest != 0 for depth.
struct ResolveConstants { float scale; float sourceScaleX; float sourceScaleY; float nearest; };
[[vk::push_constant]] ResolveConstants resolveConstants;
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};
float4 ResolveFetch(float2 pos)
{
    const float2 ratio = float2(resolveConstants.sourceScaleX, resolveConstants.sourceScaleY);
    if (ratio.x == 1.0 && ratio.y == 1.0)
        return src.Load(int3(int2(pos), 0)); // unscaled: exactly the texel under the pixel, as before
    const float2 centre = pos * ratio;
    if (resolveConstants.nearest != 0.0)
        return src.Load(int3(int2(centre), 0));
    const float2 d = ratio * 0.25; // a 2x2 box over the destination pixel's footprint
    return 0.25 * (src.Load(int3(int2(centre + float2(-d.x, -d.y)), 0)) + src.Load(int3(int2(centre + float2(d.x, -d.y)), 0)) +
        src.Load(int3(int2(centre + float2(-d.x, d.y)), 0)) + src.Load(int3(int2(centre + float2(d.x, d.y)), 0)));
}
float4 main(VSOut input) : SV_Target0
{
    return ResolveFetch(input.pos.xy) * resolveConstants.scale;
}
)";

const char kSwapPs[] = R"(
Texture2D<float4> src : register(t0);
SamplerState sPoint : register(s0);
// sourceScale: source texels per destination pixel (the render scale: 1.5 at 1920x1080). The source surface is the
// host-size target while the destination is the guest-size texture, so the fetch position must be scaled - a plain
// Load at the destination position copied the top-left 1280x720 of a larger surface. nearest != 0 for depth.
struct ResolveConstants { float scale; float sourceScaleX; float sourceScaleY; float nearest; };
[[vk::push_constant]] ResolveConstants resolveConstants;
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};
float4 ResolveFetch(float2 pos)
{
    const float2 ratio = float2(resolveConstants.sourceScaleX, resolveConstants.sourceScaleY);
    if (ratio.x == 1.0 && ratio.y == 1.0)
        return src.Load(int3(int2(pos), 0)); // unscaled: exactly the texel under the pixel, as before
    const float2 centre = pos * ratio;
    if (resolveConstants.nearest != 0.0)
        return src.Load(int3(int2(centre), 0));
    const float2 d = ratio * 0.25; // a 2x2 box over the destination pixel's footprint
    return 0.25 * (src.Load(int3(int2(centre + float2(-d.x, -d.y)), 0)) + src.Load(int3(int2(centre + float2(d.x, -d.y)), 0)) +
        src.Load(int3(int2(centre + float2(-d.x, d.y)), 0)) + src.Load(int3(int2(centre + float2(d.x, d.y)), 0)));
}
float4 main(VSOut input) : SV_Target0
{
    float4 c = ResolveFetch(input.pos.xy);
    return float4(c.b, c.g, c.r, c.a) * resolveConstants.scale; // the copy's swap bit: red and blue exchanged
}
)";

const char kGammaPs[] = R"(
Texture2D<float4> src : register(t0);
SamplerState sPoint : register(s0);
// 17 named floats instead of `float points[17]`: RADV mis-reads the array form of this push
// constant (the APU returned 83 86 97 84 95 89 85 where the 5090 returns the correct ramp), and
// named members remove any array-stride assumption from the shader compiler. Layout is unchanged:
// 17 contiguous floats at offset 0, so the host side and the 68-byte push size stay valid.
struct GammaConstants
{
    float p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16;
};
[[vk::push_constant]] GammaConstants gamma;
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};
float GammaPoint(uint i)
{
    float v[17] = { gamma.p0, gamma.p1, gamma.p2, gamma.p3, gamma.p4, gamma.p5, gamma.p6, gamma.p7,
        gamma.p8, gamma.p9, gamma.p10, gamma.p11, gamma.p12, gamma.p13, gamma.p14, gamma.p15, gamma.p16 };
    return v[min(i, 16u)];
}
float GammaCurve(float x)
{
    const float f = saturate(x) * 16.0;
    const uint i = min(uint(f), 15u);
    return lerp(GammaPoint(i), GammaPoint(i + 1u), f - float(i));
}
float4 main(VSOut input) : SV_Target0
{
    const float4 c = src.Load(int3(int2(input.pos.xy), 0));
    return float4(GammaCurve(c.r), GammaCurve(c.g), GammaCurve(c.b), c.a);
}
)";

// The frame as the window gets it: the front buffer (any size, RGBA8 or half-float) stretched to the output size with
// the display gamma curve applied, written as RGBA8 so the readback needs no per-pixel conversion on the CPU.
// 17 curve points, then useGamma (0 = identity) - same push-constant block size as kGammaPs plus one float.
const char kPresentPs[] = R"(
Texture2D<float4> src : register(t0);
SamplerState sLinear : register(s0);
struct PresentConstants
{
    float p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16;
    float useGamma;
};
[[vk::push_constant]] PresentConstants present;
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};
float PresentPoint(uint i)
{
    float v[17] = { present.p0, present.p1, present.p2, present.p3, present.p4, present.p5, present.p6, present.p7,
        present.p8, present.p9, present.p10, present.p11, present.p12, present.p13, present.p14, present.p15, present.p16 };
    return v[min(i, 16u)];
}
float PresentCurve(float x)
{
    const float f = saturate(x) * 16.0;
    const uint i = min(uint(f), 15u);
    return lerp(PresentPoint(i), PresentPoint(i + 1u), f - float(i));
}
float4 main(VSOut input) : SV_Target0
{
    float3 c = saturate(src.SampleLevel(sLinear, input.uv.xy, 0).rgb);
    if (present.useGamma != 0.0)
    {
        // The CPU path quantised to 8 bits before the curve (gamma->bytes[HalfToByte]); match it so captures compare.
        c = floor(c * 255.0 + 0.5) / 255.0;
        c = float3(PresentCurve(c.r), PresentCurve(c.g), PresentCurve(c.b));
    }
    return float4(c, 1.0);
}
)";

VkShaderModule Module(const Context& context, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(context.device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

VkPipeline MakePipeline(const Context& context, VkShaderModule vs, VkShaderModule ps, VkPipelineLayout layout, VkRenderPass pass)
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
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = layout;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(context.device, context.pipelineCache, 1, &info, nullptr, &pipeline) == VK_SUCCESS ? pipeline
        : VK_NULL_HANDLE;
}
}

bool ResolveInit(Context& context, Resolve& resolve, const std::filesystem::path& cacheDir)
{
    std::vector<uint32_t> vsCode, psCode, swapCode, gammaCode, presentCode;
    std::string error;
    if (!CompileHlsl(kResolveVs, false, HashSource(kResolveVs), cacheDir, vsCode, &error) ||
        !CompileHlsl(kResolvePs, true, HashSource(kResolvePs), cacheDir, psCode, &error) ||
        !CompileHlsl(kSwapPs, true, HashSource(kSwapPs), cacheDir, swapCode, &error) ||
        !CompileHlsl(kGammaPs, true, HashSource(kGammaPs), cacheDir, gammaCode, &error) ||
        !CompileHlsl(kPresentPs, true, HashSource(kPresentPs), cacheDir, presentCode, &error))
    {
        fprintf(stderr, "vk: resolve shaders: %s\n", error.c_str());
        return false;
    }
    VkShaderModule vs = Module(context, vsCode), ps = Module(context, psCode), swap = Module(context, swapCode);
    VkShaderModule gamma = Module(context, gammaCode), present = Module(context, presentCode);
    if (!vs || !ps || !swap || !gamma || !present)
        return false;

    // dxc's register mapping: t0 is binding 100, s0 is binding 200.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 100;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 200;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(context.device, &layoutInfo, nullptr, &resolve.setLayout) != VK_SUCCESS)
        return false;
    VkPushConstantRange pushRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 18 };
    VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &resolve.setLayout;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(context.device, &pipelineLayout, nullptr, &resolve.layout) != VK_SUCCESS)
        return false;

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // the barrier after moves it on
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &color;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(context.device, &passInfo, nullptr, &resolve.pass) != VK_SUCCESS)
        return false;
    color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (vkCreateRenderPass(context.device, &passInfo, nullptr, &resolve.hdrPass) != VK_SUCCESS)
        return false;
    color.format = VK_FORMAT_R32_SFLOAT;
    if (vkCreateRenderPass(context.device, &passInfo, nullptr, &resolve.depthPass) != VK_SUCCESS)
        return false;
    resolve.copy = MakePipeline(context, vs, ps, resolve.layout, resolve.pass);
    resolve.swap = MakePipeline(context, vs, swap, resolve.layout, resolve.pass);
    resolve.gamma = MakePipeline(context, vs, gamma, resolve.layout, resolve.pass);
    resolve.present = MakePipeline(context, vs, present, resolve.layout, resolve.pass);
    resolve.hdrCopy = MakePipeline(context, vs, ps, resolve.layout, resolve.hdrPass);
    resolve.hdrSwap = MakePipeline(context, vs, swap, resolve.layout, resolve.hdrPass);
    resolve.depthCopy = MakePipeline(context, vs, ps, resolve.layout, resolve.depthPass);
    vkDestroyShaderModule(context.device, vs, nullptr);
    vkDestroyShaderModule(context.device, ps, nullptr);
    vkDestroyShaderModule(context.device, swap, nullptr);
    vkDestroyShaderModule(context.device, gamma, nullptr);
    vkDestroyShaderModule(context.device, present, nullptr);
    if (!resolve.copy || !resolve.swap || !resolve.gamma || !resolve.present || !resolve.hdrCopy || !resolve.hdrSwap || !resolve.depthCopy)
        return false;

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(context.device, &samplerInfo, nullptr, &resolve.sampler) != VK_SUCCESS)
        return false;
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(context.device, &samplerInfo, nullptr, &resolve.linearSampler) != VK_SUCCESS)
        return false;

    // FREE_DESCRIPTOR_SET_BIT: ResolveFrameComplete returns each frame's sets with vkFreeDescriptorSets; without the
    // bit those calls reclaim nothing and the pool drains. maxSets fits a frame's worth of resolves plus the
    // one-frame async lag.
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 8192 } };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4096;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &resolve.pool) != VK_SUCCESS)
        return false;
    return true;
}

void ResolveFrameComplete(Context& context, Resolve& resolve, size_t framebufferCount, size_t viewCount, size_t setCount)
{
    auto complete = [&](auto& values, size_t count, auto destroy) {
        if (count > values.size())
            count = values.size();
        for (size_t i = 0; i < count; i++)
            destroy(values[i]);
        values.erase(values.begin(), values.begin() + count);
    };
    complete(resolve.pendingFramebuffers, framebufferCount, [&](VkFramebuffer value) { vkDestroyFramebuffer(context.device, value, nullptr); });
    complete(resolve.pendingViews, viewCount, [&](VkImageView value) { vkDestroyImageView(context.device, value, nullptr); });
    complete(resolve.pendingSets, setCount, [&](VkDescriptorSet value) {
        if (value)
            vkFreeDescriptorSets(context.device, resolve.pool, 1, &value);
    });
}

void ResolveShutdown(Context& context, Resolve& resolve)
{
    ResolveFrameComplete(context, resolve);
    if (resolve.pool)
        vkDestroyDescriptorPool(context.device, resolve.pool, nullptr);
    if (resolve.sampler)
        vkDestroySampler(context.device, resolve.sampler, nullptr);
    if (resolve.linearSampler)
        vkDestroySampler(context.device, resolve.linearSampler, nullptr);
    if (resolve.present)
        vkDestroyPipeline(context.device, resolve.present, nullptr);
    if (resolve.copy)
        vkDestroyPipeline(context.device, resolve.copy, nullptr);
    if (resolve.swap)
        vkDestroyPipeline(context.device, resolve.swap, nullptr);
    if (resolve.hdrCopy)
        vkDestroyPipeline(context.device, resolve.hdrCopy, nullptr);
    if (resolve.hdrSwap)
        vkDestroyPipeline(context.device, resolve.hdrSwap, nullptr);
    if (resolve.depthCopy)
        vkDestroyPipeline(context.device, resolve.depthCopy, nullptr);
    if (resolve.gamma)
        vkDestroyPipeline(context.device, resolve.gamma, nullptr);
    if (resolve.pass)
        vkDestroyRenderPass(context.device, resolve.pass, nullptr);
    if (resolve.hdrPass)
        vkDestroyRenderPass(context.device, resolve.hdrPass, nullptr);
    if (resolve.depthPass)
        vkDestroyRenderPass(context.device, resolve.depthPass, nullptr);
    if (resolve.layout)
        vkDestroyPipelineLayout(context.device, resolve.layout, nullptr);
    if (resolve.setLayout)
        vkDestroyDescriptorSetLayout(context.device, resolve.setLayout, nullptr);
    resolve = {};
}

bool ResolveImage(const Context& context, Resolve& resolve, VkCommandBuffer cmd, Image& source, Image& destination,
    uint32_t width, uint32_t height, const VkRect2D& rect, bool swapRedBlue, VkImageLayout destinationLayout, bool depth, float scale,
    VkFormat destinationFormat, const float* gamma, float sourceScaleX, float sourceScaleY)
{
    // Same SIZE as well as same format: a copy cannot scale. With a render resolution above 1280x720 the surface is
    // larger than the guest-size resolve target, and copying `rect` 1:1 cropped the top-left of the scene into it -
    // the magnified band on a black frame at 1920x1080.
    const bool directCopy = !depth && !gamma && !swapRedBlue && scale == 1.0f && source.format == destinationFormat &&
        destination.format == destinationFormat && source.width == destination.width && source.height == destination.height &&
        destination.width == width && destination.height == height && sourceScaleX == 1.0f && sourceScaleY == 1.0f;
    if (directCopy)
    {
        VkImageMemoryBarrier before[2]{};
        for (VkImageMemoryBarrier& barrier : before)
        {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        before[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        before[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        before[0].image = source.image;
        before[1].srcAccessMask = destinationLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
        before[1].oldLayout = destinationLayout;
        before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before[1].image = destination.image;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.srcOffset = region.dstOffset = { rect.offset.x, rect.offset.y, 0 };
        region.extent = { rect.extent.width, rect.extent.height, 1 };
        vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &region);
        VkImageMemoryBarrier after[2]{};
        for (uint32_t i = 0; i < 2; i++)
        {
            after[i] = before[i];
            after[i].srcAccessMask = i == 0 ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
            after[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            after[i].oldLayout = before[i].newLayout;
            after[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, after);
        return true;
    }
    const bool hdr = !depth && destinationFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool graded = gamma && !depth && destinationFormat == VK_FORMAT_R8G8B8A8_UNORM;
    VkRenderPass pass = depth ? resolve.depthPass : hdr ? resolve.hdrPass : resolve.pass;
    VkPipeline pipeline = graded ? resolve.gamma
        : depth ? resolve.depthCopy : hdr ? (swapRedBlue ? resolve.hdrSwap : resolve.hdrCopy) : (swapRedBlue ? resolve.swap : resolve.copy);
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = resolve.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &resolve.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(context.device, &allocate, &set) != VK_SUCCESS)
    {
        fprintf(stderr, "vk: resolve failed: descriptor set allocation\n");
        return false;
    }
    resolve.pendingSets.push_back(set);
    VkDescriptorImageInfo image{ VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo sampler{ resolve.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    writes[0].dstSet = set;
    writes[0].dstBinding = 100;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &image;
    writes[1] = writes[0];
    writes[1].dstBinding = 200;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &sampler;
    vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);

    if (!destination.resolveFramebuffer)
    {
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &destination.view;
        framebufferInfo.width = width;
        framebufferInfo.height = height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(context.device, &framebufferInfo, nullptr, &destination.resolveFramebuffer) != VK_SUCCESS)
        {
            fprintf(stderr, "vk: resolve failed: framebuffer (format %d %ux%u)\n", int(destinationFormat), width, height);
            return false;
        }
    }

    VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toColor.srcAccessMask = destinationLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = destinationLayout;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = destination.image;
    toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, destinationLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColor);

    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = pass;
    begin.framebuffer = destination.resolveFramebuffer;
    begin.renderArea = rect;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve.layout, 0, 1, &set, 0, nullptr);
    if (graded)
        vkCmdPushConstants(cmd, resolve.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 17, gamma);
    else
    {
        const float constants[4] = { scale, sourceScaleX, sourceScaleY, depth ? 1.0f : 0.0f };
        vkCmdPushConstants(cmd, resolve.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, constants);
    }
    VkViewport viewport{ 0, 0, float(width), float(height), 0, 1 };
    VkRect2D scissor = rect;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    VkImageMemoryBarrier toSampled{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // the pass's finalLayout
    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSampled.srcQueueFamilyIndex = toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSampled.image = destination.image;
    toSampled.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toSampled.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &toSampled);

    return true;
}

bool PresentDraw(const Context& context, Resolve& resolve, VkCommandBuffer cmd, VkPipeline pipeline, VkImageView source,
    const VkRect2D& area, const VkRect2D& scissor, const float* gamma)
{
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = resolve.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &resolve.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(context.device, &allocate, &set) != VK_SUCCESS)
        return false;
    resolve.pendingSets.push_back(set);
    VkDescriptorImageInfo image{ VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo sampler{ resolve.linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    writes[0].dstSet = set;
    writes[0].dstBinding = 100;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &image;
    writes[1] = writes[0];
    writes[1].dstBinding = 200;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &sampler;
    vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve.layout, 0, 1, &set, 0, nullptr);
    float constants[18]{};
    if (gamma)
    {
        for (uint32_t i = 0; i < 17; i++)
            constants[i] = gamma[i];
        constants[17] = 1.0f;
    }
    vkCmdPushConstants(cmd, resolve.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, constants);
    const VkViewport viewport{ float(area.offset.x), float(area.offset.y), float(area.extent.width), float(area.extent.height), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    return true;
}

VkPipeline ResolveMakePresentPipeline(const Context& context, Resolve& resolve, VkRenderPass pass, const std::filesystem::path& cacheDir)
{
    // The same two shaders as resolve.present, for a render pass of another format (the swap chain's). Both come out
    // of the disk cache ResolveInit filled.
    std::vector<uint32_t> vsCode, psCode;
    std::string error;
    if (!CompileHlsl(kResolveVs, false, HashSource(kResolveVs), cacheDir, vsCode, &error) ||
        !CompileHlsl(kPresentPs, true, HashSource(kPresentPs), cacheDir, psCode, &error))
        return VK_NULL_HANDLE;
    const VkShaderModule vs = Module(context, vsCode), ps = Module(context, psCode);
    const VkPipeline pipeline = vs && ps ? MakePipeline(context, vs, ps, resolve.layout, pass) : VK_NULL_HANDLE;
    if (vs)
        vkDestroyShaderModule(context.device, vs, nullptr);
    if (ps)
        vkDestroyShaderModule(context.device, ps, nullptr);
    return pipeline;
}

bool PresentImage(const Context& context, Resolve& resolve, VkCommandBuffer cmd, Image& source, Image& destination,
    VkImageLayout destinationLayout, const float* gamma)
{
    if (!destination.resolveFramebuffer)
    {
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = resolve.pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &destination.view;
        framebufferInfo.width = destination.width;
        framebufferInfo.height = destination.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(context.device, &framebufferInfo, nullptr, &destination.resolveFramebuffer) != VK_SUCCESS)
            return false;
    }
    VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toColor.srcAccessMask = destinationLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // every pixel is rewritten
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = destination.image;
    toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, destinationLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColor);
    const VkRect2D full{ { 0, 0 }, { destination.width, destination.height } };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = resolve.pass;
    begin.framebuffer = destination.resolveFramebuffer;
    begin.renderArea = full;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const bool drawn = PresentDraw(context, resolve, cmd, resolve.present, source.view, full, full, gamma);
    vkCmdEndRenderPass(cmd); // leaves the image in TRANSFER_SRC (the pass's finalLayout), ready for the readback copy
    return drawn;
}
}
