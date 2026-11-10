// Native renderer: the types of bo2zm_gpu.h converted to Vulkan's, for the Vulkan backend.

#pragma once

#include "bo2zm_gpu.h"

#include <rex/ui/vulkan/device.h>

#include <cstddef>
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

static_assert(sizeof(Viewport) == sizeof(VkViewport) && offsetof(Viewport, max_depth) == offsetof(VkViewport, maxDepth));
inline const VkViewport* ToVk(const Viewport* v) { return reinterpret_cast<const VkViewport*>(v); }
static_assert(kStencilFront == VK_STENCIL_FACE_FRONT_BIT && kStencilBack == VK_STENCIL_FACE_BACK_BIT &&
              kStencilBoth == VK_STENCIL_FACE_FRONT_AND_BACK);
static_assert(sizeof(BlendEquation) == sizeof(VkColorBlendEquationEXT) &&
              offsetof(BlendEquation, alpha_op) == offsetof(VkColorBlendEquationEXT, alphaBlendOp));

namespace vulkan {

/*
 * The layer's recording calls on Vulkan. Each one is exactly the Vulkan call the renderer made before it, through
 * the renderer's function table (deferred::Table: the vkCmd* entries are queued for the recorder thread), so the
 * recorded commands do not change. The command buffer is still a VkCommandBuffer while the seam is built.
 */
class Recorder {
 public:
  using Functions = rex::ui::vulkan::VulkanDevice::Functions;

  // The dynamic state setters loaded by name (EDS1/EDS2 core in 1.3, EDS3 the extension); null if missing.
  struct DynamicState {
    PFN_vkCmdSetCullMode cull_mode = nullptr;
    PFN_vkCmdSetFrontFace front_face = nullptr;
    PFN_vkCmdSetPrimitiveTopology topology = nullptr;
    PFN_vkCmdSetPrimitiveRestartEnable restart = nullptr;
    PFN_vkCmdSetDepthBiasEnable depth_bias = nullptr;
    PFN_vkCmdSetDepthTestEnable depth_test = nullptr;
    PFN_vkCmdSetDepthWriteEnable depth_write = nullptr;
    PFN_vkCmdSetDepthCompareOp depth_compare = nullptr;
    PFN_vkCmdSetStencilTestEnable stencil_test = nullptr;
    PFN_vkCmdSetStencilOp stencil_op = nullptr;
    PFN_vkCmdSetColorBlendEnableEXT blend_enable = nullptr;
    PFN_vkCmdSetColorBlendEquationEXT blend_equation = nullptr;
    PFN_vkCmdSetColorWriteMaskEXT color_mask = nullptr;
  };

  explicit Recorder(const Functions& dfn) : dfn_(dfn) {}

  const Functions& functions() const { return dfn_; }
  DynamicState& dynamic() { return dynamic_; }
  const DynamicState& dynamic() const { return dynamic_; }

  void SetViewport(VkCommandBuffer c, const Viewport& v) const { dfn_.vkCmdSetViewport(c, 0, 1, ToVk(&v)); }
  void SetScissor(VkCommandBuffer c, const Rect& r) const {
    const VkRect2D s = ToVk(r);
    dfn_.vkCmdSetScissor(c, 0, 1, &s);
  }
  void SetBlendConstants(VkCommandBuffer c, const float constants[4]) const {
    dfn_.vkCmdSetBlendConstants(c, constants);
  }
  // No clamp: the renderer never clamps the bias.
  void SetDepthBias(VkCommandBuffer c, float constant, float slope) const {
    dfn_.vkCmdSetDepthBias(c, constant, 0.0f, slope);
  }
  /*
   * Stencil reference, compare mask and write mask of both faces, each packed as reference | compare << 8 |
   * write << 16: the reference of the front then of the back face, then the compare masks, then the write masks.
   */
  void SetStencilValues(VkCommandBuffer c, uint32_t front, uint32_t back) const {
    dfn_.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
    dfn_.vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
    dfn_.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, (front >> 8) & 0xFF);
    dfn_.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
    dfn_.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, (front >> 16) & 0xFF);
    dfn_.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
  }

  void SetCullMode(VkCommandBuffer c, CullMode v) const { dynamic_.cull_mode(c, VkCullModeFlags(v)); }
  void SetFrontFace(VkCommandBuffer c, FrontFace v) const { dynamic_.front_face(c, VkFrontFace(v)); }
  void SetTopology(VkCommandBuffer c, Topology v) const { dynamic_.topology(c, VkPrimitiveTopology(v)); }
  void SetPrimitiveRestart(VkCommandBuffer c, bool on) const { dynamic_.restart(c, VkBool32(on)); }
  void SetDepthBiasEnable(VkCommandBuffer c, bool on) const { dynamic_.depth_bias(c, VkBool32(on)); }
  void SetDepthTest(VkCommandBuffer c, bool on) const { dynamic_.depth_test(c, VkBool32(on)); }
  void SetDepthWrite(VkCommandBuffer c, bool on) const { dynamic_.depth_write(c, VkBool32(on)); }
  void SetDepthCompare(VkCommandBuffer c, CompareOp v) const { dynamic_.depth_compare(c, VkCompareOp(v)); }
  void SetStencilTest(VkCommandBuffer c, bool on) const { dynamic_.stencil_test(c, VkBool32(on)); }
  // ops: fail, pass, depth fail, compare.
  void SetStencilOps(VkCommandBuffer c, StencilFace faces, const uint32_t ops[4]) const {
    dynamic_.stencil_op(c, VkStencilFaceFlags(faces), VkStencilOp(ops[0]), VkStencilOp(ops[1]),
                        VkStencilOp(ops[2]), VkCompareOp(ops[3]));
  }
  void SetBlendEnable(VkCommandBuffer c, uint32_t count, const uint32_t* enable) const {
    dynamic_.blend_enable(c, 0, count, enable);
  }
  void SetBlendEquations(VkCommandBuffer c, uint32_t count, const BlendEquation* equations) const {
    dynamic_.blend_equation(c, 0, count, reinterpret_cast<const VkColorBlendEquationEXT*>(equations));
  }
  void SetColorMasks(VkCommandBuffer c, uint32_t count, const ColorMask* masks) const {
    dynamic_.color_mask(c, 0, count, masks);
  }

 private:
  const Functions& dfn_;
  DynamicState dynamic_;
};

}  // namespace vulkan

}  // namespace bo2zm::native::gpu
