#pragma once
// The window's swap chain: the finished frame is drawn straight into its images (stretched to the window, display
// gamma applied) and presented, instead of being read back and handed to the Direct3D 12 presenter.
//
// Windows only (VK_KHR_win32_surface); the native test tools do not present.
#include <vulkan/vulkan.h>

#include <filesystem>
#include <vector>

#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_resolve.h"

namespace vk
{
struct Swapchain
{
    void* window = nullptr; // HWND
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    bool vsync = false;
    // Set when the window changed under the swap chain (resize, fullscreen): it is rebuilt at the next acquire.
    bool stale = false;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass pass = VK_NULL_HANDLE; // one colour attachment, cleared, left ready to present
    VkPipeline pipeline = VK_NULL_HANDLE; // the present shader for that pass
    // One pair per frame that can be in flight: the image is acquired / the frame's commands have finished.
    std::vector<VkSemaphore> acquired, rendered;
    uint32_t nextSemaphore = 0;
    std::filesystem::path cacheDir;
};

// False when the device cannot present to this window (no extension, no support on the queue family, no format):
// the caller keeps its readback path.
bool SwapchainCreate(const Context& context, Swapchain& swapchain, Resolve& resolve, void* window, bool vsync,
    const std::filesystem::path& cacheDir);
void SwapchainDestroy(const Context& context, Swapchain& swapchain);

// Takes the next image. False when there is nothing to draw into this frame (minimised window, or the swap chain
// could not be rebuilt): skip the present. `acquired` must be waited for by the submit that draws into the image,
// `rendered` signalled by it.
bool SwapchainAcquire(const Context& context, Swapchain& swapchain, Resolve& resolve, uint32_t& index, VkSemaphore& acquired,
    VkSemaphore& rendered);
// Begins / ends the pass on that image; draw into it with PresentDraw(swapchain.pipeline) in between.
void SwapchainBeginPass(const Swapchain& swapchain, VkCommandBuffer cmd, uint32_t index);
void SwapchainEndPass(VkCommandBuffer cmd);
// Queues the image for display once `rendered` is signalled.
void SwapchainPresent(const Context& context, Swapchain& swapchain, uint32_t index, VkSemaphore rendered);
}
