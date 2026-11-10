// Native renderer: the types of bo2zm_gpu.h converted to Vulkan's, for the Vulkan backend.

#pragma once

#include "bo2zm_gpu.h"

#include <rex/ui/vulkan/device.h>

#include <cstring>

namespace bo2zm::native::gpu {

inline VkRect2D ToVk(const Rect& r) { return VkRect2D{{r.x, r.y}, {r.width, r.height}}; }

inline Rect FromVk(const VkRect2D& r) { return Rect{r.offset.x, r.offset.y, r.extent.width, r.extent.height}; }

inline VkClearColorValue ToVk(const ClearColor& c) {
  VkClearColorValue v;
  static_assert(sizeof(v) == sizeof(c));
  std::memcpy(&v, &c, sizeof(v));
  return v;
}

inline ClearColor FromVk(const VkClearColorValue& v) {
  ClearColor c;
  std::memcpy(&c, &v, sizeof(c));
  return c;
}

inline VkClearValue ToVk(const ClearValue& c) {
  VkClearValue v;
  static_assert(sizeof(v) == sizeof(c));
  std::memcpy(&v, &c, sizeof(v));
  return v;
}

inline ClearValue FromVk(const VkClearValue& v) {
  ClearValue c;
  std::memcpy(&c, &v, sizeof(c));
  return c;
}

static_assert(kAspectColor == VK_IMAGE_ASPECT_COLOR_BIT && kAspectDepth == VK_IMAGE_ASPECT_DEPTH_BIT &&
              kAspectStencil == VK_IMAGE_ASPECT_STENCIL_BIT);
inline VkImageAspectFlags ToVk(Aspects a) { return a; }

}  // namespace bo2zm::native::gpu
