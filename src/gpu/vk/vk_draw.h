#pragma once
// Per-draw submission: the constant blocks, the index list and the bind chain, the Vulkan
// counterpart of what DrawImpl does on Direct3D 12.
//
// Constants go into a ring that is rewritten every draw and are bound with dynamic offsets. The
// expanded index list goes into another ring and is bound as a 32-bit index buffer. Nothing here
// knows about EDRAM or render targets: the caller opens the render pass and binds the pipeline.
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "gpu/constants.h"
#include "gpu/vk/vk_context.h"
#include "gpu/vk/vk_state.h"

namespace vk
{
// Counts draws handed to vkCmdDraw* since the last reset: what was SUBMITTED, as opposed to what was recorded.
extern uint64_t g_cmdDrawCount;
struct DrawRing
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* cpu = nullptr;
    VkDeviceSize size = 0, used = 0;
};

struct DrawContext
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE;
    DrawRing constants;   // VSConstants then PSConstants, per draw
    // The current draw's shader hashes, set by the caller so a diagnostic can name the draw (the overflow log does).
    uint64_t psHashDebug = 0, vsHashDebug = 0;
    DrawRing indices;     // expanded index lists, for vkCmdDrawIndexed
    DrawRing guestMemory; // expanded index lists and vertex snapshots (gmem)
    VkDeviceSize carSnapshotBytes = 0;
    VkDescriptorSet set0 = VK_NULL_HANDLE;      // constants (dynamic), guest memory, samplers
    VkDescriptorSet set0Local = VK_NULL_HANDLE; // gphys aliases the upload ring when no direct reads remain
    VkDescriptorSet bindless2D = VK_NULL_HANDLE; // set 1
    VkDescriptorSet bindlessCube = VK_NULL_HANDLE; // set 2
    VkDescriptorPool pool = VK_NULL_HANDLE;       // owns set0 and set0Local
    // False when the guest window could not be imported: in-place vertex reads would then read the upload ring
    // as if it were guest RAM, so every fetch has to be copied instead.
    bool hasGuestPhysical = false;
    VkDescriptorBufferInfo constantsInfo{};     // both dynamic offsets point into this buffer
    // Guest vertex buffers already copied into the ring this frame: (base << 32 | size) -> ring offset.
    std::unordered_map<uint64_t, uint64_t> vertexUploads;
    // FM2_PERF: where a DrawList call spends its time, summed over the frame.
    double perfPreFetchMs = 0, perfFetchLoopMs = 0, perfVertexMs = 0, perfConstantsMs = 0, perfBindMs = 0, perfCallMs = 0;
    uint64_t constantOverflows = 0, guestOverflows = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4]{};
    uint32_t stencil = ~0u;
    bool viewportValid = false, scissorValid = false, blendValid = false;
};

// constantsBytes should hold a frame's per-draw blocks (6352 and 5184 bytes);
// listBytes a frame's expanded lists and snapshotted vertex data.
bool DrawInit(Context& context, DrawContext& draw, VkBuffer gphys, VkSampler samplers[4], VkDescriptorSet bindless2D,
    VkDescriptorSet bindlessCube, VkDeviceSize constantsBytes, VkDeviceSize listBytes);
void DrawDestroy(Context& context, DrawContext& draw);

void DrawResetState(DrawContext& draw);
// Rewinds both rings and forgets the previous command buffer's dynamic state.
void DrawBeginFrame(DrawContext& draw);

// Records one draw with the shared constant and descriptor chain. indexed selects whether
// to upload and bind an index buffer; otherwise count vertices are drawn directly.
// scissor: the guest's window scissor in host pixels. Every draw is clipped to it — the guest
// uses it to keep a tile, a HUD element or a letterbox bar inside its rectangle, and a draw that
// ignores it paints over the whole target (which is what turned a small black quad into a black
// screen).
bool DrawIndexed(const Context& context, DrawContext& draw, VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet textures,
    const gpu::VSConstants& vs, const gpu::PSConstants& ps, const uint32_t* indices, uint32_t count, bool indexed,
    const VkRect2D& scissor);

// Records a non-indexed draw. The vertex shader reads an expanded index list from the upload
// ring, or the original guest index buffer when vs.vte[1] has its direct bit set. In the latter
// case list may be null: count is the number of expanded vertices, and no indices are uploaded.
// Guest vertex buffers the draw fetches are snapshotted into the ring (vs.vf is rewritten to
// point at the copies). fetchConstants lists the vf slots the shader reads; null checks all 96.
bool DrawList(const Context& context, DrawContext& draw, VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet textures,
    gpu::VSConstants& vs, const gpu::PSConstants& ps, const uint32_t* list, uint32_t count, const VkRect2D& scissor,
    const std::vector<uint32_t>* fetchConstants = nullptr);

void DrawSetViewport(const Context& context, DrawContext& draw, VkCommandBuffer cmd, const VkViewport& viewport);
void DrawSetBlendConstants(const Context& context, DrawContext& draw, VkCommandBuffer cmd, const float rgba[4]);
void DrawSetStencilReference(const Context& context, DrawContext& draw, VkCommandBuffer cmd, uint32_t value);
}
