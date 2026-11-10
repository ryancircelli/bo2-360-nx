// Native renderer: the graphics API seen from the renderer's own logic.
//
// The renderer (targets, draws, deferred recording) was written against Vulkan. To also run on Metal without
// MoltenVK, the calls that reach the graphics API move, step by step, behind this small layer shaped after what
// the renderer actually does (render passes, the bindless heaps, the set 4 UBOs, the dynamic state it sets, the
// occlusion and timestamp queries, copies and blits), not after a general-purpose RHI. Vulkan stays the default
// backend and the only one on the Switch; Metal is built with BO2ZM_NATIVE_METAL on Apple platforms.
//
// This header has no graphics API in it. bo2zm_gpu_vulkan.h converts its types to Vulkan's.

#pragma once

#include <cstdint>

// Handles of the drawing backend. Vulkan draws in every build for now (Metal is brought up next to it), so these are
// Vulkan's own handle types (VkImage is VkImage_T*), declared without the Vulkan headers; bo2zm_gpu_vulkan.h checks
// that they are the same types. The Metal backend gives them its own.
struct VkCommandBuffer_T;
struct VkImage_T;
struct VkImageView_T;
struct VkDeviceMemory_T;

namespace bo2zm::native::gpu {

using CommandList = VkCommandBuffer_T*;  // the commands being recorded
using Image = VkImage_T*;
using ImageView = VkImageView_T*;
using Memory = VkDeviceMemory_T*;  // a dedicated allocation

enum class Backend : uint8_t { kVulkan, kMetal };

// The backend this build renders with.
constexpr Backend kBackend =
#if BO2ZM_NATIVE_METAL
    Backend::kMetal;
#else
    Backend::kVulkan;
#endif

// A rectangle in pixels (Vulkan's VkRect2D, Metal's MTLScissorRect).
struct Rect {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// A color clear value, read as floats, signed or unsigned integers depending on the format of the image.
union ClearColor {
  float f[4];
  int32_t i[4];
  uint32_t u[4];
};

// Plain (no member initializers) so it can live in ClearValue; ClearValue{} is all zeros, like VkClearValue{}.
struct ClearDepthStencil {
  float depth;
  uint32_t stencil;
};

union ClearValue {
  ClearColor color;
  ClearDepthStencil depth_stencil;
};

// Which parts of an image an operation touches (the bits are Vulkan's VkImageAspectFlagBits).
enum Aspect : uint32_t {
  kAspectColor = 1u << 0,
  kAspectDepth = 1u << 1,
  kAspectStencil = 1u << 2,
};
using Aspects = uint32_t;

// A viewport (Vulkan's VkViewport, Metal's MTLViewport with znear and zfar).
struct Viewport {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float min_depth = 0.0f;
  float max_depth = 1.0f;
};

// The faces a stencil state applies to (the bits are Vulkan's VkStencilFaceFlagBits).
enum StencilFace : uint32_t {
  kStencilFront = 1u << 0,
  kStencilBack = 1u << 1,
  kStencilBoth = kStencilFront | kStencilBack,
};

// The fixed function state the renderer sets per draw is kept as plain numbers in Vulkan's numbering (VkCullModeFlags,
// VkFrontFace, VkPrimitiveTopology, VkCompareOp, VkStencilOp, VkBlendFactor, VkBlendOp, VkColorComponentFlags): the
// Vulkan backend passes them through and the Metal backend maps them with small tables.
using CullMode = uint32_t;
using FrontFace = uint32_t;
using Topology = uint32_t;
using CompareOp = uint32_t;
using StencilOp = uint32_t;
using ColorMask = uint32_t;

// One color target's blend equation (layout of VkColorBlendEquationEXT).
struct BlendEquation {
  uint32_t src_color = 0;
  uint32_t dst_color = 0;
  uint32_t color_op = 0;
  uint32_t src_alpha = 0;
  uint32_t dst_alpha = 0;
  uint32_t alpha_op = 0;
};

// What a buffer is used for, in Vulkan's bits (VkBufferUsageFlagBits). Metal buffers need none of it.
using BufferUsage = uint32_t;
inline constexpr BufferUsage kBufferCopySource = 0x1;
inline constexpr BufferUsage kBufferCopyTarget = 0x2;
inline constexpr BufferUsage kBufferUniform = 0x10;
inline constexpr BufferUsage kBufferStorage = 0x20;
inline constexpr BufferUsage kBufferIndex = 0x40;
inline constexpr BufferUsage kBufferVertex = 0x80;
inline constexpr BufferUsage kBufferAddress = 0x20000;  // the shaders reach it by its GPU address

// An image format, in Vulkan's numbering (VkFormat); the Metal backend maps it to its MTLPixelFormat with a table.
using Format = uint32_t;

// What a set of queries counts. Metal: timestamps are a counter sample buffer, occlusion counts a visibility result
// buffer, and pipeline statistics have no equivalent (the renderer only uses them for diagnostics).
enum class QueryKind : uint8_t {
  kTimestamp,
  kOcclusion,
  kPipelineStatistics,  // vertex shader invocations, clipped primitives and fragment shader invocations, in that order
};

}  // namespace bo2zm::native::gpu
