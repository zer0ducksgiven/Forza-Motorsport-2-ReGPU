#pragma once
// EDRAM resolves: the guest copies a render target into memory (RB_MODECONTROL 6), and later
// samples the result at that address. The renderer never writes guest memory, so the copy goes
// into a texture instead — sampled directly by whatever fetch asks for that address.
//
// The source is a render target (half-float colour here), the destination an RGBA8 or half-float
// image the bindless cache hands out, so this is a small full-screen pass that also applies the
// copy's channel swap and exponent scale.
#include <vulkan/vulkan.h>

#include <filesystem>
#include <vector>

#include "gpu/vk/vk_context.h"

namespace vk
{
struct Resolve
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline copy = VK_NULL_HANDLE;
    VkPipeline swap = VK_NULL_HANDLE;
    VkPipeline gamma = VK_NULL_HANDLE;
    VkPipeline present = VK_NULL_HANDLE;
    VkRenderPass hdrPass = VK_NULL_HANDLE;
    VkPipeline hdrCopy = VK_NULL_HANDLE;
    VkPipeline hdrSwap = VK_NULL_HANDLE;
    VkRenderPass depthPass = VK_NULL_HANDLE;
    VkPipeline depthCopy = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> pendingFramebuffers;
    std::vector<VkImageView> pendingViews;
    std::vector<VkDescriptorSet> pendingSets;
};

// cacheDir is the vendor-scoped shader cache; passing it keeps the resolve shaders out of a cache
// another GPU compiled. Empty disables caching for them.
bool ResolveInit(Context& context, Resolve& resolve, const std::filesystem::path& cacheDir);
void ResolveFrameComplete(Context& context, Resolve& resolve, size_t framebufferCount = SIZE_MAX, size_t viewCount = SIZE_MAX,
    size_t setCount = SIZE_MAX);
void ResolveShutdown(Context& context, Resolve& resolve);

// Copies `source` (any sampled format) into the destination rectangle, preserving its other pixels,
// and leaves it in SHADER_READ_ONLY. Recorded into the caller's command buffer, inside no render pass.
bool ResolveImage(const Context& context, Resolve& resolve, VkCommandBuffer cmd, Image& source, Image& destination,
    uint32_t width, uint32_t height, const VkRect2D& rect, bool swapRedBlue, VkImageLayout destinationLayout, bool depth = false,
    float scale = 1.0f, VkFormat destinationFormat = VK_FORMAT_R8G8B8A8_UNORM, const float* gamma = nullptr,
    float sourceScaleX = 1.0f, float sourceScaleY = 1.0f);

// The frame for the window: `source` (SHADER_READ, any size, RGBA8 or half-float) stretched over the whole of
// `destination` (an RGBA8 colour attachment) with the 17-point display gamma curve (null = none). Leaves the destination in
// TRANSFER_SRC. destinationLayout is UNDEFINED the first time and TRANSFER_SRC afterwards.
bool PresentImage(const Context& context, Resolve& resolve, VkCommandBuffer cmd, Image& source, Image& destination,
    VkImageLayout destinationLayout, const float* gamma);
// The same draw inside a render pass the caller began: `source` (a SHADER_READ view) stretched over `area`, clipped to
// `scissor`. `pipeline` is resolve.present or one from ResolveMakePresentPipeline for that pass.
bool PresentDraw(const Context& context, Resolve& resolve, VkCommandBuffer cmd, VkPipeline pipeline, VkImageView source,
    const VkRect2D& area, const VkRect2D& scissor, const float* gamma);
VkPipeline ResolveMakePresentPipeline(const Context& context, Resolve& resolve, VkRenderPass pass, const std::filesystem::path& cacheDir);
}
