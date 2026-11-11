#include "gpu/vk/vk_swapchain.h"

#if defined(_WIN32)
#include <windows.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>

#include "gpu/vk/vk_loader.h"
#include "log.h"

namespace vk
{
namespace
{
// The window-system entry points are not in the loader's table (vk_loader.h): they exist only where the extensions do.
struct Api
{
    PFN_vkCreateWin32SurfaceKHR createSurface = nullptr;
    PFN_vkDestroySurfaceKHR destroySurface = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR presentModes = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR swapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    bool complete = false;
};

const Api& GetApi(const Context& context)
{
    static const Api api = [&context] {
        Api a;
        uint32_t missing = 0;
        auto get = [&](const char* name) {
            const PFN_vkVoidFunction f = vkldr::GetInstanceProcAddr(context.instance, name);
            missing += f == nullptr;
            return f;
        };
        a.createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get("vkCreateWin32SurfaceKHR"));
        a.destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(get("vkDestroySurfaceKHR"));
        a.surfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(get("vkGetPhysicalDeviceSurfaceSupportKHR"));
        a.surfaceCapabilities =
            reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(get("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        a.surfaceFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(get("vkGetPhysicalDeviceSurfaceFormatsKHR"));
        a.presentModes =
            reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(get("vkGetPhysicalDeviceSurfacePresentModesKHR"));
        a.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(get("vkCreateSwapchainKHR"));
        a.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(get("vkDestroySwapchainKHR"));
        a.swapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(get("vkGetSwapchainImagesKHR"));
        a.acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(get("vkAcquireNextImageKHR"));
        a.present = reinterpret_cast<PFN_vkQueuePresentKHR>(get("vkQueuePresentKHR"));
        a.createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(get("vkCreateSemaphore"));
        a.destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(get("vkDestroySemaphore"));
        a.complete = missing == 0;
        return a;
    }();
    return api;
}

void DestroyImages(const Context& context, Swapchain& swapchain)
{
    for (VkFramebuffer framebuffer : swapchain.framebuffers)
        vkDestroyFramebuffer(context.device, framebuffer, nullptr);
    for (VkImageView view : swapchain.views)
        vkDestroyImageView(context.device, view, nullptr);
    swapchain.framebuffers.clear();
    swapchain.views.clear();
    swapchain.images.clear();
}

// Whether the window is no longer the size the swap chain was built for. "Suboptimal" alone is not a reason to
// rebuild: some window systems report it for every frame of a swap chain that is exactly right.
bool ExtentChanged(const Context& context, const Swapchain& swapchain)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    if (GetApi(context).surfaceCapabilities(context.physical, swapchain.surface, &capabilities) != VK_SUCCESS)
        return true;
    return capabilities.currentExtent.width != UINT32_MAX &&
        (capabilities.currentExtent.width != swapchain.extent.width || capabilities.currentExtent.height != swapchain.extent.height);
}

// (Re)builds the swap chain for the window's current size. False with the old one gone: nothing to present into.
bool Build(const Context& context, Swapchain& swapchain, Resolve& resolve)
{
    const Api& api = GetApi(context);
    VkSurfaceCapabilitiesKHR capabilities{};
    if (api.surfaceCapabilities(context.physical, swapchain.surface, &capabilities) != VK_SUCCESS)
        return false;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        RECT client{};
        GetClientRect(static_cast<HWND>(swapchain.window), &client);
        extent = { uint32_t(client.right), uint32_t(client.bottom) };
    }
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    if (!extent.width || !extent.height)
        return false; // minimised: try again at a later frame

    uint32_t count = 0;
    api.surfaceFormats(context.physical, swapchain.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    api.surfaceFormats(context.physical, swapchain.surface, &count, formats.data());
    // A plain 8-bit format: the present shader writes display values, as the Direct3D 12 swap chain's R8G8B8A8_UNORM takes.
    const VkSurfaceFormatKHR* chosen = nullptr;
    for (const VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32 })
    {
        for (const VkSurfaceFormatKHR& format : formats)
            if (format.format == want && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen = &format;
                break;
            }
        if (chosen)
            break;
    }
    if (!chosen)
        return false;

    api.presentModes(context.physical, swapchain.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    api.presentModes(context.physical, swapchain.surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
    // VSync off: show the newest frame at once (tearing), or failing that replace the queued one.
    const VkPresentModeKHR mode = swapchain.vsync ? VK_PRESENT_MODE_FIFO_KHR
        : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
        : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;

    VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = swapchain.surface;
    info.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && info.minImageCount > capabilities.maxImageCount)
        info.minImageCount = capabilities.maxImageCount;
    info.imageFormat = chosen->format;
    info.imageColorSpace = chosen->colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const VkCompositeAlphaFlagBitsKHR alpha : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
             VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR })
        if (capabilities.supportedCompositeAlpha & alpha)
        {
            info.compositeAlpha = alpha;
            break;
        }
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain.handle;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult result = api.createSwapchain(context.device, &info, nullptr, &created);
    if (result != VK_SUCCESS && info.presentMode != VK_PRESENT_MODE_FIFO_KHR)
    {
        // A mode the surface lists is not always one it can do (a GPU presenting to another GPU's display): FIFO is
        // the one every implementation has to support.
        Log("vk: swap chain with present mode %d failed (%d): trying FIFO", int(info.presentMode), int(result));
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        created = VK_NULL_HANDLE;
        result = api.createSwapchain(context.device, &info, nullptr, &created);
    }
    DestroyImages(context, swapchain);
    if (swapchain.handle)
        api.destroySwapchain(context.device, swapchain.handle, nullptr);
    swapchain.handle = created;
    if (result != VK_SUCCESS)
    {
        swapchain.handle = VK_NULL_HANDLE;
        Log("vk: vkCreateSwapchainKHR failed (%d): %ux%u, %u images, format %d, %zu present modes", int(result), extent.width,
            extent.height, info.minImageCount, int(info.imageFormat), modes.size());
        return false;
    }

    if (swapchain.format != chosen->format || !swapchain.pass)
    {
        if (swapchain.pipeline)
            vkDestroyPipeline(context.device, swapchain.pipeline, nullptr);
        if (swapchain.pass)
            vkDestroyRenderPass(context.device, swapchain.pass, nullptr);
        swapchain.pipeline = VK_NULL_HANDLE;
        swapchain.pass = VK_NULL_HANDLE;
        VkAttachmentDescription color{};
        color.format = chosen->format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        // The image becomes ours when the acquire semaphore is signalled, which the submit waits for at this stage.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        passInfo.attachmentCount = 1;
        passInfo.pAttachments = &color;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        passInfo.dependencyCount = 1;
        passInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(context.device, &passInfo, nullptr, &swapchain.pass) != VK_SUCCESS)
            return false;
        swapchain.pipeline = ResolveMakePresentPipeline(context, resolve, swapchain.pass, swapchain.cacheDir);
        if (!swapchain.pipeline)
            return false;
        swapchain.format = chosen->format;
    }

    api.swapchainImages(context.device, swapchain.handle, &count, nullptr);
    swapchain.images.resize(count);
    api.swapchainImages(context.device, swapchain.handle, &count, swapchain.images.data());
    for (VkImage image : swapchain.images)
    {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain.format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(context.device, &viewInfo, nullptr, &view) != VK_SUCCESS)
            return false;
        swapchain.views.push_back(view);
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = swapchain.pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(context.device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS)
            return false;
        swapchain.framebuffers.push_back(framebuffer);
    }
    // Enough pairs that one is never reused while a frame that waits on it is still queued.
    while (swapchain.acquired.size() < swapchain.images.size() + 2)
    {
        const VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore a = VK_NULL_HANDLE, r = VK_NULL_HANDLE;
        if (api.createSemaphore(context.device, &semaphoreInfo, nullptr, &a) != VK_SUCCESS ||
            api.createSemaphore(context.device, &semaphoreInfo, nullptr, &r) != VK_SUCCESS)
            return false;
        swapchain.acquired.push_back(a);
        swapchain.rendered.push_back(r);
    }
    swapchain.extent = extent;
    swapchain.stale = false;
    Log("vk: swap chain %ux%u, %zu images, format %d, present mode %d", extent.width, extent.height,
        swapchain.images.size(), int(swapchain.format), int(info.presentMode));
    return true;
}
}

bool SwapchainCreate(const Context& context, Swapchain& swapchain, Resolve& resolve, void* window, bool vsync,
    const std::filesystem::path& cacheDir)
{
    if (!window || !context.surfaceExtensions || !context.swapchainExtension)
    {
        Log("vk: no swap chain: surface extensions %d, swapchain extension %d", int(context.surfaceExtensions), int(context.swapchainExtension));
        return false;
    }
    const Api& api = GetApi(context);
    if (!api.complete)
    {
        Log("vk: no swap chain: window-system entry points are missing");
        return false;
    }
    VkWin32SurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(window);
    if (const VkResult result = api.createSurface(context.instance, &surfaceInfo, nullptr, &swapchain.surface); result != VK_SUCCESS)
    {
        Log("vk: no swap chain: vkCreateWin32SurfaceKHR failed (%d)", int(result));
        return false;
    }
    VkBool32 supported = VK_FALSE;
    api.surfaceSupport(context.physical, context.queueFamily, swapchain.surface, &supported);
    swapchain.window = window;
    swapchain.vsync = vsync;
    swapchain.cacheDir = cacheDir;
    if (!supported || !Build(context, swapchain, resolve))
    {
        Log("vk: this device cannot present to the window (queue support %d)", int(supported));
        SwapchainDestroy(context, swapchain);
        return false;
    }
    return true;
}

void SwapchainDestroy(const Context& context, Swapchain& swapchain)
{
    const Api& api = GetApi(context);
    if (context.device)
        vkDeviceWaitIdle(context.device);
    DestroyImages(context, swapchain);
    for (VkSemaphore semaphore : swapchain.acquired)
        api.destroySemaphore(context.device, semaphore, nullptr);
    for (VkSemaphore semaphore : swapchain.rendered)
        api.destroySemaphore(context.device, semaphore, nullptr);
    if (swapchain.pipeline)
        vkDestroyPipeline(context.device, swapchain.pipeline, nullptr);
    if (swapchain.pass)
        vkDestroyRenderPass(context.device, swapchain.pass, nullptr);
    if (swapchain.handle)
        api.destroySwapchain(context.device, swapchain.handle, nullptr);
    if (swapchain.surface)
        api.destroySurface(context.instance, swapchain.surface, nullptr);
    swapchain = {};
}

bool SwapchainAcquire(const Context& context, Swapchain& swapchain, Resolve& resolve, uint32_t& index, VkSemaphore& acquired,
    VkSemaphore& rendered)
{
    if (!swapchain.surface)
        return false;
    const Api& api = GetApi(context);
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (swapchain.stale || !swapchain.handle)
        {
            // Frames in flight still draw into the old images.
            vkDeviceWaitIdle(context.device);
            if (!Build(context, swapchain, resolve))
                return false;
        }
        const uint32_t slot = swapchain.nextSemaphore % uint32_t(swapchain.acquired.size());
        const VkResult result = api.acquire(context.device, swapchain.handle, UINT64_MAX, swapchain.acquired[slot], VK_NULL_HANDLE, &index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            // Suboptimal still presents; rebuild from the next frame if the window really changed.
            swapchain.stale = result == VK_SUBOPTIMAL_KHR && ExtentChanged(context, swapchain);
            swapchain.nextSemaphore++;
            acquired = swapchain.acquired[slot];
            rendered = swapchain.rendered[slot];
            return true;
        }
        if (result != VK_ERROR_OUT_OF_DATE_KHR)
        {
            Log("vk: vkAcquireNextImageKHR failed (%d)", int(result));
            swapchain.stale = true;
            return false;
        }
        swapchain.stale = true;
    }
    return false;
}

void SwapchainBeginPass(const Swapchain& swapchain, VkCommandBuffer cmd, uint32_t index)
{
    VkClearValue black{};
    black.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = swapchain.pass;
    begin.framebuffer = swapchain.framebuffers[index];
    begin.renderArea = { { 0, 0 }, swapchain.extent };
    begin.clearValueCount = 1;
    begin.pClearValues = &black;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void SwapchainEndPass(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
}

void SwapchainPresent(const Context& context, Swapchain& swapchain, uint32_t index, VkSemaphore rendered)
{
    const Api& api = GetApi(context);
    VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &rendered;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain.handle;
    info.pImageIndices = &index;
    const VkResult result = api.present(context.queue, &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        swapchain.stale = true;
    else if (result == VK_SUBOPTIMAL_KHR)
        swapchain.stale = ExtentChanged(context, swapchain);
    else if (result != VK_SUCCESS)
    {
        static int reported = 0;
        if (reported++ < 3)
            Log("vk: vkQueuePresentKHR failed (%d)", int(result));
        swapchain.stale = true;
    }
}
}
#else
namespace vk
{
bool SwapchainCreate(const Context&, Swapchain&, Resolve&, void*, bool, const std::filesystem::path&) { return false; }
void SwapchainDestroy(const Context&, Swapchain&) {}
bool SwapchainAcquire(const Context&, Swapchain&, Resolve&, uint32_t&, VkSemaphore&, VkSemaphore&) { return false; }
void SwapchainBeginPass(const Swapchain&, VkCommandBuffer, uint32_t) {}
void SwapchainEndPass(VkCommandBuffer) {}
void SwapchainPresent(const Context&, Swapchain&, uint32_t, VkSemaphore) {}
}
#endif
