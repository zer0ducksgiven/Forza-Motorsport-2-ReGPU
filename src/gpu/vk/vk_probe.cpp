// FM2_VULKAN_PROBE=1: open the Vulkan device the native renderer would use and say what it is,
// then exit. The way to tell whether a machine (or a Wine prefix) can run the backend at all,
// before any of the renderer is wired up.
#include <cstdio>

#include "gpu/vk/vk_context.h"

bool VulkanProbe()
{
    vk::Context context;
    if (!vk::Init(context))
    {
        printf("vulkan: no device\n");
        return false;
    }
    printf("vulkan: %s (API %u.%u.%u), descriptor indexing %s, maxStorageBufferRange %u MB\n", context.props.deviceName,
        VK_VERSION_MAJOR(context.props.apiVersion), VK_VERSION_MINOR(context.props.apiVersion), VK_VERSION_PATCH(context.props.apiVersion),
        context.descriptorIndexing ? "yes" : "no", uint32_t(context.props.limits.maxStorageBufferRange >> 20));
    vk::Shutdown(context);
    return true;
}
