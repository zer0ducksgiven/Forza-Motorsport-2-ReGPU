#pragma once
// Guest index streams, expanded into the host triangle lists both backends draw. This is
// guest memory and registers only — nothing here is API specific — so the Direct3D 12 and
// Vulkan renderers share it rather than each expanding strips and fans their own way.
#include <cstdint>
#include <vector>

namespace gpu
{
// One index from the DMA stream at `base` (a guest address), honouring the endian mode in
// the high bits of the packet's size field.
uint32_t ReadGuestIndex(uint32_t base, uint32_t i, bool is32, uint32_t endian);

// Expanded host vertex list: guest indices, or rectangle markers
// (bit 31 | role << 29 | list position of the rectangle's first vertex).
// `initiator` is the draw packet's initiator: primitive, index source, count, width.
// A primitive type with no expansion fails, unless `listFallback`: then its vertices are drawn as a plain triangle
// list (the Vulkan backend, which has no other way to draw them; Direct3D 12 draws those from the index buffer).
bool BuildIndexList(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize, std::vector<uint32_t>& out, bool listFallback = false);
}
