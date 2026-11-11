#pragma once
// Guest render state -> Vulkan pipeline. The renderer keys a pipeline on the Xenos registers
// that are baked into it (blend, depth/stencil, colour mask, culling) plus the two shaders;
// this is the Vulkan side of that same key, so the two backends produce equivalent state.
//
// The key packs the registers exactly as the D3D12 path does (see MakePipelineKey in
// renderer.cpp), which also makes a capture of the game's real keys replayable here.
#include <cstdint>
#include <vulkan/vulkan.h>

#include "gpu/vk/vk_context.h"

namespace vk
{
struct StateKey
{
    uint64_t vs = 0, ps = 0;
    uint32_t blend = 0;           // RB_BLENDCONTROL0: srcC 5 | opC 3 | dstC 5 | srcA 5 | opA 3 | dstA 5
    uint32_t depth = 0;           // RB_DEPTHCONTROL, plus the stencil ops
    uint32_t stencilMaskMode = 0; // stencil masks (24 bits) | colour write mask << 24 | cull mode << 28
    uint32_t flags = 0;           // 1 alpha-to-mask, 2 cullable, 4 motion-vector variant
    bool operator==(const StateKey&) const = default;
};

// What the draw renders into. The EDRAM surface model picks these; the defaults match the
// renderer's main pass (half-float colour, D32S8 depth).
struct TargetFormats
{
    VkFormat color = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depth = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Builds the pipeline state for a key. The shaders are modules the caller owns; the pipeline
// cache should be context.pipelineCache so repeat starts are instant.
bool CreatePipeline(const Context& context, const StateKey& key, VkShaderModule vs, VkShaderModule ps,
    VkPipelineLayout layout, VkRenderPass renderPass, const TargetFormats& targets, VkPipeline& pipeline,
    const char** error = nullptr);
}
