#define VK_LOADER_IMPL
#include "gpu/vk/vk_loader.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vkldr
{
PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
PFN_vkAllocateMemory vkAllocateMemory = nullptr;
PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
PFN_vkBindImageMemory vkBindImageMemory = nullptr;
PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
PFN_vkCmdClearAttachments vkCmdClearAttachments = nullptr;
PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
PFN_vkCmdDraw vkCmdDraw = nullptr;
PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
PFN_vkCmdSetBlendConstants vkCmdSetBlendConstants = nullptr;
PFN_vkCmdSetStencilReference vkCmdSetStencilReference = nullptr;
PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
PFN_vkCreateBuffer vkCreateBuffer = nullptr;
PFN_vkCreateBufferView vkCreateBufferView = nullptr;
PFN_vkDestroyBufferView vkDestroyBufferView = nullptr;
PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
PFN_vkCreateDevice vkCreateDevice = nullptr;
PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
PFN_vkCreateImage vkCreateImage = nullptr;
PFN_vkCreateImageView vkCreateImageView = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
PFN_vkCreatePipelineCache vkCreatePipelineCache = nullptr;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
PFN_vkCreateSampler vkCreateSampler = nullptr;
PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
PFN_vkDestroyDevice vkDestroyDevice = nullptr;
PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
PFN_vkDestroyImage vkDestroyImage = nullptr;
PFN_vkDestroyImageView vkDestroyImageView = nullptr;
PFN_vkDestroyInstance vkDestroyInstance = nullptr;
PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
PFN_vkDestroyPipelineCache vkDestroyPipelineCache = nullptr;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
PFN_vkDestroySampler vkDestroySampler = nullptr;
PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkFreeDescriptorSets vkFreeDescriptorSets = nullptr;
PFN_vkFreeMemory vkFreeMemory = nullptr;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPipelineCacheData vkGetPipelineCacheData = nullptr;
PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT = nullptr;
PFN_vkMapMemory vkMapMemory = nullptr;
PFN_vkUnmapMemory vkUnmapMemory = nullptr;
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;

bool Load()
{
    if (GetInstanceProcAddr)
        return true;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    if (!library)
    {
        fprintf(stderr, "vk: no vulkan-1.dll (no Vulkan driver installed?)\n");
        return false;
    }
    GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
#else
    void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        fprintf(stderr, "vk: no libvulkan.so.1 (no Vulkan driver installed?)\n");
        return false;
    }
    GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
    if (!GetInstanceProcAddr)
    {
        fprintf(stderr, "vk: the loader has no vkGetInstanceProcAddr\n");
        return false;
    }
    // Global commands are reachable with a null instance; everything else comes from
    // ResolveInstance once there is one.
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(GetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vkCreateInstance)
    {
        fprintf(stderr, "vk: the loader has no vkCreateInstance\n");
        return false;
    }
    return true;
}

PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
PFN_vkQueueSubmit vkQueueSubmit = nullptr;
PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;

bool ResolveInstance(VkInstance instance)
{
    if (!GetInstanceProcAddr)
        return false;
    int missing = 0;
    vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(GetInstanceProcAddr(instance, "vkAllocateCommandBuffers"));
    if (!vkAllocateCommandBuffers)
        missing++;
    vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(GetInstanceProcAddr(instance, "vkBeginCommandBuffer"));
    if (!vkBeginCommandBuffer)
        missing++;
    vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(GetInstanceProcAddr(instance, "vkEndCommandBuffer"));
    if (!vkEndCommandBuffer)
        missing++;
    vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(GetInstanceProcAddr(instance, "vkFreeCommandBuffers"));
    if (!vkFreeCommandBuffers)
        missing++;
    vkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(GetInstanceProcAddr(instance, "vkResetCommandBuffer"));
    if (!vkResetCommandBuffer)
        missing++;
    vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(GetInstanceProcAddr(instance, "vkQueueSubmit"));
    if (!vkQueueSubmit)
        missing++;
    vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(GetInstanceProcAddr(instance, "vkQueueWaitIdle"));
    if (!vkQueueWaitIdle)
        missing++;
    vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(GetInstanceProcAddr(instance, "vkAllocateDescriptorSets"));
    if (!vkAllocateDescriptorSets)
        missing++;
    vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(GetInstanceProcAddr(instance, "vkAllocateMemory"));
    if (!vkAllocateMemory)
        missing++;
    vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(GetInstanceProcAddr(instance, "vkBindBufferMemory"));
    if (!vkBindBufferMemory)
        missing++;
    vkBindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(GetInstanceProcAddr(instance, "vkBindImageMemory"));
    if (!vkBindImageMemory)
        missing++;
    vkCmdBeginRenderPass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(GetInstanceProcAddr(instance, "vkCmdBeginRenderPass"));
    if (!vkCmdBeginRenderPass)
        missing++;
    vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(GetInstanceProcAddr(instance, "vkCmdBindDescriptorSets"));
    if (!vkCmdBindDescriptorSets)
        missing++;
    vkCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(GetInstanceProcAddr(instance, "vkCmdBindIndexBuffer"));
    if (!vkCmdBindIndexBuffer)
        missing++;
    vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(GetInstanceProcAddr(instance, "vkCmdBindPipeline"));
    if (!vkCmdBindPipeline)
        missing++;
    vkCmdClearAttachments = reinterpret_cast<PFN_vkCmdClearAttachments>(GetInstanceProcAddr(instance, "vkCmdClearAttachments"));
    if (!vkCmdClearAttachments)
        missing++;
    vkCmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(GetInstanceProcAddr(instance, "vkCmdClearColorImage"));
    if (!vkCmdClearColorImage)
        missing++;
    vkCmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(GetInstanceProcAddr(instance, "vkCmdCopyBufferToImage"));
    if (!vkCmdCopyBufferToImage)
        missing++;
    vkCmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(GetInstanceProcAddr(instance, "vkCmdCopyImage"));
    if (!vkCmdCopyImage)
        missing++;
    vkCmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(GetInstanceProcAddr(instance, "vkCmdCopyImageToBuffer"));
    if (!vkCmdCopyImageToBuffer)
        missing++;
    vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(GetInstanceProcAddr(instance, "vkCmdDraw"));
    if (!vkCmdDraw)
        missing++;
    vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(GetInstanceProcAddr(instance, "vkCmdDrawIndexed"));
    if (!vkCmdDrawIndexed)
        missing++;
    vkCmdEndRenderPass = reinterpret_cast<PFN_vkCmdEndRenderPass>(GetInstanceProcAddr(instance, "vkCmdEndRenderPass"));
    if (!vkCmdEndRenderPass)
        missing++;
    vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(GetInstanceProcAddr(instance, "vkCmdPipelineBarrier"));
    if (!vkCmdPipelineBarrier)
        missing++;
    vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(GetInstanceProcAddr(instance, "vkCmdPushConstants"));
    if (!vkCmdPushConstants)
        missing++;
    vkCmdResolveImage = reinterpret_cast<PFN_vkCmdResolveImage>(GetInstanceProcAddr(instance, "vkCmdResolveImage"));
    if (!vkCmdResolveImage)
        missing++;
    vkCmdSetBlendConstants = reinterpret_cast<PFN_vkCmdSetBlendConstants>(GetInstanceProcAddr(instance, "vkCmdSetBlendConstants"));
    if (!vkCmdSetBlendConstants)
        missing++;
    vkCmdSetStencilReference = reinterpret_cast<PFN_vkCmdSetStencilReference>(GetInstanceProcAddr(instance, "vkCmdSetStencilReference"));
    if (!vkCmdSetStencilReference)
        missing++;
    vkCmdSetScissor = reinterpret_cast<PFN_vkCmdSetScissor>(GetInstanceProcAddr(instance, "vkCmdSetScissor"));
    if (!vkCmdSetScissor)
        missing++;
    vkCmdSetViewport = reinterpret_cast<PFN_vkCmdSetViewport>(GetInstanceProcAddr(instance, "vkCmdSetViewport"));
    if (!vkCmdSetViewport)
        missing++;
    vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(GetInstanceProcAddr(instance, "vkCreateBuffer"));
    if (!vkCreateBuffer)
        missing++;
    vkCreateBufferView = reinterpret_cast<PFN_vkCreateBufferView>(GetInstanceProcAddr(instance, "vkCreateBufferView"));
    if (!vkCreateBufferView)
        missing++;
    vkDestroyBufferView = reinterpret_cast<PFN_vkDestroyBufferView>(GetInstanceProcAddr(instance, "vkDestroyBufferView"));
    if (!vkDestroyBufferView)
        missing++;
    vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(GetInstanceProcAddr(instance, "vkCreateCommandPool"));
    if (!vkCreateCommandPool)
        missing++;
    vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(GetInstanceProcAddr(instance, "vkCreateDescriptorPool"));
    if (!vkCreateDescriptorPool)
        missing++;
    vkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(GetInstanceProcAddr(instance, "vkCreateDescriptorSetLayout"));
    if (!vkCreateDescriptorSetLayout)
        missing++;
    vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(GetInstanceProcAddr(instance, "vkCreateDevice"));
    if (!vkCreateDevice)
        missing++;
    vkCreateFramebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(GetInstanceProcAddr(instance, "vkCreateFramebuffer"));
    if (!vkCreateFramebuffer)
        missing++;
    vkCreateGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(GetInstanceProcAddr(instance, "vkCreateGraphicsPipelines"));
    if (!vkCreateGraphicsPipelines)
        missing++;
    vkCreateImage = reinterpret_cast<PFN_vkCreateImage>(GetInstanceProcAddr(instance, "vkCreateImage"));
    if (!vkCreateImage)
        missing++;
    vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(GetInstanceProcAddr(instance, "vkCreateImageView"));
    if (!vkCreateImageView)
        missing++;
    vkCreatePipelineCache = reinterpret_cast<PFN_vkCreatePipelineCache>(GetInstanceProcAddr(instance, "vkCreatePipelineCache"));
    if (!vkCreatePipelineCache)
        missing++;
    vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(GetInstanceProcAddr(instance, "vkCreatePipelineLayout"));
    if (!vkCreatePipelineLayout)
        missing++;
    vkCreateRenderPass = reinterpret_cast<PFN_vkCreateRenderPass>(GetInstanceProcAddr(instance, "vkCreateRenderPass"));
    if (!vkCreateRenderPass)
        missing++;
    vkCreateSampler = reinterpret_cast<PFN_vkCreateSampler>(GetInstanceProcAddr(instance, "vkCreateSampler"));
    if (!vkCreateSampler)
        missing++;
    vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(GetInstanceProcAddr(instance, "vkCreateShaderModule"));
    if (!vkCreateShaderModule)
        missing++;
    vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(GetInstanceProcAddr(instance, "vkDestroyBuffer"));
    if (!vkDestroyBuffer)
        missing++;
    vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(GetInstanceProcAddr(instance, "vkDestroyCommandPool"));
    if (!vkDestroyCommandPool)
        missing++;
    vkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(GetInstanceProcAddr(instance, "vkDestroyDescriptorPool"));
    if (!vkDestroyDescriptorPool)
        missing++;
    vkDestroyDescriptorSetLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(GetInstanceProcAddr(instance, "vkDestroyDescriptorSetLayout"));
    if (!vkDestroyDescriptorSetLayout)
        missing++;
    vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(GetInstanceProcAddr(instance, "vkDestroyDevice"));
    if (!vkDestroyDevice)
        missing++;
    vkDestroyFramebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(GetInstanceProcAddr(instance, "vkDestroyFramebuffer"));
    if (!vkDestroyFramebuffer)
        missing++;
    vkDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(GetInstanceProcAddr(instance, "vkDestroyImage"));
    if (!vkDestroyImage)
        missing++;
    vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(GetInstanceProcAddr(instance, "vkDestroyImageView"));
    if (!vkDestroyImageView)
        missing++;
    vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(GetInstanceProcAddr(instance, "vkDestroyInstance"));
    if (!vkDestroyInstance)
        missing++;
    vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(GetInstanceProcAddr(instance, "vkDestroyPipeline"));
    if (!vkDestroyPipeline)
        missing++;
    vkDestroyPipelineCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(GetInstanceProcAddr(instance, "vkDestroyPipelineCache"));
    if (!vkDestroyPipelineCache)
        missing++;
    vkDestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(GetInstanceProcAddr(instance, "vkDestroyPipelineLayout"));
    if (!vkDestroyPipelineLayout)
        missing++;
    vkDestroyRenderPass = reinterpret_cast<PFN_vkDestroyRenderPass>(GetInstanceProcAddr(instance, "vkDestroyRenderPass"));
    if (!vkDestroyRenderPass)
        missing++;
    vkDestroySampler = reinterpret_cast<PFN_vkDestroySampler>(GetInstanceProcAddr(instance, "vkDestroySampler"));
    if (!vkDestroySampler)
        missing++;
    vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(GetInstanceProcAddr(instance, "vkDestroyShaderModule"));
    if (!vkDestroyShaderModule)
        missing++;
    vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(GetInstanceProcAddr(instance, "vkDeviceWaitIdle"));
    if (!vkDeviceWaitIdle)
        missing++;
    vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    if (!vkEnumeratePhysicalDevices)
        missing++;
    vkEnumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(GetInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
    if (!vkEnumerateDeviceExtensionProperties)
        missing++;
    vkFreeDescriptorSets = reinterpret_cast<PFN_vkFreeDescriptorSets>(GetInstanceProcAddr(instance, "vkFreeDescriptorSets"));
    if (!vkFreeDescriptorSets)
        missing++;
    vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(GetInstanceProcAddr(instance, "vkFreeMemory"));
    if (!vkFreeMemory)
        missing++;
    vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(GetInstanceProcAddr(instance, "vkGetBufferMemoryRequirements"));
    if (!vkGetBufferMemoryRequirements)
        missing++;
    vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(GetInstanceProcAddr(instance, "vkGetDeviceQueue"));
    if (!vkGetDeviceQueue)
        missing++;
    vkGetImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(GetInstanceProcAddr(instance, "vkGetImageMemoryRequirements"));
    if (!vkGetImageMemoryRequirements)
        missing++;
    vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!vkGetPhysicalDeviceFeatures2)
        missing++;
    vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!vkGetPhysicalDeviceMemoryProperties)
        missing++;
    vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    if (!vkGetPhysicalDeviceProperties)
        missing++;
    vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (!vkGetPhysicalDeviceProperties2)
        missing++;
    vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!vkGetPhysicalDeviceQueueFamilyProperties)
        missing++;
    vkGetPipelineCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(GetInstanceProcAddr(instance, "vkGetPipelineCacheData"));
    if (!vkGetPipelineCacheData)
        missing++;
    vkGetMemoryHostPointerPropertiesEXT =
        reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(GetInstanceProcAddr(instance, "vkGetMemoryHostPointerPropertiesEXT"));
    vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(GetInstanceProcAddr(instance, "vkMapMemory"));
    if (!vkMapMemory)
        missing++;
    vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(GetInstanceProcAddr(instance, "vkUnmapMemory"));
    if (!vkUnmapMemory)
        missing++;
    vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(GetInstanceProcAddr(instance, "vkUpdateDescriptorSets"));
    if (!vkUpdateDescriptorSets)
        missing++;
    if (missing)
        fprintf(stderr, "vk: %d entry points missing from the loader\n", missing);
    return missing == 0;
}
}
