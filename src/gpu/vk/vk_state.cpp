#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_state.h"

#include <cstdio>

namespace vk
{
namespace
{
// Xenos blend factors, in the order the renderer's Blend() maps (0 zero, 1 one, 4..11 the
// colour/alpha pairs, 12..15 the constant factors, 16 alpha-saturate).
VkBlendFactor Blend(uint32_t f)
{
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

// Colour factors have no meaning for the alpha channel in Vulkan either: use their alpha forms.
VkBlendFactor AlphaBlend(uint32_t f)
{
    switch (f)
    {
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: return Blend(f);
    }
}

VkBlendOp BlendOp(uint32_t op)
{
    switch (op)
    {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

// The guest stores comparison functions as 0..7 counting from NEVER; Vulkan's enum starts at 0
// for NEVER too, so the +1 the D3D12 path adds is not needed here.
VkCompareOp Compare(uint32_t f) { return VkCompareOp(f & 7); }

// Xenos stencil ops are already in Vulkan's order: keep, zero, replace, incr clamp, decr clamp,
// invert, incr wrap, decr wrap.
VkStencilOp StencilOp(uint32_t op) { return VkStencilOp(op & 7); }
}

bool CreatePipeline(const Context& context, const StateKey& key, VkShaderModule vs, VkShaderModule ps,
    VkPipelineLayout layout, VkRenderPass renderPass, const TargetFormats& targets, VkPipeline& pipeline,
    const char** error)
{
    const uint32_t blend = key.blend, depth = key.depth;
    const uint32_t mask = (key.stencilMaskMode >> 24) & 0xF, mode = (key.stencilMaskMode >> 28) & 7;
    const uint32_t stencil = key.stencilMaskMode & 0xFFFFFF;
    const bool alphaToMask = key.flags & 1, cullable = (key.flags >> 1) & 1, depthOnly = (key.flags >> 2) & 1;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    // No vertex input: the vertex shader pulls the guest's vertex data out of gphys.
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    // The renderer's BuildIndexList expands every guest primitive type (strips, fans, quads, rectangles) into
    // triangle-list indices, so the pipeline is a list whatever key.flags says the guest topology was.
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = !cullable ? VK_CULL_MODE_NONE
        : (mode & 3) == 1 ? VK_CULL_MODE_FRONT_BIT : (mode & 3) == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // The translated shaders and viewports produce the same y-down framebuffer coordinates on
    // both backends, so front-face selection must match D3D12 too.
    raster.frontFace = (mode & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.depthBiasEnable = VK_FALSE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = targets.samples;
    multisample.alphaToCoverageEnable = alphaToMask ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    // FM2_NO_ZTEST turns the depth test off on both backends.
    static const bool noDepth = getenv("FM2_NO_ZTEST") != nullptr;
    depthStencil.depthTestEnable = !noDepth && ((depth >> 1) & 1) ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = (depth >> 2) & 1 ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = Compare((depth >> 4) & 7);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = (depth & 1) ? VK_TRUE : VK_FALSE;
    if (depthStencil.stencilTestEnable)
    {
        // read/write masks come from RB_STENCILREFMASK; the reference itself is dynamic.
        depthStencil.front.compareMask = uint8_t(stencil >> 8);
        depthStencil.front.writeMask = uint8_t(stencil >> 16);
        depthStencil.back.compareMask = depthStencil.front.compareMask;
        depthStencil.back.writeMask = depthStencil.front.writeMask;
        auto face = [&](uint32_t shift, VkStencilOpState& state) {
            state.compareOp = Compare((depth >> shift) & 7);
            state.failOp = StencilOp((depth >> (shift + 3)) & 7);
            state.passOp = StencilOp((depth >> (shift + 6)) & 7);
            state.depthFailOp = StencilOp((depth >> (shift + 9)) & 7);
        };
        face(8, depthStencil.front);
        if ((depth >> 7) & 1)
            face(20, depthStencil.back);
        else
            depthStencil.back = depthStencil.front;
    }

    const uint32_t srcC = blend & 31, opC = (blend >> 5) & 7, dstC = (blend >> 8) & 31;
    const uint32_t srcA = (blend >> 16) & 31, opA = (blend >> 21) & 7, dstA = (blend >> 24) & 31;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = !(srcC == 1 && dstC == 0 && opC == 0 && srcA == 1 && dstA == 0 && opA == 0) ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = Blend(srcC);
    attachment.dstColorBlendFactor = Blend(dstC);
    attachment.colorBlendOp = BlendOp(opC);
    attachment.srcAlphaBlendFactor = AlphaBlend(srcA);
    attachment.dstAlphaBlendFactor = AlphaBlend(dstA);
    attachment.alphaBlendOp = BlendOp(opA);
    attachment.colorWriteMask = depthOnly ? 0u : mask;
    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &attachment;

    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 4;
    dynamicState.pDynamicStates = dynamic;

    VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &targets.color;
    rendering.depthAttachmentFormat = targets.depth;

    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.pNext = renderPass == VK_NULL_HANDLE ? &rendering : nullptr;
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
    info.renderPass = renderPass;
    const VkResult result = vkCreateGraphicsPipelines(context.device, context.pipelineCache, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        if (error)
            *error = "vkCreateGraphicsPipelines failed";
        fprintf(stderr, "vk: pipeline vs %016llx ps %016llx blend %08x depth %08x mask %08x -> %d\n",
            (unsigned long long)key.vs, (unsigned long long)key.ps, key.blend, key.depth, key.stencilMaskMode, int(result));
        return false;
    }
    return true;
}
}
