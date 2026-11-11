// Drives the Vulkan backend's per-draw submission the way the renderer will: build a
// VSConstants/PSConstants block, an expanded index list, bind the chain and draw — then check
// the result pixel by pixel. Covers what a draw needs besides the device and the shaders:
// the constant ring, index ring and bindless texture set. The texture setup also covers a mip replacement.
//
//   ./out/vk_draw_test                      # DrawIndexed
//   FM2_RING_FETCH=1 ./out/vk_draw_test     # DrawList, index list and vertices read from the upload ring
//   FM2_DIRECT_INDEX_TEST=1 ./out/vk_draw_test   # DrawList without an index list
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/vk/vk_loader.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_draw.h"
#include "gpu/vk/vk_shader.h"
#include "gpu/vk/vk_state.h"
#include "gpu/vk/vk_texture.h"

namespace
{
// Reads the constant blocks exactly as the game's shaders do: vc[128] holds the quad's
// transform, gtex[0] the texture.
const char kQuadVs[] = R"(
cbuffer VSConstants : register(b0)
{
    float4 vc[256];
    uint4 vb[2];
    uint4 vf[96];
    uint4 vinfo;
    float4 vpoint;
    float4 vscale;
    float4 voffset;
    uint4 vte;
    uint4 vfl[32];
    float4 vsquash[4];
    float4 vwide;
    float4 vjitter;
};

struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

// Vertex pull, like the translated shaders: the vertex stream lives in guest memory.
ByteAddressBuffer gphys : register(t1);

VSOut main(uint id : SV_VertexID)
{
    uint base = asuint(vc[128].x);
    uint index = id;
    float2 p = asfloat(gphys.Load2(base + index * 8));
    VSOut o;
    o.pos = float4(p * vc[128].y + vc[128].zw, 0, 1);
    o.uv = float4(p * 0.5 + 0.5, 0, 1);
    return o;
}
)";

const char kSamplePs[] = R"(
cbuffer PSConstants : register(b1)
{
    float4 pc[256];
    uint4 pb[2];
    uint4 pinfo;
    float4 pinfo2;
    uint4 tinfo[32];
    uint4 tbias[32];
};
[nonuniform] Texture2D<float4> gtex[] : register(t0, space1);
[nonuniform] TextureCube<float4> gcube[] : register(t0, space2);
struct Push { uint index; }; [[vk::push_constant]] Push push;
SamplerState sPoint : register(s0);

float4 SampleBindless(uint index, float2 uv)
{
    if (tinfo[0].w == 3)
        return gcube[index].SampleLevel(sPoint, float3(1, 0, 0), 0);
    return gtex[index].SampleLevel(sPoint, uv, 0);
}

struct VSOut
{
    float4 pos : SV_Position;
    float4 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target0
{
    return gtex[166].Sample(sPoint, input.uv.xy) * pc[64];
}
)";

VkShaderModule Module(vk::Context& context, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(context.device, &ci, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}
}

int main()
{
    const uint32_t width = 320, height = 200;
    std::vector<uint8_t> source(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint8_t* p = &source[(size_t(y) * width + x) * 4];
            const bool on = ((x / 16) + (y / 16)) & 1;
            p[0] = on ? 220 : 30;
            p[1] = uint8_t(255 * x / width);
            p[2] = on ? 60 : 30;
            p[3] = 255;
        }

    vk::Context context;
    if (!vk::Init(context, getenv("FM2_TEST_NO_CACHE") ? "" : "out/vkcache"))
        return 1;
    printf("vk: %s\n", context.props.deviceName);

    // Guest memory, as the renderer maps it: vertex data at an offset inside it.
    void* gphysCpu = nullptr;
    VkBuffer gphys = vk::CreateBuffer(context, 1ull << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &gphysCpu);
    void* gmemCpu = nullptr;
    VkBuffer gmem = vk::CreateBuffer(context, 1ull << 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &gmemCpu);
    const uint32_t vertexBase = 65536;
    const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    const float expanded[12] = { -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1 };
    const bool directIndex = getenv("FM2_DIRECT_INDEX_TEST") != nullptr;
    const float* vertices = directIndex ? expanded : quad;
    const size_t vertexBytes = directIndex ? sizeof expanded : sizeof quad;
    memcpy(static_cast<uint8_t*>(gphysCpu) + vertexBase, vertices, vertexBytes);

    vk::TextureCache textures;
    if (!vk::TextureCacheInit(context, textures, getenv("FM2_LARGE_CACHE") ? uint32_t(atoi(getenv("FM2_LARGE_CACHE"))) : 1024))
        return 1;
    const uint8_t white[4] = { 255, 255, 255, 255 };
    for (uint32_t i = 0; i < 166; i++)
        if (vk::TextureAdd(context, textures, 1, 1, white) != i)
            return 1;
    const uint32_t mipCount = 2;
    const uint32_t index = vk::TextureAdd(context, textures, width, height, source.data(), 0, mipCount);
    if (index != 166 || textures.slots[index].levels != mipCount)
    {
        fprintf(stderr, "unexpected bindless index or mip count %u\n", index);
        return 1;
    }
    if (!vk::TextureReplace(context, textures, index, width, height, source.data(), mipCount))
        return 1;
    const uint32_t mipWidth = width / 2, mipHeight = height / 2;
    std::vector<uint8_t> mip(size_t(mipWidth) * mipHeight * 4, 0x5A);
    vk::UploadImage(context, textures.slots[index], mip.data(), mip.size(), VK_IMAGE_LAYOUT_UNDEFINED, 1);
    vk::UploadImage(context, textures.slots[index], mip.data(), mip.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

    vk::TextureCache cubeTextures;
    if (!vk::TextureCacheInit(context, cubeTextures, 4, true))
        return 1;
    const uint8_t blank[4] = {};
    const uint8_t* blanks[6] = { blank, blank, blank, blank, blank, blank };
    const uint32_t cubeIndex = vk::TextureAddCube(context, cubeTextures, 1, 1, blanks);
    vk::Image cubeFaces[6];
    const vk::Image* cubeFacePointers[6]{};
    uint8_t facePixels[6][4]{};
    for (uint32_t face = 0; face < 6; face++)
    {
        facePixels[face][0] = uint8_t(face + 1);
        facePixels[face][1] = uint8_t(11 + face);
        facePixels[face][2] = uint8_t(21 + face);
        facePixels[face][3] = 255;
        cubeFaces[face] = vk::CreateImage(context, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        vk::UploadImage(context, cubeFaces[face], facePixels[face], 4, VK_IMAGE_LAYOUT_UNDEFINED);
        cubeFacePointers[face] = &cubeFaces[face];
    }
    void* cubeReadback = nullptr;
    VkBuffer cubeCopy = vk::CreateBuffer(context, 6 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &cubeReadback);
    bool copied = false;
    const bool cubeOk = vk::Submit(context, [&](VkCommandBuffer cb) {
        copied = vk::TextureCopyCube(context, cubeTextures, cubeIndex, cb, cubeFacePointers, 1, 1);
        VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        read.image = cubeTextures.slots[cubeIndex].image;
        read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &read);
        for (uint32_t face = 0; face < 6; face++)
        {
            VkBufferImageCopy region{};
            region.bufferOffset = face * 4;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
            region.imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer(cb, cubeTextures.slots[cubeIndex].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cubeCopy, 1, &region);
        }
    });
    size_t cubeDiffer = copied && cubeOk ? 0 : 1;
    if (!cubeDiffer)
        for (uint32_t face = 0; face < 6; face++)
            cubeDiffer += memcmp(static_cast<const uint8_t*>(cubeReadback) + face * 4, facePixels[face], 4) != 0;
    printf("vk: resolved cube copy: %zu of 6 faces differ\n", cubeDiffer);
    const uint32_t chainIndex = vk::TextureReserve(context, textures, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    const vk::Image* mipLevels[6] = { &cubeFaces[0], &cubeFaces[1] };
    void* mipReadback = nullptr;
    VkBuffer mipCopy = vk::CreateBuffer(context, 2 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mipReadback);
    bool mipCopied = false;
    const bool mipOk = vk::Submit(context, [&](VkCommandBuffer cb) {
        mipCopied = vk::TextureCopyMipChain(context, textures, chainIndex, cb, mipLevels, 2, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        VkImageMemoryBarrier read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        read.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        read.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        read.srcQueueFamilyIndex = read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        read.image = textures.slots[chainIndex].image;
        read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &read);
        for (uint32_t level = 0; level < 2; level++)
        {
            VkBufferImageCopy region{};
            region.bufferOffset = level * 4;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            region.imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer(cb, textures.slots[chainIndex].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mipCopy, 1, &region);
        }
    });
    size_t mipDiffer = mipCopied && mipOk ? 0 : 1;
    if (!mipDiffer)
        for (uint32_t level = 0; level < 2; level++)
            mipDiffer += memcmp(static_cast<const uint8_t*>(mipReadback) + level * 4, facePixels[level], 4) != 0;
    printf("vk: resolved mip chain: %zu of 2 levels differ\n", mipDiffer);
    vkDeviceWaitIdle(context.device);
    vkDestroyBuffer(context.device, mipCopy, nullptr);
    vkDestroyBuffer(context.device, cubeCopy, nullptr);
    for (vk::Image& face : cubeFaces)
        vk::DestroyImage(context, face);
    vk::TextureCacheShutdown(context, cubeTextures);

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(context.device, &samplerInfo, nullptr, &sampler);
    VkSampler samplers[4] = { sampler, sampler, sampler, sampler };

    vk::DrawContext draw;
    if (!vk::DrawInit(context, draw, gphys, samplers, textures.set, cubeTextures.set, 4u << 20, 4u << 20))
    {
        fprintf(stderr, "vk: draw initialization failed\n");
        return 1;
    }
    std::string vertexShader = kQuadVs;
    const bool ringFetch = getenv("FM2_RING_FETCH") != nullptr;
    if (ringFetch)
    {
        for (size_t at = 0; (at = vertexShader.find("gphys", at)) != std::string::npos;)
            vertexShader.replace(at, 5, "gmem");
        vertexShader.replace(vertexShader.find("uint index = id;"), strlen("uint index = id;"),
            "uint index = gmem.Load(vinfo.x + id * 4);");
    }
    std::string samplePs = kSamplePs;
    if (getenv("FM2_TEST_WIDE_IO"))
    {
        for (std::string* source : { &vertexShader, &samplePs })
        {
            source->replace(source->find("float4 uv : TEXCOORD0;"), strlen("float4 uv : TEXCOORD0;"), "float4 values[16] : TEXCOORD0;");
            for (size_t at = 0; (at = source->find("o.uv", at)) != std::string::npos;)
                source->replace(at, 4, "o.values[0]");
            for (size_t at = 0; (at = source->find("input.uv", at)) != std::string::npos;)
                source->replace(at, 8, "input.values[0]");
        }
        vertexShader.replace(vertexShader.find("    return o;"), strlen("    return o;"),
            "    [unroll] for (int j = 1; j < 16; j++) o.values[j] = float4(j, j + 1, j + 2, 1);\n    return o;");
        samplePs.replace(samplePs.find("    return gtex[166].Sample(sPoint, input.values[0].xy) * pc[64];"),
            strlen("    return gtex[166].Sample(sPoint, input.values[0].xy) * pc[64];"),
            "    float4 unused = 0;\n    [unroll] for (int j = 1; j < 16; j++) unused += input.values[j];\n"
            "    return gtex[166].Sample(sPoint, input.values[0].xy) * pc[64] + unused * 1e-30;");
    }
    if (getenv("FM2_INDEX_PROBE"))
        samplePs.replace(samplePs.find("return gtex[166]"), strlen("return gtex[166].Sample(sPoint, input.uv.xy) * pc[64];"),
            "return float4(tbias[0].y / 166.0, float(asuint(tbias[0].y)) / 1126307840.0, 0, 1);");
    else if (getenv("FM2_GET_DIMENSIONS"))
        samplePs.replace(samplePs.find("return gtex[166]"), strlen("return gtex[166].Sample(sPoint, input.uv.xy) * pc[64];"),
            "uint w, h, levels; gtex[asuint(tbias[0].y)].GetDimensions(0, w, h, levels); return float4(w / 320.0, h / 200.0, levels / 2.0, 1);");
    else if (getenv("FM2_DYNAMIC_INDEX"))
        samplePs.replace(samplePs.find("return gtex[166]"), strlen("return gtex[166].Sample(sPoint, input.uv.xy) * pc[64];"),
            "return SampleBindless(tbias[0].y, input.uv.xy) * pc[64];");
    std::vector<uint32_t> vsCode, psCode;
    std::string error;
    if (!vk::CompileHlsl(vertexShader, false, vk::HashSource(vertexShader), getenv("FM2_TEST_NO_CACHE") ? std::filesystem::path() : "out/vkcache",
            vsCode, &error) ||
        !vk::CompileHlsl(samplePs, true, vk::HashSource(samplePs), getenv("FM2_TEST_NO_CACHE") ? std::filesystem::path() : "out/vkcache", psCode,
            &error))
    {
        fprintf(stderr, "vk: shader: %s\n", error.c_str());
        return 1;
    }
    // Target: one colour image, the way a minimal EDRAM surface will look.
    vk::Image target = vk::CreateImage(context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &color;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    vkCreateRenderPass(context.device, &passInfo, nullptr, &pass);

    vk::StateKey key; // opaque blend, no depth: the state a UI draw uses
    key.blend = 0x00010001;           // src = ONE, dst = ZERO for colour and alpha: opaque
    key.stencilMaskMode = 0x0F << 24; // colour write mask (RB_COLOR_MASK = 0xF): no mask, no pixels
    key.flags = 2;                    // cullable, cull mode 0 (none)
    vk::TargetFormats targets;
    targets.color = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const char* pipelineError = nullptr;
    VkShaderModule vs = Module(context, vsCode), ps = Module(context, psCode);
    if (!vk::CreatePipeline(context, key, vs, ps, draw.layout, pass, targets, pipeline, &pipelineError))
    {
        fprintf(stderr, "vk: %s\n", pipelineError ? pipelineError : "?");
        return 1;
    }

    // The draw's constants: the quad's transform in vc[128], a tint in pc[64].
    gpu::VSConstants vsConstants{};
    gpu::PSConstants psConstants{};
    vsConstants.c[0][0] = 0.25f;
    psConstants.c[0][0] = 0.5f;
    reinterpret_cast<uint32_t*>(&vsConstants.c[128][0])[0] = vertexBase; // asuint, as the shader reads it
    vsConstants.c[128][1] = 1.0f; // 1:1, so every texel lands on its pixel
    vsConstants.c[128][2] = 0.0f;  // offset x
    vsConstants.c[128][3] = 0.0f;  // offset y
    psConstants.c[64][0] = 1.0f;
    psConstants.c[64][1] = 1.0f;
    psConstants.c[64][2] = 1.0f;
    if (getenv("FM2_DYNAMIC_INDEX") || getenv("FM2_GET_DIMENSIONS") || getenv("FM2_INDEX_PROBE"))
    {
        const uint32_t textureIndex = 166;
        psConstants.tbias[0][0] = 0.0f;
        memcpy(&psConstants.tbias[0][1], &textureIndex, sizeof textureIndex);
    }
    psConstants.c[64][3] = 1.0f;
    if (directIndex)
        vsConstants.vte[1] = 1;

    vk::DrawBeginFrame(draw);
    if (ringFetch)
        memcpy(draw.guestMemory.cpu + vertexBase, vertices, vertexBytes);

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
    VkBuffer copy = vk::CreateBuffer(context, VkDeviceSize(width) * height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
    const uint32_t indices[6] = { 0, 1, 2, 2, 1, 3 }; // the quad as a triangle list
    const std::vector<uint32_t> noFetchConstants;
    const auto submitStarted = std::chrono::steady_clock::now();
    const bool ok = vk::Submit(context, [&](VkCommandBuffer cb) {
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = { { 0, 0 }, { width, height } };
        vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport v{ 0, 0, float(width), float(height), 0, 1 };
        VkRect2D s{ { 0, 0 }, { width, height } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &s);
        const uint32_t textureIndex = 166;
        vkCmdPushConstants(cb, draw.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof textureIndex, &textureIndex);
        const uint32_t repeats = getenv("FM2_TEST_REPEAT") ? uint32_t(atoi(getenv("FM2_TEST_REPEAT"))) : 1;
        bool drawn = true;
        for (uint32_t i = 0; i < repeats && drawn; i++)
        {
            if (ringFetch || directIndex)
                drawn = vk::DrawList(context, draw, cb, pipeline, textures.set, vsConstants, psConstants, directIndex ? nullptr : indices, 6, s,
                    &noFetchConstants);
            else
                drawn = vk::DrawIndexed(context, draw, cb, pipeline, textures.set, vsConstants, psConstants, indices, 6, true, s);
        }
        if (!drawn)
            return;
        // FM2_DRAW_HOT_LOOP=<n>: repeat the draw n more times WITHOUT resubmitting, so a profiler gets a dense sample of
        // the per-draw path in one run.
        static const long hotLoop = getenv("FM2_DRAW_HOT_LOOP") ? atol(getenv("FM2_DRAW_HOT_LOOP")) : 0;
        for (long hot = 0; hot < hotLoop; hot++)
        {
            if (ringFetch || directIndex)
                vk::DrawList(context, draw, cb, pipeline, textures.set, vsConstants, psConstants,
                    directIndex ? static_cast<const uint32_t*>(nullptr) : indices, 6, s, &noFetchConstants);
            else
                vk::DrawIndexed(context, draw, cb, pipeline, textures.set, vsConstants, psConstants, indices, 6, true, s);
        }
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, 1, &region);
    });
    if (!ok)
    {
        fprintf(stderr, "vk: submission failed\n");
        return 1;
    }
    if (getenv("FM2_TEST_REPEAT"))
        printf("vk: %u draws in %.3f ms\n", uint32_t(atoi(getenv("FM2_TEST_REPEAT"))),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitStarted).count());
    if (getenv("FM2_TEST_DUMP_RING"))
    {
        const uint32_t repeats = uint32_t(atoi(getenv("FM2_TEST_REPEAT")));
        const size_t stride = ((sizeof(gpu::VSConstants) + 255) & ~size_t(255)) + ((sizeof(gpu::PSConstants) + 255) & ~size_t(255));
        for (uint32_t i = 0; i < repeats; i++)
        {
            const auto& vs = *reinterpret_cast<const gpu::VSConstants*>(draw.constants.cpu + i * stride);
            const auto& ps = *reinterpret_cast<const gpu::PSConstants*>(draw.constants.cpu + i * stride + 6400);
            const auto* list = reinterpret_cast<const uint32_t*>(draw.guestMemory.cpu + i * 256);
            printf("ring %u: info %u scale %g tint %g indices %u %u %u %u %u %u\n", i, vs.info[0], vs.c[128][1], ps.c[64][0],
                list[0], list[1], list[2], list[3], list[4], list[5]);
        }
    }

    if (getenv("FM2_INDEX_PROBE"))
        printf("vk: host tbias %g, ring tbias %g\n", psConstants.tbias[0][1],
            *reinterpret_cast<float*>(draw.constants.cpu + ((sizeof(gpu::VSConstants) + 255) & ~size_t(255)) + 4676));
    // The quad covers the whole target (the vertex stream is in [-1,1]); compare with the source.
    auto half = [](uint16_t h) {
        const uint32_t sign = uint32_t(h & 0x8000u) << 16;
        uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits = 0;
        if (exp == 0)
        {
            bits = sign;
            if (man)
            {
                exp = 127 - 15 + 1;
                while (!(man & 0x400))
                {
                    man <<= 1;
                    exp--;
                }
                bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
            }
        }
        else if (exp == 31)
            bits = sign | 0x7F800000u | (man << 13);
        else
            bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    const uint16_t* result = static_cast<const uint16_t*>(readback);
    size_t differ = 0, mirrored = 0, sampled = 0;
    float worst = 0;
    for (uint32_t y = 1; y < height - 1; y++)
        for (uint32_t x = 1; x < width - 1; x++, sampled++)
        {
            const uint16_t* px = result + (size_t(y) * width + x) * 4;
            const uint8_t* expected = &source[(size_t(y) * width + x) * 4];
            const uint8_t wanted[3] = { expected[0], expected[1], expected[2] };
            for (int c = 0; c < 3; c++)
                worst = std::max(worst, std::abs(half(px[c]) * 255.0f - wanted[c]));
            const uint8_t* flippedRow = &source[(size_t(height - 1 - y) * width + x) * 4];
            const bool straight = std::abs(half(px[0]) * 255.0f - wanted[0]) <= 1.5f &&
                std::abs(half(px[1]) * 255.0f - wanted[1]) <= 1.5f &&
                std::abs(half(px[2]) * 255.0f - wanted[2]) <= 1.5f;
            const bool flipped = std::abs(half(px[0]) * 255.0f - flippedRow[0]) <= 1.5f &&
                std::abs(half(px[1]) * 255.0f - flippedRow[1]) <= 1.5f &&
                std::abs(half(px[2]) * 255.0f - flippedRow[2]) <= 1.5f;
            differ += !straight;
            mirrored += !flipped;
        }
    const uint16_t* centre = result + (size_t(height / 2) * width + width / 2) * 4;
    {
        // Quick look at what came out.
        FILE* f = fopen("out/vk_draw.ppm", "wb");
        if (f)
        {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (uint32_t i = 0; i < width * height; i++)
            {
                uint8_t rgb[3];
                for (int c = 0; c < 3; c++)
                {
                    const float v = std::min(1.0f, std::max(0.0f, half(result[i * 4 + c])));
                    rgb[c] = uint8_t(v * 255.0f);
                }
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    printf("vk: centre pixel %.3f %.3f %.3f, worst channel error %.1f/255\n", half(centre[0]), half(centre[1]), half(centre[2]),
        worst);
    printf("vk: %zu of %zu sampled pixels differ from the source texture (%zu if it were flipped)\n", differ, sampled,
        mirrored);
    const bool directIndexOk = !directIndex || draw.guestMemory.used == 0;
    if (directIndex)
        printf("vk: direct index path: %s\n", directIndexOk ? "no index upload" : "unexpected index upload");
    vkDeviceWaitIdle(context.device);
    vk::DrawDestroy(context, draw);
    vk::Shutdown(context);
    return differ || cubeDiffer || mipDiffer || !directIndexOk ? 2 : 0;
}
