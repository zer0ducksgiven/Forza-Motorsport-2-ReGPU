// Proves the bindless texture path with a real game texture: loads RGBA8 dumped from the
// port's own XDS decode (out/bootscreen/*.png -> raw), uploads it through the texture cache
// the renderer will use, samples it at a bindless index from a shader translated the same way
// the game's shaders are, and compares every pixel of the result with the source.
//
//   convert out/bootscreen/C_MYC_carlist_ForzaLogo.png -depth 8 rgba:out/tex.rgba
//   FM2_TEXTURE=out/tex.rgba FM2_TEXTURE_SIZE=WxH linux/vk.sh --texture
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <iterator>
#include <vector>
#include <filesystem>
#include <fstream>

#include "gpu/texture.h"

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_shader.h"
#include "gpu/vk/vk_texture.h"

namespace
{
const char kQuadVs[] = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);   // full-screen triangle
    VSOut o;
    o.pos = float4(p * 2 - 1, 0, 1);
    o.uv = float4(p.x, p.y, 0, 1);
    return o;
}
)";

// The game's shaders read their textures through an unbounded array, indexed at runtime.
const char kSamplePs[] = R"(
cbuffer PSConstants : register(b1)
{
    float4 pc[256];
    uint4 pb[2];
    uint4 pinfo;
    float4 pinfo2;
    uint4 tinfo[32];
    float4 tbias[32];
};
Texture2D<float4> gtex[] : register(t0, space1);
SamplerState sPoint : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target0
{
    return gtex[1].SampleLevel(sPoint, input.uv.xy, 0);
}
)";

bool LoadRaw(const char* path, size_t bytes, std::vector<uint8_t>& out)
{
    out.resize(bytes);
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    const bool ok = fread(out.data(), 1, bytes, f) == bytes;
    fclose(f);
    return ok;
}

VkShaderModule Module(vk::Context& context, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(context.device, &ci, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}
}

int main(int argc, char** argv)
{
    const char* fixturePath = getenv("FM2_BC_FIXTURE");
    const bool cpuMips = fixturePath && getenv("FM2_CPU_MIPS");
    const uint32_t fixtureSlot = getenv("FM2_BC_SLOT") ? uint32_t(atoi(getenv("FM2_BC_SLOT"))) : 4;
    TextureFetch fixture{};
    std::vector<TextureFetch> chain;
    std::vector<std::vector<uint8_t>> blocks;
    if (fixturePath)
    {
        char name[32];
        snprintf(name, sizeof name, "tex_%u.fetch", fixtureSlot);
        std::ifstream fetchFile(std::filesystem::path(fixturePath) / name, std::ios::binary);
        if (!fetchFile.read(reinterpret_cast<char*>(&fixture), sizeof fixture))
            return 2;
        snprintf(name, sizeof name, "tex_%u.bin", fixtureSlot);
        std::ifstream sourceFile(std::filesystem::path(fixturePath) / name, std::ios::binary | std::ios::ate);
        if (!sourceFile)
            return 2;
        std::vector<uint8_t> guest(size_t(sourceFile.tellg()));
        sourceFile.seekg(0);
        if (!sourceFile.read(reinterpret_cast<char*>(guest.data()), std::streamsize(guest.size())) || guest.size() < TextureMipSpan(fixture))
            return 2;
        chain = { fixture };
        for (uint32_t level = 1;; level++)
        {
            TextureFetch next;
            if (!TextureMipLevel(fixture, level, next))
                break;
            chain.push_back(next);
        }
        const uint32_t kind = TextureBCKind(fixture.Format());
        if (kind < 1 || kind > 4)
            return 2;
        const uint32_t blockBytes = kind == 1 || kind == 4 ? 8 : 16;
        for (const TextureFetch& fetch : chain)
        {
            const uint32_t pitch = fetch.Width() / 4 * blockBytes;
            std::vector<uint8_t> level(cpuMips ? 0 : size_t(pitch) * (fetch.Height() / 4));
            if (cpuMips ? !DecodeTexture(fetch, guest.data(), level) : !CopyTextureBlocks(fetch, guest.data(), level.data(), pitch))
                return 2;
            blocks.push_back(std::move(level));
        }
    }
    const char* rawPath = getenv("FM2_TEXTURE");
    const char* sizeText = getenv("FM2_TEXTURE_SIZE");
    if (!fixturePath && (!rawPath || !sizeText))
    {
        fprintf(stderr, "set FM2_TEXTURE=<rgba file> and FM2_TEXTURE_SIZE=<w>x<h>\n");
        return 2;
    }
    const uint32_t sampleLevel = getenv("FM2_BC_LEVEL") ? uint32_t(atoi(getenv("FM2_BC_LEVEL"))) : 0;
    if (fixturePath && sampleLevel >= chain.size())
        return 2;
    const TextureFetch sampleFetch = fixturePath ? chain[sampleLevel] : fixture;
    uint32_t width = fixturePath ? sampleFetch.Width() : 0, height = fixturePath ? sampleFetch.Height() : 0;
    if (!fixturePath && (sscanf(sizeText, "%ux%u", &width, &height) != 2 || !width || !height))
    {
        fprintf(stderr, "FM2_TEXTURE_SIZE must be WxH\n");
        return 2;
    }
    std::vector<uint8_t> source;
    if (fixturePath)
    {
        std::ifstream guestFile(std::filesystem::path(fixturePath) / ("tex_" + std::to_string(fixtureSlot) + ".bin"), std::ios::binary);
        std::vector<uint8_t> guest{ std::istreambuf_iterator<char>(guestFile), {} };
        if (!DecodeTexture(sampleFetch, guest.data(), source))
            return 2;
    }
    else if (!LoadRaw(rawPath, size_t(width) * height * 4, source))
    {
        fprintf(stderr, "cannot read %s (%ux%u RGBA)\n", rawPath, width, height);
        return 2;
    }

    const char* d3dReference = getenv("FM2_D3D_TEXELS");
    if (d3dReference)
    {
        std::vector<uint8_t> texels;
        if (!LoadRaw(d3dReference, size_t(width) * height, texels))
        {
            fprintf(stderr, "cannot read the D3D12 reference texels (%ux%u, one byte per texel)\n", width, height);
            return 2;
        }
        for (size_t i = 0; i < texels.size(); i++)
            source[i * 4] = source[i * 4 + 1] = source[i * 4 + 2] = source[i * 4 + 3] = texels[i];
    }
    const uint32_t tolerance = d3dReference ? 0u : fixturePath ? 4u : 0u;
    const bool floatReadback = getenv("FM2_FLOAT_READBACK") != nullptr;
    const VkFormat outputFormat = floatReadback ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    vk::Context context;
    if (!vk::Init(context, "out/vkcache-test"))
        return 1;
    printf("vk: %s, texture %ux%u from %s\n", context.props.deviceName, width, height, rawPath);

    printf("vk: sampled image limits %u/%u, allocations %u\n", context.props.limits.maxDescriptorSetSampledImages,
        context.props.limits.maxPerStageDescriptorSampledImages, context.props.limits.maxMemoryAllocationCount);
    const uint32_t expectedIndex = getenv("FM2_BC_INDEX") ? uint32_t(atoi(getenv("FM2_BC_INDEX"))) : 1;
    vk::TextureCache textures;
    if (!vk::TextureCacheInit(context, textures, std::max(1024u, expectedIndex + 1)))
    {
        fprintf(stderr, "vk: texture cache init failed\n");
        return 1;
    }
    // Index 0 is never used by the shader (it samples index 1): add a dummy first, the way the
    // port reserves its null descriptors.
    std::vector<uint8_t> dummy(4 * 4 * 4, 0);
    vk::TextureAdd(context, textures, 4, 4, dummy.data());
    for (uint32_t i = 1; i < expectedIndex; i++)
        if (vk::TextureAdd(context, textures, 4, 4, dummy.data()) != i)
        {
            fprintf(stderr, "vk: placeholder %u failed\n", i);
            return 1;
        }
    printf("vk: placeholders ready\n");
    const uint32_t kind = fixturePath ? TextureBCKind(fixture.Format()) : 0;
    const VkFormat bcFormat = kind == 1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : kind == 2 ? VK_FORMAT_BC2_UNORM_BLOCK : kind == 3 ? VK_FORMAT_BC3_UNORM_BLOCK
        : VK_FORMAT_BC4_UNORM_BLOCK;
    const uint32_t index = fixturePath && !cpuMips
        ? vk::TextureAddBlocks(context, textures, fixture.Width(), fixture.Height(), bcFormat, blocks[0].data(), width / 4 * (kind == 1 || kind == 4 ? 8 : 16), 0,
              uint32_t(blocks.size()))
        : vk::TextureAdd(context, textures, fixturePath ? fixture.Width() : width, fixturePath ? fixture.Height() : height,
              cpuMips ? blocks[0].data() : source.data(), 0, cpuMips ? uint32_t(blocks.size()) : 1);
    if (fixturePath)
        for (uint32_t level = 1; level < blocks.size(); level++)
            if (cpuMips
                    ? !vk::TextureReplace(context, textures, index, fixture.Width(), fixture.Height(), blocks[level].data(),
                          uint32_t(blocks.size()), level, VK_IMAGE_LAYOUT_UNDEFINED)
                    : !vk::TextureReplaceBlocks(context, textures, index, fixture.Width(), fixture.Height(), bcFormat, blocks[level].data(),
                          (fixture.Width() >> level) / 4 * (kind == 1 || kind == 4 ? 8 : 16), uint32_t(blocks.size()), level, VK_IMAGE_LAYOUT_UNDEFINED))
            {
                printf("vk: mip replacement %u failed\n", level);
                return 1;
            }
    if (index != expectedIndex)
    {
        fprintf(stderr, "unexpected bindless index %u\n", index);
        return 1;
    }

    std::string samplePs = kSamplePs;
    if (getenv("FM2_LOAD_TEXEL"))
    {
        const size_t at = samplePs.find("return gtex[1].SampleLevel");
        if (at == std::string::npos)
        {
            fprintf(stderr, "vk: texel-load probe marker missing\n");
            return 1;
        }
        const std::string load = "return gtex[1].Load(int3(int2(input.uv * float2(" + std::to_string(width) + ", " +
            std::to_string(height) + ")), 0));";
        const size_t end = samplePs.find(';', at);
        if (end == std::string::npos)
        {
            fprintf(stderr, "vk: texel-load probe statement missing at %zu: %s\n", at, samplePs.c_str());
            return 1;
        }
        samplePs.replace(at, end - at + 1, load);
    }
    if (getenv("FM2_LINEAR_CLAMP"))
        samplePs.replace(samplePs.find("register(s0)"), strlen("register(s0)"), "register(s1)");
    const std::string selector = getenv("FM2_DYNAMIC_INDEX") ? "gtex[NonUniformResourceIndex(asuint(tbias[1].y))]" : "gtex[" + std::to_string(expectedIndex) + "]";
    if (!getenv("FM2_INDEX_PROBE"))
        samplePs.replace(samplePs.find("gtex[1]"), 7, selector);
    if (getenv("FM2_INDEX_PROBE") && !getenv("FM2_LOAD_TEXEL"))
        samplePs.replace(samplePs.find("return gtex[1].SampleLevel(sPoint, input.uv.xy, 0);"),
            strlen("return gtex[1].SampleLevel(sPoint, input.uv.xy, 0);"),
            "return float4(pc[0].x, asuint(tbias[1].y) / 2048.0, 0, 1);");
    const std::string level = std::to_string(sampleLevel);
    if (!getenv("FM2_INDEX_PROBE"))
    {
        const char* marker = getenv("FM2_LOAD_TEXEL") ? ", 0));" : ", 0);";
        const std::string replacement = ", " + level + (getenv("FM2_LOAD_TEXEL") ? "));" : ");");
        samplePs.replace(samplePs.find(marker), strlen(marker), replacement);
    }
    printf("vk: texture setup index %u\n", index);
    std::vector<uint32_t> vs, ps;
    std::string error;
    if (!vk::CompileHlsl(kQuadVs, false, vk::HashSource(kQuadVs), "out/vkcache", vs, &error) ||
        !vk::CompileHlsl(samplePs, true, vk::HashSource(samplePs), "out/vkcache", ps, &error))
    {
        fprintf(stderr, "vk: shader: %s\n", error.c_str());
        return 1;
    }

    // The game's layout: set 0 has the constant buffers, guest memory and samplers, set 1 the
    // bindless 2D array (the cache owns that one).
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE;
    if (!vk::CreateRendererLayout(context, pipelineLayout, &set0Layout))
        return 1;

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = samplerInfo.minFilter = getenv("FM2_LINEAR_CLAMP") ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 32.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(context.device, &samplerInfo, nullptr, &sampler);

    // Set 0 carries the constant buffers and guest memory, which this pass does not read: fill
    // them with the dummy buffer the way the renderer's null bindings do.
    void* scratch = nullptr;
    VkBuffer dummyBuffer = vk::CreateBuffer(context, 65536,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratch);
    if (scratch)
    {
        const float one = 1.0f;
        memcpy(scratch, &one, sizeof one);
        memcpy(static_cast<uint8_t*>(scratch) + 4692, &expectedIndex, sizeof expectedIndex);
        printf("vk: constant index %u\n", *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(scratch) + 4692));
    }
    VkDescriptorPoolSize poolSizes[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 4 } };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &pool);
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set0Layout;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(context.device, &allocate, &set0);
    VkDescriptorBufferInfo bufferInfo{ dummyBuffer, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo samplerDesc{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[8]{};
    for (int i = 0; i < 8; i++)
    {
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[i].dstSet = set0;
        writes[i].dstBinding = i < 4 ? (i < 2 ? uint32_t(i) : uint32_t(100 + i - 2)) : 200u;
        writes[i].dstArrayElement = i < 4 ? 0u : uint32_t(i - 4);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            : (i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLER);
        if (i < 4)
            writes[i].pBufferInfo = &bufferInfo;
        else
            writes[i].pImageInfo = &samplerDesc;
    }
    vkUpdateDescriptorSets(context.device, 8, writes, 0, nullptr);

    // Render the texture 1:1 into an image of its own size.
    VkAttachmentDescription color{};
    color.format = outputFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &color;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    vkCreateRenderPass(context.device, &renderPassInfo, nullptr, &pass);

    VkShaderModule vsModule = Module(context, vs);
    VkShaderModule psModule = Module(context, ps);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = psModule;
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;
    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        fprintf(stderr, "vk: pipeline failed\n");
        return 1;
    }

    vk::Image target = vk::CreateImage(context, width, height, outputFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebufferInfo.renderPass = pass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &target.view;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    vkCreateFramebuffer(context.device, &framebufferInfo, nullptr, &framebuffer);

    void* readback = nullptr;
    VkBuffer copy = vk::CreateBuffer(context, VkDeviceSize(width) * height * (floatReadback ? 16 : 4), VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
    const bool ok = vk::Submit(context, [&](VkCommandBuffer cb) {
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = { { 0, 0 }, { width, height } };
        vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDescriptorSet sets[2] = { set0, textures.set };
        const uint32_t offsets[2] = { 0, 0 }; // dynamic constants: unused by this pass
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 2, offsets);
        VkViewport v{ 0, 0, float(width), float(height), 0, 1 };
        VkRect2D s{ { 0, 0 }, { width, height } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &s);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, 1, &region);
    });
    if (!ok)
    {
        fprintf(stderr, "vk: draw failed\n");
        return 1;
    }
    size_t different = 0;
    uint32_t worst = 0;
    std::vector<uint8_t> quantized;
    if (floatReadback)
    {
        const float* values = static_cast<const float*>(readback);
        quantized.resize(size_t(width) * height * 4);
        for (size_t i = 0; i < quantized.size(); i++)
            quantized[i] = uint8_t(values[i] * 255.0f + 0.5f);
        if (width * height <= 16)
            for (uint32_t i = 0; i < width * height; i++)
                printf("vk: texel %u %.9g (%u)\n", i, values[i * 4], quantized[i * 4]);
    }
    const uint8_t* result = floatReadback ? quantized.data() : static_cast<const uint8_t*>(readback);
    for (size_t i = 0; i < size_t(width) * height; i++)
    {
        uint32_t error = 0;
        for (uint32_t c = 0; c < 4; c++)
        {
            const uint32_t d = result[i * 4 + c] > source[i * 4 + c] ? result[i * 4 + c] - source[i * 4 + c]
                : source[i * 4 + c] - result[i * 4 + c];
            error = std::max(error, d);
        }
        worst = std::max(worst, error);
        if (error > tolerance && different < 16)
            fprintf(stderr, "vk: mismatch at %zu,%zu: %u -> %u\n", i % width, i / width, source[i * 4], result[i * 4]);
        different += error > tolerance;
    }
    printf("vk: first source/result %u,%u,%u,%u / %u,%u,%u,%u; centre %u,%u,%u,%u / %u,%u,%u,%u\n",
        source[0], source[1], source[2], source[3], result[0], result[1], result[2], result[3],
        source[(size_t(height / 2) * width + width / 2) * 4], source[(size_t(height / 2) * width + width / 2) * 4 + 1],
        source[(size_t(height / 2) * width + width / 2) * 4 + 2], source[(size_t(height / 2) * width + width / 2) * 4 + 3],
        result[(size_t(height / 2) * width + width / 2) * 4], result[(size_t(height / 2) * width + width / 2) * 4 + 1],
        result[(size_t(height / 2) * width + width / 2) * 4 + 2], result[(size_t(height / 2) * width + width / 2) * 4 + 3]);
    printf("vk: bindless index %u, %zu of %u pixels differ from the source texture; worst channel %u\n", index, different,
        width * height, worst);
    vkDeviceWaitIdle(context.device);
    vk::Shutdown(context);
    return different ? 2 : 0;
}
