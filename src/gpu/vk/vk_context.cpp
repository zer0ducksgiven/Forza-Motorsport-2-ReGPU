#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "log.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace vk
{
double g_imageCreateMs = 0, g_uploadRecordMs = 0, g_slotWriteMs = 0;
uint64_t g_slotWrites = 0;
void (*g_beforeSubmit)(const Context& context) = nullptr;
uint64_t g_imageCreates = 0;
namespace
{
struct WallMs
{
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double& sink;
    explicit WallMs(double& s) : sink(s) {}
    ~WallMs() { sink += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};
}
namespace
{
bool Fail(const char* what, VkResult result)
{
    fprintf(stderr, "vk: %s failed (%d)\n", what, int(result));
    return false;
}
bool NameContains(const char* name, const char* needle)
{
    while (*name)
    {
        const char* a = name;
        const char* b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        { a++; b++; }
        if (!*b)
            return true;
        name++;
    }
    return false;
}
bool SelectDevice(const std::vector<VkPhysicalDevice>& devices, uint32_t& selected)
{
    selected = 0;
    const char* pick = getenv("FM2_VK_DEVICE");
    uint32_t chosen = 0;
    if (pick && *pick)
    {
        chosen = devices.size();
        char* end;
        long index = strtol(pick, &end, 10);
        if (*end == 0 && index >= 0 && size_t(index) < devices.size())
            chosen = size_t(index);
        else if (*end != 0)
            for (size_t i = 0; i < devices.size(); i++)
            {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(devices[i], &props);
                if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && NameContains(props.deviceName, pick))
                    { chosen = size_t(i); break; }
            }
        if (chosen == devices.size())
            return Fail("FM2_VK_DEVICE", VK_ERROR_INITIALIZATION_FAILED);
    }
    selected = chosen;
    for (size_t i = 0; i < devices.size(); i++)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        fprintf(stderr, "vk: device %zu: %s%s\n", i, props.deviceName, i == selected ? " (selected)" : "");
    }
    return true;
}
}

bool Init(Context& context, const std::filesystem::path& cacheDir)
{
    if (!vkldr::Load())
        return false;
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "fm2";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (getenv("FM2_VK_VALIDATION"))
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &validationLayer;
    }
#if defined(_WIN32)
    // The window-system extensions, when the loader has them: without them the frame is read back for the presenter.
    const char* const surfaceNames[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            vkldr::GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")))
    {
        uint32_t instanceExtensionCount = 0;
        enumerate(nullptr, &instanceExtensionCount, nullptr);
        std::vector<VkExtensionProperties> instanceExtensions(instanceExtensionCount);
        enumerate(nullptr, &instanceExtensionCount, instanceExtensions.data());
        uint32_t found = 0;
        for (const auto& extension : instanceExtensions)
            for (const char* name : surfaceNames)
                found += !strcmp(extension.extensionName, name);
        if (found == 2)
        {
            context.surfaceExtensions = true;
            ici.enabledExtensionCount = 2;
            ici.ppEnabledExtensionNames = surfaceNames;
        }
    }
#endif
    if (vkCreateInstance(&ici, nullptr, &context.instance) != VK_SUCCESS)
        return Fail("vkCreateInstance", VK_ERROR_INITIALIZATION_FAILED);
    if (!vkldr::ResolveInstance(context.instance))
        return Fail("vkGetInstanceProcAddr", VK_ERROR_INITIALIZATION_FAILED);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(context.instance, &count, nullptr);
    if (!count)
        return Fail("no Vulkan device", VK_ERROR_INITIALIZATION_FAILED);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(context.instance, &count, devices.data());
    uint32_t selected = 0;
    if (!SelectDevice(devices, selected))
        return false;
    context.physical = devices[selected];
    vkGetPhysicalDeviceProperties(context.physical, &context.props);

    VkPhysicalDeviceDescriptorIndexingFeatures indexing{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    indexing.runtimeDescriptorArray = VK_TRUE;
    indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    indexing.descriptorBindingPartiallyBound = VK_TRUE;
    indexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    indexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
    VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    query.pNext = &indexing;
    vkGetPhysicalDeviceFeatures2(context.physical, &query);
    context.descriptorIndexing = indexing.runtimeDescriptorArray == VK_TRUE && indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    VkPhysicalDeviceDescriptorIndexingProperties descriptorProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties2.pNext = &descriptorProperties;
    vkGetPhysicalDeviceProperties2(context.physical, &properties2);
    fprintf(stderr, "vk: update-after-bind sampled image limit %u\n", descriptorProperties.maxPerStageDescriptorUpdateAfterBindSampledImages);
    fprintf(stderr, "vk: descriptor indexing runtime %d nonuniform %d partial %d update %d variable %d\n", indexing.runtimeDescriptorArray,
        indexing.shaderSampledImageArrayNonUniformIndexing, indexing.descriptorBindingPartiallyBound,
        indexing.descriptorBindingSampledImageUpdateAfterBind, indexing.descriptorBindingVariableDescriptorCount);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(context.physical, &familyCount, families.data());
    context.queueFamily = familyCount;
    for (uint32_t i = 0; i < familyCount; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            context.queueFamily = i;
            break;
        }
    if (context.queueFamily == familyCount)
        return Fail("no graphics queue", VK_ERROR_INITIALIZATION_FAILED);

    // Device extensions, if the driver offers them.
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(context.physical, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(context.physical, nullptr, &extensionCount, extensions.data());
    std::vector<const char*> deviceExtensions;
    for (const auto& extension : extensions)
        if (!strcmp(extension.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
        {
            context.externalMemoryHost = true;
            deviceExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        }
        else if (context.surfaceExtensions && !strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            context.swapchainExtension = true;
            deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = context.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.features.samplerAnisotropy = query.features.samplerAnisotropy;
    features.features.pipelineStatisticsQuery = query.features.pipelineStatisticsQuery;
    features.features.textureCompressionBC = query.features.textureCompressionBC;
    features.features.multiDrawIndirect = query.features.multiDrawIndirect;
    // The descriptor indexing features the bindless texture sets rely on, as the device reported them above.
    indexing.pNext = nullptr;
    features.pNext = &indexing;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
    dci.ppEnabledExtensionNames = deviceExtensions.data();
    if (vkCreateDevice(context.physical, &dci, nullptr, &context.device) != VK_SUCCESS)
        return Fail("vkCreateDevice", VK_ERROR_INITIALIZATION_FAILED);
    vkGetDeviceQueue(context.device, context.queueFamily, 0, &context.queue);

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = context.queueFamily;
    if (vkCreateCommandPool(context.device, &pci, nullptr, &context.pool) != VK_SUCCESS)
        return Fail("vkCreateCommandPool", VK_ERROR_INITIALIZATION_FAILED);

    std::vector<char> cached;
    VkPipelineCacheCreateInfo pcci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (!cacheDir.empty())
    {
        // Vendor- and device-specific: selecting a different adapter must not read pipelines compiled for
        // another one, so the device identity is a subdirectory of the cache, next to the shader cache.
        context.pipelineCachePath = cacheDir / (std::to_string(context.props.vendorID) + "-" + std::to_string(context.props.deviceID))
            / "pipelines.bin";
        if (FILE* f = fopen(context.pipelineCachePath.string().c_str(), "rb"))
        {
            // ftell returns -1 on a non-seekable or broken file, and size_t(-1) would ask the vector
            // for ~18 EB - a bad_alloc/abort at startup. Treat any non-positive size as no cache.
            fseek(f, 0, SEEK_END);
            const long bytes = ftell(f);
            if (bytes > 0)
                cached.resize(size_t(bytes));
            fseek(f, 0, SEEK_SET);
            if (bytes > 0 && fread(cached.data(), 1, cached.size(), f) != cached.size())
                cached.clear();
            fclose(f);
        }
        // Say whether the cache was actually loaded: a silent 0 here looks identical to a wrong cache path.
        fprintf(stderr, "vk: pipeline cache %s (%zu bytes loaded)\n", cached.empty() ? "cold" : "warm", cached.size());
        pcci.initialDataSize = cached.size();
        pcci.pInitialData = cached.data();
    }
    if (vkCreatePipelineCache(context.device, &pcci, nullptr, &context.pipelineCache) != VK_SUCCESS)
        return Fail("vkCreatePipelineCache", VK_ERROR_INITIALIZATION_FAILED);
    return true;
}

// Writes the pipeline cache to disk without tearing anything down: a game run does not reach Shutdown(),
// so the renderer calls this from its save path.
size_t SavePipelineCache(Context& context)
{
    if (!context.device || !context.pipelineCache || context.pipelineCachePath.empty())
        return 0;
    // The renderer saves while the game runs (and the process can be ended at any moment): written to a temporary
    // file and renamed over the old one, so a cut-off write never leaves half a cache behind.
    size_t bytes = 0;
    if (vkGetPipelineCacheData(context.device, context.pipelineCache, &bytes, nullptr) != VK_SUCCESS || !bytes)
        return 0;
    std::vector<char> data(bytes);
    if (vkGetPipelineCacheData(context.device, context.pipelineCache, &bytes, data.data()) != VK_SUCCESS)
        return 0;
    std::error_code ec;
    std::filesystem::create_directories(context.pipelineCachePath.parent_path(), ec);
    std::filesystem::path temporary = context.pipelineCachePath;
    temporary += ".tmp";
    FILE* f = fopen(temporary.string().c_str(), "wb");
    if (!f)
        return 0;
    const bool written = fwrite(data.data(), 1, bytes, f) == bytes;
    if (fclose(f) != 0 || !written)
        return 0;
    std::filesystem::rename(temporary, context.pipelineCachePath, ec);
    return ec ? 0 : bytes;
}

namespace
{
void ImagePoolShutdown(const Context& context);
}

void Shutdown(Context& context)
{
    if (context.device)
    {
        vkDeviceWaitIdle(context.device);
        ShutdownUploads(context);
        ImagePoolShutdown(context);
        SavePipelineCache(context);
        if (context.pipelineCache)
            vkDestroyPipelineCache(context.device, context.pipelineCache, nullptr);
        if (context.pool)
            vkDestroyCommandPool(context.device, context.pool, nullptr);
        vkDestroyDevice(context.device, nullptr);
    }
    if (context.instance)
        vkDestroyInstance(context.instance, nullptr);
    context = {};
}

bool CreateRendererLayout(const Context& context, VkPipelineLayout& layout, VkDescriptorSetLayout* set0Out)
{
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding sets0[5]{};
    auto bind = [&](int i, VkDescriptorType type, uint32_t binding, VkShaderStageFlags stages) {
        sets0[i].binding = binding;
        sets0[i].descriptorType = type;
        sets0[i].descriptorCount = 1;
        sets0[i].stageFlags = stages;
    };
    // The two constant blocks change every draw: bound once as a set with dynamic offsets
    // rather than rewritten per draw.
    bind(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0, both); // VSConstants
    bind(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both); // PSConstants
    bind(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100, both); // gmem
    bind(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 101, both); // gphys (guest memory)
    // The four guest samplers are ONE array binding (hlsl.cpp: SamplerState gSamplers[4] : register(s0)), indexed by
    // the fetch's mode, so a fetch is one sample instruction instead of a branch to each of four.
    bind(4, VK_DESCRIPTOR_TYPE_SAMPLER, 200, both);
    sets0[4].descriptorCount = 4;
    VkDescriptorSetLayoutCreateInfo i0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    i0.bindingCount = 5;
    i0.pBindings = sets0;
    VkDescriptorSetLayout set0 = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(context.device, &i0, nullptr, &set0) != VK_SUCCESS)
        return false;

    // The bindless arrays: indexed at runtime, partially filled, appended to as textures load.
    VkDescriptorSetLayoutBinding array{};
    array.binding = 0;
    array.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    array.descriptorCount = getenv("FM2_SMALL_DESCRIPTOR_LAYOUT") ? uint32_t(atoi(getenv("FM2_SMALL_DESCRIPTOR_LAYOUT"))) : 1u << 16;
    array.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo ia{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ia.pNext = &bf;
    ia.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ia.bindingCount = 1;
    ia.pBindings = &array;
    VkDescriptorSetLayout arrayLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(context.device, &ia, nullptr, &arrayLayout) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayout sets[3] = { set0, arrayLayout, arrayLayout };
    VkPushConstantRange push{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 3;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    const bool ok = vkCreatePipelineLayout(context.device, &pli, nullptr, &layout) == VK_SUCCESS;
    vkDestroyDescriptorSetLayout(context.device, arrayLayout, nullptr);
    if (set0Out)
        *set0Out = set0;
    else
        vkDestroyDescriptorSetLayout(context.device, set0, nullptr);
    return ok;
}

uint32_t FindMemory(const Context& context, uint32_t bits, VkMemoryPropertyFlags want);

VkBuffer CreateHostImportedBuffer(const Context& context, void* host, VkDeviceSize size)
{
    if (!context.externalMemoryHost || !vkGetMemoryHostPointerPropertiesEXT)
        return VK_NULL_HANDLE;
    VkMemoryHostPointerPropertiesEXT hostProperties{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if (vkGetMemoryHostPointerPropertiesEXT(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hostProperties) !=
        VK_SUCCESS)
    {
        fprintf(stderr, "vk: the driver refused the host pointer %p\n", host);
        return VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(context.device, &bi, nullptr, &buffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(context.device, buffer, &req);
    VkImportMemoryHostPointerInfoEXT import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = host;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &import;
    ai.allocationSize = req.size;
    const uint32_t compatible = hostProperties.memoryTypeBits & req.memoryTypeBits;
    // Ask for HOST_VISIBLE: the first compatible type may belong to a heap that is not host-visible, and importing
    // a host pointer into that fails with VK_ERROR_OUT_OF_DEVICE_MEMORY.
    const uint32_t candidates = compatible ? compatible : req.memoryTypeBits;
    ai.memoryTypeIndex = FindMemory(context, candidates, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = FindMemory(context, candidates, 0);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult allocResult = VK_SUCCESS;
    if (ai.memoryTypeIndex == UINT32_MAX)
        allocResult = VK_ERROR_INITIALIZATION_FAILED;
    else
        allocResult = vkAllocateMemory(context.device, &ai, nullptr, &memory);
    VkResult bindResult = allocResult == VK_SUCCESS ? vkBindBufferMemory(context.device, buffer, memory, 0) : allocResult;
    if (allocResult != VK_SUCCESS || bindResult != VK_SUCCESS)
    {
        // Name the failing call and its VkResult, and the alignment the extension demands of the pointer
        // (minImportedHostPointerAlignment): a misaligned pointer is rejected before the import is attempted.
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostLimits{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        hostLimits.minImportedHostPointerAlignment = 0;
        VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = &hostLimits;
        if (vkGetPhysicalDeviceProperties2)
            vkGetPhysicalDeviceProperties2(context.physical, &props2);
        fprintf(stderr, "vk: cannot import the host pointer %p as a buffer: alloc %d bind %d size %llu type %u "
            "align %llu (pointer %% 4 = %llu) host bits 0x%x req bits 0x%x\n", host, int(allocResult),
            int(bindResult), (unsigned long long)req.size, ai.memoryTypeIndex,
            (unsigned long long)hostLimits.minImportedHostPointerAlignment,
            (unsigned long long)((uintptr_t)host % 4), hostProperties.memoryTypeBits, int(req.memoryTypeBits));
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(context.device, memory, nullptr);
        vkDestroyBuffer(context.device, buffer, nullptr);
        return VK_NULL_HANDLE;
    }
    return buffer;
}

uint32_t FindMemory(const Context& context, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(context.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

VkBuffer CreateBuffer(const Context& context, VkDeviceSize size, VkBufferUsageFlags usage, void** mapped, VkMemoryPropertyFlags extra)
{
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(context.device, &bi, nullptr, &buffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(context.device, buffer, &req);
    // An unmapped buffer wants device memory for the same reason images do; see CreateImage.
    const VkMemoryPropertyFlags want = mapped ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                              : VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemory(context, req.memoryTypeBits, want | extra);
    if (ai.memoryTypeIndex == UINT32_MAX && extra)
        ai.memoryTypeIndex = FindMemory(context, req.memoryTypeBits, want); // a hint, not a requirement
    if (ai.memoryTypeIndex == UINT32_MAX && !mapped)
        ai.memoryTypeIndex = FindMemory(context, req.memoryTypeBits, 0);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(context.device, &ai, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(context.device, buffer, memory, 0) != VK_SUCCESS)
    {
        vkDestroyBuffer(context.device, buffer, nullptr);
        return VK_NULL_HANDLE;
    }
    if (mapped)
        vkMapMemory(context.device, memory, 0, VK_WHOLE_SIZE, 0, mapped);
    return buffer; // memory is owned by the buffer; freed with it below only in the test
}

namespace
{
// Image memory comes from 64 MB blocks instead of one vkAllocateMemory per image: on the NVIDIA driver each
// allocation costs about a millisecond (17 first-use textures in a frame: 11.9 ms inside CreateImage, against
// 0.03 ms each on RADV). Freed ranges are reused by the next image that fits. FM2_VK_NO_IMAGE_POOL=1 restores a
// dedicated allocation per image.
struct ImagePoolBlock
{
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize size = 0, used = 0;
};
struct ImagePoolFree
{
    uint32_t block;
    VkDeviceSize offset, size;
};
std::vector<ImagePoolBlock> g_imageBlocks;
std::vector<ImagePoolFree> g_imageFree;
constexpr VkDeviceSize kImagePoolBlock = 64ull << 20, kImagePoolLargest = 8ull << 20;

bool ImagePoolAllocate(const Context& context, const VkMemoryRequirements& req, uint32_t type, Image& image)
{
    static const bool disabled = getenv("FM2_VK_NO_IMAGE_POOL") != nullptr;
    if (disabled || req.size > kImagePoolLargest || !req.alignment)
        return false;
    // A freed range first: the smallest one that fits, so sizes that recur (most do) are reused exactly.
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < g_imageFree.size(); i++)
    {
        const ImagePoolFree& range = g_imageFree[i];
        const ImagePoolBlock& block = g_imageBlocks[range.block];
        if (block.device == context.device && block.type == type && range.size >= req.size && range.offset % req.alignment == 0 &&
            (best == SIZE_MAX || range.size < g_imageFree[best].size))
            best = i;
    }
    if (best != SIZE_MAX)
    {
        const ImagePoolFree range = g_imageFree[best];
        g_imageFree.erase(g_imageFree.begin() + ptrdiff_t(best));
        image.memory = g_imageBlocks[range.block].memory;
        image.poolBlock = range.block;
        image.poolOffset = range.offset;
        image.poolSize = range.size;
        return true;
    }
    for (uint32_t i = 0; i <= g_imageBlocks.size(); i++)
    {
        if (i == g_imageBlocks.size())
        {
            ImagePoolBlock block;
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = kImagePoolBlock;
            ai.memoryTypeIndex = type;
            if (vkAllocateMemory(context.device, &ai, nullptr, &block.memory) != VK_SUCCESS)
                return false; // the caller falls back to a dedicated allocation
            block.device = context.device;
            block.type = type;
            block.size = kImagePoolBlock;
            g_imageBlocks.push_back(block);
        }
        ImagePoolBlock& block = g_imageBlocks[i];
        if (block.device != context.device || block.type != type)
            continue;
        const VkDeviceSize offset = (block.used + req.alignment - 1) / req.alignment * req.alignment;
        if (offset + req.size > block.size)
            continue;
        block.used = offset + req.size;
        image.memory = block.memory;
        image.poolBlock = i;
        image.poolOffset = offset;
        image.poolSize = req.size;
        return true;
    }
    return false;
}

void ImagePoolShutdown(const Context& context)
{
    for (ImagePoolBlock& block : g_imageBlocks)
        if (block.device == context.device && block.memory)
        {
            vkFreeMemory(context.device, block.memory, nullptr);
            block = {}; // indices stay valid for other devices' images; a dead block matches no device
        }
    std::erase_if(g_imageFree, [](const ImagePoolFree& range) { return !g_imageBlocks[range.block].memory; });
}
}

Image CreateImage(const Context& context, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
    VkImageViewType viewType, uint32_t layers, VkSampleCountFlagBits samples, uint32_t levels, VkComponentMapping components)
{
    WallMs timed(g_imageCreateMs);
    g_imageCreates++;
    Image image;
    image.format = format;
    image.width = width;
    image.height = height;
    image.levels = levels;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { width, height, 1 };
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (viewType == VK_IMAGE_VIEW_TYPE_CUBE)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(context.device, &ci, nullptr, &image.image) != VK_SUCCESS)
        return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(context.device, image.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    // DEVICE_LOCAL, explicitly. Asking for no properties takes the FIRST compatible type, and on the NVIDIA driver that
    // is type 0: system memory (heap 1, not device-local), which accepts colour and depth images. Every render target,
    // depth buffer and texture would then live in system RAM and be reached over PCIe, at two to three times the frame
    // time. RADV's first type is device-local. FM2_VK_IMAGE_SYSTEM_MEMORY=1 takes the first compatible type instead.
    static const bool systemImages = getenv("FM2_VK_IMAGE_SYSTEM_MEMORY") != nullptr;
    ai.memoryTypeIndex = systemImages ? UINT32_MAX : FindMemory(context, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = FindMemory(context, req.memoryTypeBits, 0);
    if ((!ImagePoolAllocate(context, req, ai.memoryTypeIndex, image) &&
            vkAllocateMemory(context.device, &ai, nullptr, &image.memory) != VK_SUCCESS) ||
        vkBindImageMemory(context.device, image.image, image.memory, image.poolOffset) != VK_SUCCESS)
    {
        DestroyImage(context, image);
        return {};
    }
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = image.image;
    vi.viewType = viewType;
    vi.format = format;
    vi.components = components;
    vi.subresourceRange = { format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                                                  : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
        0, levels, 0, layers };
    if (vkCreateImageView(context.device, &vi, nullptr, &image.view) != VK_SUCCESS)
    {
        DestroyImage(context, image);
        return {};
    }
    if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
    {
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (vkCreateImageView(context.device, &vi, nullptr, &image.attachmentView) != VK_SUCCESS)
        {
            DestroyImage(context, image);
            return {};
        }
    }
    return image;
}

void DestroyImage(const Context& context, Image& image)
{
    UploadsWaitForImage(context, image.image);
    if (image.resolveFramebuffer)
        vkDestroyFramebuffer(context.device, image.resolveFramebuffer, nullptr);
    if (image.view)
        vkDestroyImageView(context.device, image.view, nullptr);
    if (image.attachmentView)
        vkDestroyImageView(context.device, image.attachmentView, nullptr);
    if (image.image)
        vkDestroyImage(context.device, image.image, nullptr);
    if (image.poolBlock != UINT32_MAX)
        g_imageFree.push_back({ image.poolBlock, image.poolOffset, image.poolSize });
    else if (image.memory)
        vkFreeMemory(context.device, image.memory, nullptr);
    image = {};
}

uint64_t g_uploadSubmits = 0, g_uploadBytes = 0, g_uploadFlushes = 0;


namespace
{
struct UploadJob
{
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    bool pending = false;
    std::vector<VkImage> images; // written by this job's command buffer; see UploadsWaitForImage
};
std::vector<UploadJob> g_uploadJobs;
uint32_t g_uploadPending = 0;

struct UploadFenceApi
{
    PFN_vkCreateFence create = nullptr;
    PFN_vkDestroyFence destroy = nullptr;
    PFN_vkResetFences reset = nullptr;
    PFN_vkGetFenceStatus status = nullptr;
    PFN_vkWaitForFences wait = nullptr;
};

// Looked up here rather than through the loader table: same approach as the renderer's frame fences.
const UploadFenceApi& UploadFences(const Context& context)
{
    static UploadFenceApi api;
    static VkInstance loadedFor = VK_NULL_HANDLE;
    if (loadedFor != context.instance)
    {
        loadedFor = context.instance;
        api.create = reinterpret_cast<PFN_vkCreateFence>(vkldr::GetInstanceProcAddr(context.instance, "vkCreateFence"));
        api.destroy = reinterpret_cast<PFN_vkDestroyFence>(vkldr::GetInstanceProcAddr(context.instance, "vkDestroyFence"));
        api.reset = reinterpret_cast<PFN_vkResetFences>(vkldr::GetInstanceProcAddr(context.instance, "vkResetFences"));
        api.status = reinterpret_cast<PFN_vkGetFenceStatus>(vkldr::GetInstanceProcAddr(context.instance, "vkGetFenceStatus"));
        api.wait = reinterpret_cast<PFN_vkWaitForFences>(vkldr::GetInstanceProcAddr(context.instance, "vkWaitForFences"));
    }
    return api;
}

bool UploadFencesUsable(const UploadFenceApi& api)
{
    return api.create && api.destroy && api.reset && api.status && api.wait;
}

// Uploads recorded since the last flush share one command buffer and one staging buffer: a submit per mip costs
// 0.4 ms on RADV even without a queue idle (37 submits, 66 KB: 15 ms of a frame).
UploadJob* g_openJob = nullptr;
VkDeviceSize g_openUsed = 0;
const VkDeviceSize kUploadChunk = 4u << 20;

// A free job whose staging buffer holds `bytes`, or a new one, with its command buffer begun. Never a pending job.
UploadJob* OpenUploadJob(const Context& context, VkDeviceSize bytes)
{
    const UploadFenceApi& api = UploadFences(context);
    // Bound the pool: a burst larger than this waits for the oldest uploads instead of growing without limit.
    CollectUploads(context, g_uploadPending >= 64);
    UploadJob* job = nullptr;
    for (UploadJob& candidate : g_uploadJobs)
        if (!candidate.pending && candidate.capacity >= bytes && (!job || candidate.capacity < job->capacity))
            job = &candidate;
    if (!job)
    {
        UploadJob created;
        VkDeviceSize capacity = kUploadChunk;
        while (capacity < bytes)
            capacity *= 2;
        created.staging = CreateBuffer(context, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &created.mapped);
        if (!created.staging || !created.mapped)
            return nullptr;
        created.capacity = capacity;
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = context.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (api.create(context.device, &fi, nullptr, &created.fence) != VK_SUCCESS ||
            vkAllocateCommandBuffers(context.device, &ai, &created.cb) != VK_SUCCESS)
        {
            if (created.fence)
                api.destroy(context.device, created.fence, nullptr);
            vkDestroyBuffer(context.device, created.staging, nullptr);
            return nullptr;
        }
        g_uploadJobs.push_back(created);
        job = &g_uploadJobs.back();
    }
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(job->cb, 0) != VK_SUCCESS || vkBeginCommandBuffer(job->cb, &bi) != VK_SUCCESS)
        return nullptr;
    return job;
}
}

void FlushUploads(const Context& context)
{
    if (g_beforeSubmit)
        g_beforeSubmit(context);
    if (!g_openJob)
        return;
    UploadJob* job = g_openJob;
    g_openJob = nullptr;
    g_openUsed = 0;
    const UploadFenceApi& api = UploadFences(context);
    VkResult result = vkEndCommandBuffer(job->cb);
    if (result == VK_SUCCESS)
        result = api.reset(context.device, 1, &job->fence);
    if (result == VK_SUCCESS)
    {
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &job->cb;
        result = vkQueueSubmit(context.queue, 1, &si, job->fence);
    }
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vk: upload submit %d\n", int(result));
        job->images.clear();
        return; // never marked pending, so the job stays reusable
    }
    job->pending = true;
    g_uploadPending++;
    g_uploadFlushes++;
}

void CollectUploads(const Context& context, bool waitAll)
{
    if (waitAll)
        FlushUploads(context);
    if (!g_uploadPending)
        return;
    const UploadFenceApi& api = UploadFences(context);
    for (UploadJob& job : g_uploadJobs)
    {
        if (!job.pending)
            continue;
        const VkResult done = waitAll ? api.wait(context.device, 1, &job.fence, VK_TRUE, UINT64_MAX)
                                      : api.status(context.device, job.fence);
        if (done == VK_NOT_READY)
            continue;
        if (done != VK_SUCCESS)
        {
            // Device loss or similar: nothing more will signal, and the wait below is the only safe way out.
            fprintf(stderr, "vk: upload fence %d\n", int(done));
            vkQueueWaitIdle(context.queue);
        }
        job.pending = false;
        job.images.clear();
        g_uploadPending--;
    }
}

void ShutdownUploads(const Context& context)
{
    if (g_uploadJobs.empty())
        return;
    CollectUploads(context, true);
    const UploadFenceApi& api = UploadFences(context);
    for (UploadJob& job : g_uploadJobs)
    {
        if (job.fence && api.destroy)
            api.destroy(context.device, job.fence, nullptr);
        if (job.cb)
            vkFreeCommandBuffers(context.device, context.pool, 1, &job.cb);
        if (job.staging)
            vkDestroyBuffer(context.device, job.staging, nullptr); // its memory is unowned, as with every CreateBuffer
    }
    g_uploadJobs.clear();
    g_uploadPending = 0;
}

// Destroying an image a recorded-but-unfinished upload still writes would leave that command buffer invalid. Only
// then is a wait needed; waiting on every destroy would bring back the stall this code removes.
void UploadsWaitForImage(const Context& context, VkImage image)
{
    if (!image || (!g_openJob && !g_uploadPending))
        return;
    for (const UploadJob& job : g_uploadJobs)
    {
        if (!job.pending && &job != g_openJob)
            continue;
        for (VkImage used : job.images)
            if (used == image)
            {
                CollectUploads(context, true);
                return;
            }
    }
}

bool SubmitUpload(const Context& context, VkImage target, const void* data, VkDeviceSize bytes,
    const std::function<void(VkCommandBuffer, VkBuffer, VkDeviceSize)>& record)
{
    static const bool synchronous = getenv("FM2_VK_SYNC_UPLOADS") != nullptr;
    WallMs timed(g_uploadRecordMs);
    if (!data || !bytes || !UploadFencesUsable(UploadFences(context)))
        return false;
    // Offsets are kept at 16 bytes: a copy's bufferOffset must be a multiple of the texel block size (BC2/BC3: 16).
    const VkDeviceSize aligned = (bytes + 15) & ~VkDeviceSize(15);
    if (g_openJob && g_openUsed + aligned > g_openJob->capacity)
        FlushUploads(context);
    if (!g_openJob)
    {
        g_openJob = OpenUploadJob(context, aligned);
        g_openUsed = 0;
        if (!g_openJob)
        {
            fprintf(stderr, "vk: upload submit: no staging for %llu bytes\n", (unsigned long long)bytes);
            return false;
        }
    }
    const VkDeviceSize offset = g_openUsed;
    memcpy(static_cast<uint8_t*>(g_openJob->mapped) + offset, data, size_t(bytes));
    g_openUsed += aligned;
    if (g_openJob->images.empty() || g_openJob->images.back() != target)
        g_openJob->images.push_back(target);
    record(g_openJob->cb, g_openJob->staging, offset);
    g_uploadSubmits++;
    g_uploadBytes += bytes;
    if (synchronous)
    {
        FlushUploads(context);
        vkQueueWaitIdle(context.queue);
        CollectUploads(context, false);
    }
    return true;
}

void UploadImage(const Context& context, const Image& image, const void* rgba, VkDeviceSize bytes, VkImageLayout oldLayout,
    uint32_t level)
{
    if (level >= image.levels || level >= 32)
        return;
    // Non-blocking: the staging buffer and command buffer belong to a pooled upload job that is recycled once its
    // fence signals (see SubmitUpload), so no queue idle is needed per mip.
    SubmitUpload(context, image.image, rgba, bytes, [&](VkCommandBuffer cb, VkBuffer staging, VkDeviceSize offset) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = oldLayout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
        b.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const uint32_t levelWidth = image.width >> level;
        const uint32_t levelHeight = image.height >> level;
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        copy.imageExtent = { levelWidth ? levelWidth : 1, levelHeight ? levelHeight : 1, 1 };
        vkCmdCopyBufferToImage(cb, staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
}
}
