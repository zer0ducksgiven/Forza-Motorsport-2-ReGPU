#pragma once
// The renderer's own full-screen pass on Vulkan: take an RGBA8 image on the CPU, draw it into
// an image of the same size, read it back. This is the path the boot screen, the shader
// preparation overlay and the held frame use, and it is the first thing the native renderer
// has to get right (device, dxc, pipeline, descriptors, render pass, readback) before the PM4
// draw translation and the EDRAM surface model can be built on top of it.
#include <cstdint>
#include <filesystem>
#include <vector>

#include "gpu/vk/vk_context.h"

namespace vk
{
struct Blit
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule ps = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

bool BlitInit(Context& context, Blit& blit, const std::filesystem::path& cacheDir);
void BlitShutdown(Context& context, Blit& blit);

// Renders rgba (width x height RGBA8) into a fresh image, then reads it back into out.
bool BlitImage(Context& context, Blit& blit, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
    std::vector<uint8_t>& out);
}
