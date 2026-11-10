// Native renderer: the types of bo2zm_gpu.h converted to Vulkan's, for the Vulkan backend.

#pragma once

#include "bo2zm_gpu.h"

#include <rex/ui/vulkan/device.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

static_assert(std::is_same_v<bo2zm::native::gpu::CommandList, VkCommandBuffer>);
static_assert(std::is_same_v<bo2zm::native::gpu::Image, VkImage>);
static_assert(std::is_same_v<bo2zm::native::gpu::ImageView, VkImageView>);
static_assert(std::is_same_v<bo2zm::native::gpu::Memory, VkDeviceMemory>);

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
  struct Loaded {
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
    // Not in the SDK's table either; queued through deferred::Wrap like the others.
    PFN_vkCmdCopyImage copy_image = nullptr;
    PFN_vkCmdClearDepthStencilImage clear_depth = nullptr;
    PFN_vkCmdBlitImage blit = nullptr;
    PFN_vkCmdWriteTimestamp write_timestamp = nullptr;
  };

  explicit Recorder(const Functions& dfn) : dfn_(dfn) {}

  const Functions& functions() const { return dfn_; }
  Loaded& loaded() { return loaded_; }
  const Loaded& loaded() const { return loaded_; }

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

  // Binding and drawing. The handles are still Vulkan's (VkPipeline, VkPipelineLayout, VkDescriptorSet, VkBuffer)
  // until the resources move behind the layer.
  void BindGraphicsPipeline(VkCommandBuffer c, VkPipeline p) const {
    dfn_.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
  }
  void BindComputePipeline(VkCommandBuffer c, VkPipeline p) const {
    dfn_.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, p);
  }
  // Graphics descriptor sets first .. first + count - 1, with the dynamic offsets of their dynamic UBOs.
  void BindSets(VkCommandBuffer c, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet* sets,
                uint32_t n_offsets = 0, const uint32_t* offsets = nullptr) const {
    dfn_.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, sets, n_offsets, offsets);
  }
  // stages: VkShaderStageFlags (vertex 0x1, fragment 0x10, compute 0x20).
  void PushConstants(VkCommandBuffer c, VkPipelineLayout layout, uint32_t stages, uint32_t offset, uint32_t size,
                     const void* data) const {
    dfn_.vkCmdPushConstants(c, layout, VkShaderStageFlags(stages), offset, size, data);
  }
  void BindVertexBuffers(VkCommandBuffer c, uint32_t count, const VkBuffer* buffers,
                         const VkDeviceSize* offsets) const {
    dfn_.vkCmdBindVertexBuffers(c, 0, count, buffers, offsets);
  }
  void BindIndexBuffer(VkCommandBuffer c, VkBuffer buffer, bool indices_16) const {
    dfn_.vkCmdBindIndexBuffer(c, buffer, 0, indices_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
  }
  // One instance, first instance 0: the renderer never instances.
  void Draw(VkCommandBuffer c, uint32_t count, uint32_t first_vertex) const {
    dfn_.vkCmdDraw(c, count, 1, first_vertex, 0);
  }
  void DrawIndexed(VkCommandBuffer c, uint32_t count, uint32_t first_index, int32_t vertex_offset) const {
    dfn_.vkCmdDrawIndexed(c, count, 1, first_index, vertex_offset, 0);
  }
  void Dispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) const { dfn_.vkCmdDispatch(c, x, y, z); }

  void SetCullMode(VkCommandBuffer c, CullMode v) const { loaded_.cull_mode(c, VkCullModeFlags(v)); }
  void SetFrontFace(VkCommandBuffer c, FrontFace v) const { loaded_.front_face(c, VkFrontFace(v)); }
  void SetTopology(VkCommandBuffer c, Topology v) const { loaded_.topology(c, VkPrimitiveTopology(v)); }
  void SetPrimitiveRestart(VkCommandBuffer c, bool on) const { loaded_.restart(c, VkBool32(on)); }
  void SetDepthBiasEnable(VkCommandBuffer c, bool on) const { loaded_.depth_bias(c, VkBool32(on)); }
  void SetDepthTest(VkCommandBuffer c, bool on) const { loaded_.depth_test(c, VkBool32(on)); }
  void SetDepthWrite(VkCommandBuffer c, bool on) const { loaded_.depth_write(c, VkBool32(on)); }
  void SetDepthCompare(VkCommandBuffer c, CompareOp v) const { loaded_.depth_compare(c, VkCompareOp(v)); }
  void SetStencilTest(VkCommandBuffer c, bool on) const { loaded_.stencil_test(c, VkBool32(on)); }
  // ops: fail, pass, depth fail, compare.
  void SetStencilOps(VkCommandBuffer c, StencilFace faces, const uint32_t ops[4]) const {
    loaded_.stencil_op(c, VkStencilFaceFlags(faces), VkStencilOp(ops[0]), VkStencilOp(ops[1]),
                        VkStencilOp(ops[2]), VkCompareOp(ops[3]));
  }
  void SetBlendEnable(VkCommandBuffer c, uint32_t count, const uint32_t* enable) const {
    loaded_.blend_enable(c, 0, count, enable);
  }
  void SetBlendEquations(VkCommandBuffer c, uint32_t count, const BlendEquation* equations) const {
    loaded_.blend_equation(c, 0, count, reinterpret_cast<const VkColorBlendEquationEXT*>(equations));
  }
  void SetColorMasks(VkCommandBuffer c, uint32_t count, const ColorMask* masks) const {
    loaded_.color_mask(c, 0, count, masks);
  }

  // Render passes, clears, copies, barriers and queries. These still take the Vulkan structures (the images,
  // buffers and passes are Vulkan objects until the resources move behind the layer); each is the same call.
  void BeginPass(VkCommandBuffer c, const VkRenderPassBeginInfo& begin) const {
    dfn_.vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
  }
  void EndPass(VkCommandBuffer c) const { dfn_.vkCmdEndRenderPass(c); }
  void ClearAttachments(VkCommandBuffer c, uint32_t count, const VkClearAttachment* attachments, uint32_t rects,
                        const VkClearRect* rectangles) const {
    dfn_.vkCmdClearAttachments(c, count, attachments, rects, rectangles);
  }
  void ClearColorImage(VkCommandBuffer c, VkImage image, VkImageLayout layout, const VkClearColorValue* color,
                       uint32_t count, const VkImageSubresourceRange* ranges) const {
    dfn_.vkCmdClearColorImage(c, image, layout, color, count, ranges);
  }
  void CopyBuffer(VkCommandBuffer c, VkBuffer source, VkBuffer target, uint32_t count,
                  const VkBufferCopy* regions) const {
    dfn_.vkCmdCopyBuffer(c, source, target, count, regions);
  }
  void CopyBufferToImage(VkCommandBuffer c, VkBuffer source, VkImage target, VkImageLayout layout, uint32_t count,
                         const VkBufferImageCopy* regions) const {
    dfn_.vkCmdCopyBufferToImage(c, source, target, layout, count, regions);
  }
  void CopyImageToBuffer(VkCommandBuffer c, VkImage source, VkImageLayout layout, VkBuffer target, uint32_t count,
                         const VkBufferImageCopy* regions) const {
    dfn_.vkCmdCopyImageToBuffer(c, source, layout, target, count, regions);
  }
  void Barrier(VkCommandBuffer c, VkPipelineStageFlags source, VkPipelineStageFlags target,
               VkDependencyFlags dependencies, uint32_t memories, const VkMemoryBarrier* memory, uint32_t buffers,
               const VkBufferMemoryBarrier* buffer, uint32_t images, const VkImageMemoryBarrier* image) const {
    dfn_.vkCmdPipelineBarrier(c, source, target, dependencies, memories, memory, buffers, buffer, images, image);
  }
  void BeginQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t index, VkQueryControlFlags flags) const {
    dfn_.vkCmdBeginQuery(c, pool, index, flags);
  }
  void EndQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t index) const { dfn_.vkCmdEndQuery(c, pool, index); }
  void CopyImage(VkCommandBuffer c, VkImage source, VkImageLayout source_layout, VkImage target,
                 VkImageLayout target_layout, uint32_t count, const VkImageCopy* regions) const {
    loaded_.copy_image(c, source, source_layout, target, target_layout, count, regions);
  }
  void ClearDepthStencilImage(VkCommandBuffer c, VkImage image, VkImageLayout layout,
                              const VkClearDepthStencilValue* value, uint32_t count,
                              const VkImageSubresourceRange* ranges) const {
    loaded_.clear_depth(c, image, layout, value, count, ranges);
  }
  void Blit(VkCommandBuffer c, VkImage source, VkImageLayout source_layout, VkImage target,
            VkImageLayout target_layout, uint32_t count, const VkImageBlit* regions, VkFilter filter) const {
    loaded_.blit(c, source, source_layout, target, target_layout, count, regions, filter);
  }
  void WriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t index) const {
    loaded_.write_timestamp(c, stage, pool, index);
  }
  void ResetQueries(VkCommandBuffer c, VkQueryPool pool, uint32_t first, uint32_t count) const {
    dfn_.vkCmdResetQueryPool(c, pool, first, count);
  }

 private:
  const Functions& dfn_;
  Loaded loaded_;
};

}  // namespace vulkan

}  // namespace bo2zm::native::gpu
