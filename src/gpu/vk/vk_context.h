#pragma once
// Vulkan device plumbing for the native renderer: instance, device, queue, memory and the
// one-shot command helpers everything else uses. No window and no swap chain yet: the
// renderer renders to images and the presenter reads them back (FM2_HEADLESS), so the whole
// backend is testable without a platform layer.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <functional>
#include "gpu/vk/vk_loader.h"

#include "fast_env.h"
// Every getenv in the Vulkan backend goes through the cache (see fast_env.h).
#define getenv(name) FastEnv(name)

namespace vk
{
struct Context
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    // Device-wide pipeline cache, kept on disk: creating a pipeline costs ~140 ms cold on this
    // driver and ~2.6 ms from the cache, which is the difference between a 2 minute first start
    // and a 2 second one for the game's ~780 pipelines.
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::filesystem::path pipelineCachePath;
    // Descriptor indexing: the shaders index their texture arrays with runtime values.
    bool descriptorIndexing = false;
    // VK_EXT_external_memory_host: the guest's own memory becomes a GPU buffer, so the
    // shaders read vertex and index data where the game wrote it (the D3D12 path opens the same
    // memory as a heap).
    bool externalMemoryHost = false;
};

bool Init(Context& context, const std::filesystem::path& cacheDir = {});
void Shutdown(Context& context);
// Persists the pipeline cache without tearing the device down: a game run does not reach Shutdown.
void SavePipelineCache(Context& context);

uint32_t FindMemory(const Context& context, uint32_t bits, VkMemoryPropertyFlags want);

// Host-visible, coherent buffer. mapped is kept valid for the buffer's lifetime.
// extra: additional memory property flags to ask for when the buffer is mapped (e.g. HOST_CACHED for
// a readback buffer whose contents will be read on the CPU). Fallen back on if the driver has no
// memory type with them, so it is only ever a hint.
VkBuffer CreateBuffer(const Context& context, VkDeviceSize size, VkBufferUsageFlags usage, void** mapped,
    VkMemoryPropertyFlags extra = 0);

// Wraps a page-aligned host allocation as a buffer without copying it. Returns
// VK_NULL_HANDLE when the device lacks VK_EXT_external_memory_host.
VkBuffer CreateHostImportedBuffer(const Context& context, void* host, VkDeviceSize size);

// The pipeline layout the game's translated shaders expect: two constant buffers, guest
// memory as storage buffers, four samplers, and one bindless array each for 2D and cube
// textures (dxc's register mapping: b0/b1, t0->100, t1->101, s0..s3->200..203, and the
// register spaces become descriptor sets 1 and 2).
// set0Out (optional) receives the layout of set 0, for the caller to allocate a descriptor set from.
bool CreateRendererLayout(const Context& context, VkPipelineLayout& layout, VkDescriptorSetLayout* set0Out = nullptr);

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView attachmentView = VK_NULL_HANDLE;
    VkFramebuffer resolveFramebuffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1;
    // Set when `memory` is a range of a pooled block (see CreateImage): returned to the pool, never freed.
    VkDeviceSize poolOffset = 0, poolSize = 0;
    uint32_t poolBlock = UINT32_MAX;
};
// Device-local 2D image with a view covering every mip and layer.
Image CreateImage(const Context& context, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D, uint32_t layers = 1,
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, uint32_t levels = 1, VkComponentMapping components = {});
void DestroyImage(const Context& context, Image& image);

// Uploads tightly packed RGBA8 rows for one mip into a SHADER_READ_ONLY image.
void UploadImage(const Context& context, const Image& image, const void* rgba, VkDeviceSize bytes,
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, uint32_t level = 0);

// Records `record` into a pooled command buffer that is submitted WITHOUT waiting. `bytes` of `data` are first copied
// into a pooled staging buffer that stays alive until the GPU has finished; `record` receives that buffer. Queue
// order makes the upload run before any later submission that samples the image, so nothing has to wait: a
// vkQueueWaitIdle here stalls for the rest of the PREVIOUS frame's GPU time (12-17 ms for one 64x64 texture on an
// integrated GPU, 22-42 ms on a discrete one). FM2_VK_SYNC_UPLOADS=1 restores the blocking behaviour for comparison.
// Uploads are batched: `record` also receives the offset of its bytes in the staging buffer, and nothing reaches the
// queue until FlushUploads. EVERY vkQueueSubmit of commands that sample an uploaded image must flush first.
bool SubmitUpload(const Context& context, VkImage target, const void* data, VkDeviceSize bytes,
    const std::function<void(VkCommandBuffer, VkBuffer, VkDeviceSize)>& record);
void FlushUploads(const Context& context);
void UploadsWaitForImage(const Context& context, VkImage image);
// Returns finished upload jobs to the pool; waitAll blocks until every pending one is done.
void CollectUploads(const Context& context, bool waitAll);
void ShutdownUploads(const Context& context);
extern uint64_t g_uploadSubmits, g_uploadBytes, g_uploadFlushes;
// Wall time inside CreateImage and inside SubmitUpload (staging copy + recording), for the slow_vk line.
extern double g_imageCreateMs, g_uploadRecordMs, g_slotWriteMs;
extern uint64_t g_slotWrites;
// Called at the top of FlushUploads, i.e. before every queue submission: work that only has to be done by then
// (vk_texture.cpp's batched bindless descriptor writes).
extern void (*g_beforeSubmit)(const Context& context);
extern uint64_t g_imageCreates;

// Runs `record` in a command buffer and waits for it. Returns false if submission failed.
template <typename F>
bool Submit(const Context& context, F&& record)
{
    FlushUploads(context); // pending texture uploads go first: `record` may read them
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = context.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(context.device, &ai, &cb) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool ok = vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
    if (ok)
    {
        record(cb);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    }
    if (ok)
    {
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        const VkResult submitted = vkQueueSubmit(context.queue, 1, &si, VK_NULL_HANDLE);
        const VkResult waited = submitted == VK_SUCCESS ? vkQueueWaitIdle(context.queue) : submitted;
        if (submitted != VK_SUCCESS || waited != VK_SUCCESS)
            fprintf(stderr, "vk: submit %d, wait %d\n", int(submitted), int(waited));
        ok = submitted == VK_SUCCESS && waited == VK_SUCCESS;
    }
    vkFreeCommandBuffers(context.device, context.pool, 1, &cb);
    return ok;
}
}
