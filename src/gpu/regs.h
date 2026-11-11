#pragma once
#include <cstdint>
#include <cstring>

#include "memory.h"

// The Xenos register file, memory-mapped at 0x7FC80000 in guest space and
// written by the command processor. Constants live in it too:
// ALU 0x4000+, fetch 0x4800+, bool 0x4900+, loop 0x4908+.
namespace gpureg
{
constexpr uint32_t kBase = 0x7FC80000;
constexpr uint32_t AluConstants = 0x4000;
constexpr uint32_t FetchConstants = 0x4800;
constexpr uint32_t BoolConstants = 0x4900;
constexpr uint32_t LoopConstants = 0x4908;

// Draw state the index expansion reads.
constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;

// The register file this thread reads: the guest's (written by the command processor), or
// the draw worker's own copy, which follows the command processor's writes in order.
inline thread_local uint8_t* t_file = nullptr;
inline uint8_t* File() { return t_file ? t_file : GuestPtr<uint8_t>(kBase); }
inline uint32_t Get(uint32_t index)
{
    uint32_t v;
    memcpy(&v, File() + index * 4, 4);
    return __builtin_bswap32(v);
}
inline void Set(uint32_t index, uint32_t value)
{
    value = __builtin_bswap32(value);
    memcpy(File() + index * 4, &value, 4);
}
}

// Physical addresses as the GPU sees them, in our 0xA0000000 view.
inline uint32_t GpuPhysToGuest(uint32_t phys) { return 0xA0000000 + (phys & 0x1FFFFFFF); }

// Dry-runs the command stream that is queued but not executed yet (rest of the buffers
// being executed, then the ring up to the write pointer) on a snapshot of the state
// registers, reporting each upcoming draw's shaders and render state to the renderer
// (RendererPredictShader / RendererPredictDraw). Command processor thread only.
void GpuLookahead();

// Frame capture (src/gpu/frame.cpp), called on the command processor thread.
enum class ShaderType
{
    Vertex = 0,
    Pixel = 1,
};
void FrameOnShaderLoad(ShaderType type, const uint32_t* microcodeBE, uint32_t dwords, uint32_t guestAddress);
void FrameOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
void FrameOnSwap(uint64_t frame, uint32_t frontBuffer);
