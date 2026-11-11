// Replays the pipeline states a real game session asked for (FM2_PIPELINE_KEYS=<file>) through
// the Vulkan backend's state translation, with the game's own shaders: the check that the
// register -> pipeline mapping covers what the title actually draws, and what a first start
// costs once all of it has to be built.
//
//   FM2_PIPELINE_KEYS=out/keys.txt ./out/vk_pipelines_test out/keys.txt /tmp/spv3
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_state.h"

namespace
{
std::vector<char> ReadAll(const std::string& path)
{
    std::vector<char> bytes;
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        bytes.resize(size_t(size));
        if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
            bytes.clear();
        fclose(f);
    }
    return bytes;
}

VkShaderModule Module(vk::Context& context, const std::vector<char>& code)
{
    if (code.empty() || (code.size() & 3))
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(context.device, &ci, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: vk_pipelines_test <keys.txt> <spirv dir> [limit]\n");
        return 2;
    }
    std::ifstream keys(argv[1]);
    if (!keys)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    const std::string spvDir = argv[2];
    const size_t limit = argc > 3 ? size_t(atoi(argv[3])) : SIZE_MAX;

    vk::Context context;
    if (!vk::Init(context, "out/vkcache-test"))
        return 1;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!vk::CreateRendererLayout(context, layout))
        return 1;

    // The renderer's main pass: half-float colour with depth-stencil.
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(context.device, &passInfo, nullptr, &pass) != VK_SUCCESS)
        return 1;

    vk::TargetFormats targets;
    size_t total = 0, made = 0, failed = 0, skipped = 0;
    std::set<std::string> states;
    auto start = std::chrono::steady_clock::now();
    std::string line;
    std::vector<VkPipeline> pipelines;
    while (std::getline(keys, line))
    {
        if (line.empty() || total >= limit)
            continue;
        vk::StateKey key{};
        unsigned blend = 0, depth = 0, mask = 0, flags = 0;
        if (sscanf(line.c_str(), "%llx %llx %x %x %x %x", (unsigned long long*)&key.vs, (unsigned long long*)&key.ps, &blend, &depth,
                &mask, &flags) != 6)
            continue;
        key.blend = blend;
        key.depth = depth;
        key.stencilMaskMode = mask;
        key.flags = flags;
        total++;

        char vsPath[512], psPath[512];
        snprintf(vsPath, sizeof vsPath, "%s/vs_%016llx.spv", spvDir.c_str(), (unsigned long long)key.vs);
        snprintf(psPath, sizeof psPath, "%s/ps_%016llx.spv", spvDir.c_str(), (unsigned long long)key.ps);
        VkShaderModule vs = Module(context, ReadAll(vsPath));
        VkShaderModule ps = Module(context, ReadAll(psPath));
        if (!vs || !ps)
        {
            skipped++; // the shader was not in the captured set
            if (vs)
                vkDestroyShaderModule(context.device, vs, nullptr);
            if (ps)
                vkDestroyShaderModule(context.device, ps, nullptr);
            continue;
        }
        char state[32];
        snprintf(state, sizeof state, "%08x %08x %08x", key.blend, key.depth, key.stencilMaskMode);
        states.insert(state);
        VkPipeline pipeline = VK_NULL_HANDLE;
        const char* error = nullptr;
        if (vk::CreatePipeline(context, key, vs, ps, layout, pass, targets, pipeline, &error))
        {
            made++;
            pipelines.push_back(pipeline);
        }
        else
            failed++;
        vkDestroyShaderModule(context.device, vs, nullptr);
        vkDestroyShaderModule(context.device, ps, nullptr);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    printf("vk: %zu states replayed: %zu ok, %zu failed, %zu skipped (shader not captured), %zu distinct render states\n",
        total, made, failed, skipped, states.size());
    printf("vk: %.0f ms total, %.2f ms each\n", ms, made ? ms / made : 0.0);
    for (VkPipeline pipeline : pipelines)
        vkDestroyPipeline(context.device, pipeline, nullptr);
    vkDestroyRenderPass(context.device, pass, nullptr);
    vkDestroyPipelineLayout(context.device, layout, nullptr);
    vk::Shutdown(context);
    return failed ? 2 : 0;
}
