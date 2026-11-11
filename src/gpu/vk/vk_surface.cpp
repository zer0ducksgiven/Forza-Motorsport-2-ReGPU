#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_surface.h"
#include <cstdio>
#include <cstdlib>

namespace vk
{
namespace
{
// Every colour target is half-float, whatever the guest's EDRAM format.
constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
    VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    if (from == to)
        return;
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    if (from == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        // Nothing happened before: no source scope (and the contents are discarded).
        srcAccess = 0;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkRenderPass MakeRenderPass(const Context& context, VkSampleCountFlagBits samples)
{
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = kColorFormat;
    attachments[0].samples = samples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // resolves can close and reopen a pass
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = kDepthFormat;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    return vkCreateRenderPass(context.device, &info, nullptr, &pass) == VK_SUCCESS ? pass : VK_NULL_HANDLE;
}
}

void SurfaceCacheDestroy(Context& context, SurfaceCache& cache)
{
    for (auto& [base, surface] : cache.surfaces)
    {
        if (surface.framebuffer)
            vkDestroyFramebuffer(context.device, surface.framebuffer, nullptr);
        if (surface.pass)
            vkDestroyRenderPass(context.device, surface.pass, nullptr);
        DestroyImage(context, surface.color);
    }
    for (auto& [base, depth] : cache.depths)
        DestroyImage(context, depth->image);
    cache.surfaces.clear();
    cache.depths.clear();
}

static Surface* SurfaceAcquireSlow(Context& context, SurfaceCache& cache, uint32_t base, uint32_t width, uint32_t height,
    VkSampleCountFlagBits samples, uint32_t depthBase)
{
    auto& surface = cache.surfaces[base];
    // The guest can point the same EDRAM base at a different resolution (splitscreen, a resize),
    // and the settings can change the sample count: rebuild when either disagrees.
    const bool colorReady = surface.color.image && surface.width == width && surface.height == height && surface.samples == samples;
    if (!colorReady)
    {
        if (surface.framebuffer)
            vkDestroyFramebuffer(context.device, surface.framebuffer, nullptr);
        if (surface.pass)
            vkDestroyRenderPass(context.device, surface.pass, nullptr);
        DestroyImage(context, surface.color);
        surface = {};
        surface.base = base;
        surface.width = width;
        surface.height = height;
        surface.samples = samples;
        surface.color = CreateImage(context, width, height, kColorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_VIEW_TYPE_2D, 1, samples);
        if (!surface.color.image)
            return nullptr;
        surface.pass = MakeRenderPass(context, samples);
        if (!surface.pass)
            return nullptr;
    }

    if (depthBase == UINT32_MAX)
        depthBase = base;
    auto& depthSlot = cache.depths[depthBase];
    if (!depthSlot)
        depthSlot = std::make_unique<DepthSurface>();
    DepthSurface& depth = *depthSlot;
    if (depth.image.image && (depth.width != width || depth.height != height || depth.samples != samples))
    {
        DestroyImage(context, depth.image);
        depth.image = {};
        depth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (!depth.image.image)
    {
        depth.image = CreateImage(context, width, height, kDepthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_VIEW_TYPE_2D, 1, samples);
        depth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.width = width;
        depth.height = height;
        depth.samples = samples;
        if (!depth.image.image)
            return nullptr;
    }
    if (surface.depthBase != depthBase || surface.depth.image != depth.image.image || !surface.framebuffer)
    {
        if (surface.framebuffer)
            vkDestroyFramebuffer(context.device, surface.framebuffer, nullptr);
        surface.depthBase = depthBase;
        surface.depth = depth.image;
        surface.depthState = &depth;
        VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = surface.pass;
        VkImageView views[2] = { surface.color.view, surface.depth.attachmentView ? surface.depth.attachmentView : surface.depth.view };
        info.attachmentCount = 2;
        info.pAttachments = views;
        info.width = width;
        info.height = height;
        info.layers = 1;
        if (vkCreateFramebuffer(context.device, &info, nullptr, &surface.framebuffer) != VK_SUCCESS)
            return nullptr;
    }
    return &surface;
}

void SurfaceBegin(Context& context, Surface& surface, VkCommandBuffer cmd, const VkClearColorValue* clearColor)
{
    Barrier(cmd, surface.color.image, VK_IMAGE_ASPECT_COLOR_BIT, surface.colorLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    surface.colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const bool firstDepthUse = surface.depthState->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    Barrier(cmd, surface.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, surface.depthState->layout,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    surface.depthState->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = surface.pass;
    begin.framebuffer = surface.framebuffer;
    begin.renderArea = { { 0, 0 }, { surface.width, surface.height } };
    VkClearValue values[2]{};
    if (clearColor)
        values[0].color = *clearColor;
    values[1].depthStencil = { 1.0f, 0 };
    begin.clearValueCount = 2;
    begin.pClearValues = values;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (clearColor || firstDepthUse)
    {
        VkClearAttachment attachments[2]{};
        uint32_t count = 0;
        if (clearColor)
        {
            attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[count].colorAttachment = 0;
            attachments[count++].clearValue = values[0];
        }
        if (firstDepthUse)
        {
            // A new D3D12 depth surface starts at 1; subsequent changes come from the guest.
            attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            attachments[count++].clearValue = values[1];
        }
        VkClearRect rect = { begin.renderArea, 0, 1 };
        vkCmdClearAttachments(cmd, count, attachments, 1, &rect);
    }
    // hlsl.cpp emits `1 - p.y / size.y * 2`, i.e. y-down like the guest's framebuffer. Vulkan's
    // NDC has y pointing down too but its framebuffer origin is top-left, so the viewport is
    // flipped to land the image the right way up (the menu text is the quickest tell).
    VkViewport viewport{ 0, float(surface.height), float(surface.width), -float(surface.height), 0, 1 };
    VkRect2D scissor{ { 0, 0 }, { surface.width, surface.height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    (void)context;
}

void SurfaceEnd(Context& context, Surface& surface, VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
    (void)surface;
    (void)context;
}

void SurfaceShaderRead(Context& context, Surface& surface, VkCommandBuffer cmd)
{
    Barrier(cmd, surface.color.image, VK_IMAGE_ASPECT_COLOR_BIT, surface.colorLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    surface.colorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    (void)context;
}

void SurfaceDepthShaderRead(Context& context, Surface& surface, VkCommandBuffer cmd)
{
    Barrier(cmd, surface.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, surface.depthState->layout,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    surface.depthState->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    (void)context;
}

bool SurfaceResolve(Context& context, Surface& surface, VkCommandBuffer cmd, VkImage destination, uint32_t width,
    uint32_t height)
{
    if (width != surface.width || height != surface.height)
        return false;
    {
        Barrier(cmd, surface.color.image, VK_IMAGE_ASPECT_COLOR_BIT, surface.colorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        surface.colorLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkImageMemoryBarrier dst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        dst.srcAccessMask = 0;
        dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst.srcQueueFamilyIndex = dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dst.image = destination;
        dst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dst);
        if (surface.samples == VK_SAMPLE_COUNT_1_BIT)
        {
            VkImageCopy region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { width, height, 1 };
            vkCmdCopyImage(cmd, surface.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        else
        {
            VkImageResolve region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { width, height, 1 };
            vkCmdResolveImage(cmd, surface.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        read.image = destination;
        read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &read);
    }
    (void)context;
    return true;
}

// This runs once per draw, and the map lookups cost a few microseconds each. Keep the last
// (base, size, samples, depth) result and re-validate the cheap fields; a rebuild changes color.image or the
// dimensions, and the Surface lives in the cache map for the whole run, so the pointer cannot dangle.
Surface* SurfaceAcquire(Context& context, SurfaceCache& cache, uint32_t base, uint32_t width, uint32_t height,
    VkSampleCountFlagBits samples, uint32_t depthBase)
{
    static uint32_t cBase = ~0u, cW = 0, cH = 0, cD = ~0u;
    static VkSampleCountFlagBits cS = VK_SAMPLE_COUNT_1_BIT;
    static Surface* cSurface = nullptr;
    if (cSurface && base == cBase && width == cW && height == cH && depthBase == cD &&
        samples == cS && cSurface->color.image && cSurface->width == width && cSurface->height == height)
        return cSurface;
    Surface* surface = SurfaceAcquireSlow(context, cache, base, width, height, samples, depthBase);
    if (surface && surface->color.image)
    {
        cBase = base; cW = width; cH = height; cD = depthBase; cS = samples; cSurface = surface;
    }
    return surface;
}
}
