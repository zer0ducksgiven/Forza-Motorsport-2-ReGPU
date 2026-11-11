#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_blit.h"

#include <cstdio>
#include <cstring>

#include "gpu/vk/vk_shader.h"

namespace vk
{
namespace
{
// A full-screen triangle, no vertex buffer: the Vulkan backend never binds vertex input, the
// game's geometry is pulled out of guest memory in the vertex shader.
const char kBlitVs[] = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);   // 0,0  2,0  0,2
    VSOut o;
    o.pos = float4(p * 2 - 1, 0, 1);
    o.uv = float4(p.x, p.y, 0, 1); // frame rows are top-down, like the source image
    return o;
}
)";

// Nearest-neighbour: the boot screen is already at the output size.
const char kBlitPs[] = R"(
Texture2D<float4> src : register(t0);
SamplerState sPoint : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target0
{
    return src.Sample(sPoint, input.uv.xy);
}
)";

bool Fail(const char* what, VkResult result)
{
    fprintf(stderr, "vk: %s failed (%d)\n", what, int(result));
    return false;
}

VkShaderModule MakeModule(const Context& context, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(context.device, &ci, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}
}

bool BlitInit(Context& context, Blit& blit, const std::filesystem::path& cacheDir)
{
    std::vector<uint32_t> vsCode, psCode;
    std::string error;
    if (!CompileHlsl(kBlitVs, false, HashSource(kBlitVs), cacheDir, vsCode, &error))
    {
        fprintf(stderr, "vk: blit vertex shader: %s\n", error.c_str());
        return false;
    }
    if (!CompileHlsl(kBlitPs, true, HashSource(kBlitPs), cacheDir, psCode, &error))
    {
        fprintf(stderr, "vk: blit pixel shader: %s\n", error.c_str());
        return false;
    }
    blit.vs = MakeModule(context, vsCode);
    blit.ps = MakeModule(context, psCode);
    if (!blit.vs || !blit.ps)
        return false;

    // dxc's register mapping: t0 is binding 100, s0 is binding 200 (see vk_shader.cpp), and a
    // Texture2D with a separate SamplerState stays two descriptors.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 100;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 200;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    sli.bindingCount = 2;
    sli.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(context.device, &sli, nullptr, &blit.setLayout) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &blit.setLayout;
    if (vkCreatePipelineLayout(context.device, &pli, nullptr, &blit.layout) != VK_SUCCESS)
        return false;

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1;
    rpi.pAttachments = &color;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &subpass;
    if (vkCreateRenderPass(context.device, &rpi, nullptr, &blit.pass) != VK_SUCCESS)
        return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = blit.vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = blit.ps;
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
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
    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &assembly;
    pci.pViewportState = &viewport;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &multisample;
    pci.pColorBlendState = &colorBlend;
    pci.pDynamicState = &dynamicState;
    pci.layout = blit.layout;
    pci.renderPass = blit.pass;
    if (vkCreateGraphicsPipelines(context.device, context.pipelineCache, 1, &pci, nullptr, &blit.pipeline) != VK_SUCCESS)
        return Fail("vkCreateGraphicsPipelines", VK_ERROR_INITIALIZATION_FAILED);

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0;
    if (vkCreateSampler(context.device, &sci, nullptr, &blit.sampler) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 8 } };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 8;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    return vkCreateDescriptorPool(context.device, &dpi, nullptr, &blit.pool) == VK_SUCCESS;
}

void BlitShutdown(Context& context, Blit& blit)
{
    if (blit.pool)
        vkDestroyDescriptorPool(context.device, blit.pool, nullptr);
    if (blit.sampler)
        vkDestroySampler(context.device, blit.sampler, nullptr);
    if (blit.pipeline)
        vkDestroyPipeline(context.device, blit.pipeline, nullptr);
    if (blit.pass)
        vkDestroyRenderPass(context.device, blit.pass, nullptr);
    if (blit.layout)
        vkDestroyPipelineLayout(context.device, blit.layout, nullptr);
    if (blit.setLayout)
        vkDestroyDescriptorSetLayout(context.device, blit.setLayout, nullptr);
    if (blit.ps)
        vkDestroyShaderModule(context.device, blit.ps, nullptr);
    if (blit.vs)
        vkDestroyShaderModule(context.device, blit.vs, nullptr);
    blit = {};
}

bool BlitImage(Context& context, Blit& blit, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
    std::vector<uint8_t>& out)
{
    if (rgba.size() < size_t(width) * height * 4)
        return false;
    Image source = CreateImage(context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Image target = CreateImage(context, width, height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!source.image || !target.image)
        return false;
    UploadImage(context, source, rgba.data(), VkDeviceSize(width) * height * 4);

    VkImageViewCreateInfo tvi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkFramebufferCreateInfo fbi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = blit.pass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &target.view;
    fbi.width = width;
    fbi.height = height;
    fbi.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(context.device, &fbi, nullptr, &framebuffer) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = blit.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &blit.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(context.device, &ai, &set) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo imageInfo{ VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo samplerInfo{ blit.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    writes[0].dstSet = set;
    writes[0].dstBinding = 100;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &imageInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 200;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &samplerInfo;
    vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);

    void* readback = nullptr;
    VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    VkBuffer copy = CreateBuffer(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
    if (!copy)
        return false;

    const bool ok = Submit(context, [&](VkCommandBuffer cb) {
        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = blit.pass;
        rp.framebuffer = framebuffer;
        rp.renderArea = { { 0, 0 }, { width, height } };
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blit.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blit.layout, 0, 1, &set, 0, nullptr);
        VkViewport viewport{ 0, 0, float(width), float(height), 0, 1 };
        VkRect2D scissor{ { 0, 0 }, { width, height } };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, 1, &region);
    });

    if (ok)
    {
        out.resize(size_t(bytes));
        memcpy(out.data(), readback, size_t(bytes));
    }
    vkDeviceWaitIdle(context.device);
    vkDestroyBuffer(context.device, copy, nullptr);
    vkDestroyFramebuffer(context.device, framebuffer, nullptr);
    vkFreeDescriptorSets(context.device, blit.pool, 1, &set);
    DestroyImage(context, target);
    DestroyImage(context, source);
    return ok;
}
}
