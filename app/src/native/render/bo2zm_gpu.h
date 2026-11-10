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

namespace bo2zm::native::gpu {

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

}  // namespace bo2zm::native::gpu
