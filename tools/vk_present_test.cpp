// Runs the native renderer's first pass headless: composes an image on the CPU (the shape of
// the port's boot screen and held frame), draws it through the Vulkan backend's full-screen
// pipeline, reads the result back and writes it as a PNG. No window, no swap chain, so it
// runs anywhere:
//
//   linux/vk.sh            # builds and runs it, writes out/vk_present.png
//
// The pipeline here is the one the presenter will use; the next step is the PM4 draw
// translation and the EDRAM surface model behind the same device.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_blit.h"
#include "gpu/vk/vk_context.h"
#include "png.h"

int main(int argc, char** argv)
{
    const uint32_t width = 1280, height = 720;

    vk::Context context;
    if (!vk::Init(context, "out/vkcache-test"))
        return 1;
    printf("vk: %s\n", context.props.deviceName);
    printf("vk: maxStorageBufferRange %u MB, descriptor indexing %s\n",
        uint32_t(context.props.limits.maxStorageBufferRange >> 20), context.descriptorIndexing ? "yes" : "no");

    vk::Blit blit;
    if (!vk::BlitInit(context, blit, "out/vkcache"))
        return 1;

    // What the port's boot screen looks like: a flat background with a progress band.
    std::vector<uint8_t> source(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint8_t* p = &source[(size_t(y) * width + x) * 4];
            const bool band = y > height / 2 - 12 && y < height / 2 + 12 && x > width / 8 && x < width * 7 / 8;
            const bool fill = band && x < width / 8 + (width * 3 / 4) * 2 / 5;
            p[0] = fill ? 230 : (band ? 60 : uint8_t(x * 255 / width));
            p[1] = fill ? 160 : (band ? 60 : uint8_t(y * 255 / height));
            p[2] = fill ? 40 : 70;
            p[3] = 255;
        }

    std::vector<uint8_t> result;
    if (!vk::BlitImage(context, blit, width, height, source, result))
    {
        fprintf(stderr, "vk: blit failed\n");
        return 1;
    }

    size_t different = 0, flipped = 0, lit = 0;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            const uint8_t* a = &source[(size_t(y) * width + x) * 4];
            const uint8_t* b = &result[(size_t(y) * width + x) * 4];
            const uint8_t* f = &source[(size_t(height - 1 - y) * width + x) * 4];
            const bool same = a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
            const bool sameFlipped = f[0] == b[0] && f[1] == b[1] && f[2] == b[2];
            different += !same;
            flipped += !sameFlipped;
            lit += (b[0] + b[1] + b[2]) > 24;
        }
    printf("vk: %zu of %u pixels differ (%zu if the image were flipped), %zu lit\n", different, width * height, flipped, lit);

    WritePng(argc > 1 ? argv[1] : "out/vk_present.png", width, height, result);
    vk::BlitShutdown(context, blit);

    // Pipeline creation with the game's real SPIR-V modules: the cost that decides how long a
    // first start takes, and whether the on-disk cache brings later starts down.
    if (argc > 2)
    {
        std::vector<std::string> vsFiles, psFiles;
        if (DIR* dir = opendir(argv[2]))
        {
            while (dirent* entry = readdir(dir))
            {
                const std::string name = entry->d_name;
                if (name.size() < 4 || name.compare(name.size() - 4, 4, ".spv"))
                    continue;
                if (name.rfind("vs_", 0) == 0)
                    vsFiles.push_back(std::string(argv[2]) + "/" + name);
                else if (name.rfind("ps_", 0) == 0)
                    psFiles.push_back(std::string(argv[2]) + "/" + name);
            }
            closedir(dir);
        }
        const size_t limit = argc > 3 ? size_t(atoi(argv[3])) : 200;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (!vsFiles.empty() && !psFiles.empty() && vk::CreateRendererLayout(context, layout))
        {
            auto load = [](const std::string& path, std::vector<uint32_t>& out) {
                FILE* f = fopen(path.c_str(), "rb");
                if (!f)
                    return false;
                fseek(f, 0, SEEK_END);
                const long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                out.resize(size_t(size) / 4);
                const bool ok = fread(out.data(), 1, size_t(size), f) == size_t(size);
                fclose(f);
                return ok;
            };
            auto module = [&](const std::vector<uint32_t>& code) {
                VkShaderModule m = VK_NULL_HANDLE;
                VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                ci.codeSize = code.size() * 4;
                ci.pCode = code.data();
                return vkCreateShaderModule(context.device, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
            };
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].pName = "main";
            stages[1] = stages[0];
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blendState{};
            blendState.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            colorBlend.attachmentCount = 1;
            colorBlend.pAttachments = &blendState;
            VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamic;
            VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pci.stageCount = 2;
            pci.pStages = stages;
            pci.pVertexInputState = &vertexInput;
            pci.pInputAssemblyState = &assembly;
            pci.pViewportState = &viewport;
            pci.pRasterizationState = &raster;
            pci.pMultisampleState = &multisample;
            pci.pColorBlendState = &colorBlend;
            pci.pDynamicState = &dynamicState;
            pci.layout = layout;
            pci.renderPass = blit.pass;

            const size_t n = std::min({ vsFiles.size(), psFiles.size(), limit });
            auto start = std::chrono::steady_clock::now();
            size_t made = 0, failed = 0;
            std::vector<VkPipeline> pipelines;
            for (size_t i = 0; i < n; i++)
            {
                std::vector<uint32_t> vsCode, psCode;
                if (!load(vsFiles[i], vsCode) || !load(psFiles[i], psCode))
                {
                    failed++;
                    continue;
                }
                stages[0].module = module(vsCode);
                stages[1].module = module(psCode);
                VkPipeline pipeline = VK_NULL_HANDLE;
                const VkResult r = vkCreateGraphicsPipelines(context.device, context.pipelineCache, 1, &pci, nullptr, &pipeline);
                if (r == VK_SUCCESS)
                {
                    made++;
                    pipelines.push_back(pipeline);
                }
                else
                    failed++;
                vkDestroyShaderModule(context.device, stages[0].module, nullptr);
                vkDestroyShaderModule(context.device, stages[1].module, nullptr);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            printf("vk: %zu game pipelines: %zu ok, %zu failed, %.0f ms (%.2f ms each)\n", n, made, failed, ms,
                made ? ms / made : 0.0);
            for (VkPipeline pipeline : pipelines)
                vkDestroyPipeline(context.device, pipeline, nullptr);
            vkDestroyPipelineLayout(context.device, layout, nullptr);
        }
    }
    vk::Shutdown(context);
    return different ? 2 : 0;
}
