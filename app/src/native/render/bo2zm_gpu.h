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
struct VkSampler_T;
struct VkDescriptorSetLayout_T;

namespace bo2zm::native::gpu {

using CommandList = VkCommandBuffer_T*;  // the commands being recorded
using Image = VkImage_T*;
using ImageView = VkImageView_T*;
using Memory = VkDeviceMemory_T*;  // a dedicated allocation
using Sampler = VkSampler_T*;
using SetLayout = VkDescriptorSetLayout_T*;  // what a descriptor set holds

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

// A sampler. The values are in Vulkan's numbering (VkFilter, VkSamplerMipmapMode, VkSamplerAddressMode,
// VkBorderColor); Metal's MTLSamplerDescriptor has a counterpart for each.
enum Filter : uint32_t { kFilterNearest = 0, kFilterLinear = 1 };
enum AddressMode : uint32_t {
  kAddressRepeat = 0,
  kAddressMirroredRepeat = 1,
  kAddressClampToEdge = 2,
  kAddressClampToBorder = 3,
  kAddressMirrorClampToEdge = 4,
};
enum BorderColor : uint32_t { kBorderTransparentBlack = 0, kBorderOpaqueWhite = 4 };
// No upper limit on the level of detail (VK_LOD_CLAMP_NONE).
inline constexpr float kLodUnclamped = 1000.0f;

// Shader stages (Vulkan's bits).
using ShaderStages = uint32_t;
inline constexpr ShaderStages kStageVertex = 0x1;
inline constexpr ShaderStages kStageFragment = 0x10;
inline constexpr ShaderStages kStageCompute = 0x20;

// Kinds of descriptor (Vulkan's numbering).
enum DescriptorKind : uint32_t {
  kDescriptorSampler = 0,
  kDescriptorSampledImage = 2,
  kDescriptorUniformBuffer = 6,
  kDescriptorUniformBufferDynamic = 8,
};

// One binding of a descriptor set layout: `count` descriptors of `kind` seen by `stages`, and optionally an
// immutable sampler (one, for a count of 1).
struct SetBinding {
  uint32_t binding = 0;
  DescriptorKind kind = kDescriptorSampler;
  uint32_t count = 1;
  ShaderStages stages = 0;
  const Sampler* immutable_samplers = nullptr;
};

// A push constant range (layout of VkPushConstantRange).
struct PushRange {
  ShaderStages stages = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
};

// Render passes. Load and store actions and image layouts in Vulkan's numbering; Metal's render pass
// descriptor takes the same load and store actions and ignores the layouts.
enum LoadOp : uint32_t { kLoadLoad = 0, kLoadClear = 1, kLoadDontCare = 2 };
enum StoreOp : uint32_t { kStoreStore = 0, kStoreDontCare = 1 };
using ImageLayout = uint32_t;
inline constexpr ImageLayout kLayoutUndefined = 0;
inline constexpr ImageLayout kLayoutGeneral = 1;
inline constexpr ImageLayout kLayoutColorAttachment = 2;

// One attachment of a pass: its format, what happens to it at the start and the end of the pass, and its
// layouts (before, during and after).
struct PassAttachment {
  Format format = 0;
  LoadOp load = kLoadDontCare;
  StoreOp store = kStoreStore;
  LoadOp stencil_load = kLoadDontCare;
  StoreOp stencil_store = kStoreDontCare;
  ImageLayout initial = kLayoutGeneral;
  ImageLayout used = kLayoutGeneral;
  ImageLayout final = kLayoutGeneral;
};

// A single-subpass pass: up to 4 colour attachments and optionally a depth/stencil one.
struct PassDesc {
  uint32_t color_count = 0;
  PassAttachment colors[4];
  bool has_depth = false;
  PassAttachment depth;
};

// What a descriptor pool holds: `max_sets` sets and the descriptors of each kind across them.
// `update_after_bind` for the bindless heaps (written while bound). Metal sizes argument buffers from it.
struct PoolCounts {
  uint32_t max_sets = 0;
  uint32_t sampled_images = 0;
  uint32_t samplers = 0;
  uint32_t uniform_buffers = 0;
  uint32_t uniform_buffers_dynamic = 0;
  bool update_after_bind = false;
};

// What an image is used for, in Vulkan's bits (VkImageUsageFlagBits). Metal maps them to MTLTextureUsage.
using ImageUsage = uint32_t;
inline constexpr ImageUsage kImageCopySource = 0x1;
inline constexpr ImageUsage kImageCopyTarget = 0x2;
inline constexpr ImageUsage kImageSampled = 0x4;
inline constexpr ImageUsage kImageColorTarget = 0x10;
inline constexpr ImageUsage kImageDepthTarget = 0x20;

// An image: 2D (with `layers`, 6 for a cube) or 3D (`depth` slices), one sample, optimal tiling.
struct ImageDesc {
  bool volume = false;
  bool cube = false;
  Format format = 0;
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t depth = 1;
  uint32_t levels = 1;
  uint32_t layers = 1;
  ImageUsage usage = kImageSampled | kImageCopyTarget;
};

// A view of an image: its type, format, aspect, mips and layers, and the channel swizzle (Vulkan numbering:
// 0 identity, 1 zero, 2 one, 3 to 6 red to alpha). Metal makes a texture view with the same swizzle.
enum ViewType : uint32_t { kView2D = 1, kView3D = 2, kViewCube = 3 };
constexpr uint32_t kAllLevels = ~0u;
struct ViewDesc {
  ViewType type = kView2D;
  Format format = 0;
  Aspects aspects = kAspectColor;
  uint32_t levels = 1;
  uint32_t layers = 1;
  uint32_t swizzle[4] = {0, 0, 0, 0};
};

struct SamplerDesc {
  Filter mag = kFilterNearest;
  Filter min = kFilterNearest;
  Filter mip = kFilterNearest;
  AddressMode u = kAddressRepeat;
  AddressMode v = kAddressRepeat;
  AddressMode w = kAddressRepeat;
  float lod_bias = 0.0f;
  bool anisotropy = false;
  float anisotropy_max = 0.0f;
  float lod_min = 0.0f;
  float lod_max = 0.0f;
  BorderColor border = kBorderTransparentBlack;
};

}  // namespace bo2zm::native::gpu
