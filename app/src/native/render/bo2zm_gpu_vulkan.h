// Native renderer: the types of bo2zm_gpu.h converted to Vulkan's, for the Vulkan backend.

#pragma once

#include "bo2zm_gpu.h"

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

#if BO2ZM_NATIVE_METAL
#include "metal/bo2zm_gpu_metal_mirror.h"
// Gives the call to the Metal mirror when it is on (see metal/bo2zm_gpu_metal_mirror.h).
#define BO2ZM_MIRROR(call)                                                         \
  do {                                                                             \
    if (auto* bo2zm_mirror = ::bo2zm::native::gpu::metal::Mirror::Get()) {         \
      bo2zm_mirror->call;                                                          \
    }                                                                              \
  } while (0)
#else
#define BO2ZM_MIRROR(call) \
  do {                     \
  } while (0)
#endif

static_assert(std::is_same_v<bo2zm::native::gpu::CommandList, VkCommandBuffer>);
static_assert(std::is_same_v<bo2zm::native::gpu::Image, VkImage>);
static_assert(std::is_same_v<bo2zm::native::gpu::ImageView, VkImageView>);
static_assert(std::is_same_v<bo2zm::native::gpu::Memory, VkDeviceMemory>);
static_assert(std::is_same_v<bo2zm::native::gpu::Sampler, VkSampler>);
static_assert(std::is_same_v<bo2zm::native::gpu::SetLayout, VkDescriptorSetLayout>);
static_assert(std::is_same_v<bo2zm::native::gpu::Shader, VkShaderModule>);
static_assert(std::is_same_v<bo2zm::native::gpu::Pass, VkRenderPass>);
static_assert(std::is_same_v<bo2zm::native::gpu::PipelineLayout, VkPipelineLayout>);
static_assert(std::is_same_v<bo2zm::native::gpu::Pipeline, VkPipeline>);
static_assert(std::is_same_v<bo2zm::native::gpu::PipelineCache, VkPipelineCache>);
static_assert(std::is_same_v<bo2zm::native::gpu::Buffer, VkBuffer>);
static_assert(std::is_same_v<bo2zm::native::gpu::Size, VkDeviceSize>);
static_assert(std::is_same_v<bo2zm::native::gpu::Address, VkDeviceAddress>);

// The copy regions have Vulkan's layout, so the recorder passes them through.
#define BO2ZM_GPU_SAME(G, V, gf, vf) static_assert(offsetof(G, gf) == offsetof(V, vf))
static_assert(sizeof(bo2zm::native::gpu::Layers) == sizeof(VkImageSubresourceLayers));
BO2ZM_GPU_SAME(bo2zm::native::gpu::Layers, VkImageSubresourceLayers, level, mipLevel);
BO2ZM_GPU_SAME(bo2zm::native::gpu::Layers, VkImageSubresourceLayers, base_layer, baseArrayLayer);
BO2ZM_GPU_SAME(bo2zm::native::gpu::Layers, VkImageSubresourceLayers, layers, layerCount);
static_assert(sizeof(bo2zm::native::gpu::Offset3) == sizeof(VkOffset3D));
static_assert(sizeof(bo2zm::native::gpu::Extent3) == sizeof(VkExtent3D));
static_assert(sizeof(bo2zm::native::gpu::ImageCopy) == sizeof(VkImageCopy));
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageCopy, VkImageCopy, source_offset, srcOffset);
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageCopy, VkImageCopy, target, dstSubresource);
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageCopy, VkImageCopy, target_offset, dstOffset);
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageCopy, VkImageCopy, extent, extent);
static_assert(sizeof(bo2zm::native::gpu::ImageBlit) == sizeof(VkImageBlit));
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageBlit, VkImageBlit, source_offsets, srcOffsets);
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageBlit, VkImageBlit, target, dstSubresource);
BO2ZM_GPU_SAME(bo2zm::native::gpu::ImageBlit, VkImageBlit, target_offsets, dstOffsets);
static_assert(sizeof(bo2zm::native::gpu::BufferImageCopy) == sizeof(VkBufferImageCopy));
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferImageCopy, VkBufferImageCopy, row_length, bufferRowLength);
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferImageCopy, VkBufferImageCopy, image_height, bufferImageHeight);
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferImageCopy, VkBufferImageCopy, layers, imageSubresource);
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferImageCopy, VkBufferImageCopy, offset, imageOffset);
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferImageCopy, VkBufferImageCopy, extent, imageExtent);
static_assert(sizeof(bo2zm::native::gpu::BufferCopy) == sizeof(VkBufferCopy));
static_assert(sizeof(bo2zm::native::gpu::ClearValue) == sizeof(VkClearValue));
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferCopy, VkBufferCopy, target_offset, dstOffset);
BO2ZM_GPU_SAME(bo2zm::native::gpu::BufferCopy, VkBufferCopy, size, size);
#undef BO2ZM_GPU_SAME
static_assert(std::is_same_v<bo2zm::native::gpu::Framebuffer, VkFramebuffer>);
static_assert(std::is_same_v<bo2zm::native::gpu::Set, VkDescriptorSet>);
static_assert(std::is_same_v<bo2zm::native::gpu::SetPool, VkDescriptorPool>);
static_assert(std::is_same_v<bo2zm::native::gpu::QueryPool, VkQueryPool>);
static_assert(std::is_same_v<bo2zm::native::gpu::Fence, VkFence>);
static_assert(std::is_same_v<bo2zm::native::gpu::CommandPool, VkCommandPool>);
static_assert(bo2zm::native::gpu::kStageVertex == VK_SHADER_STAGE_VERTEX_BIT &&
              bo2zm::native::gpu::kStageFragment == VK_SHADER_STAGE_FRAGMENT_BIT &&
              bo2zm::native::gpu::kStageCompute == VK_SHADER_STAGE_COMPUTE_BIT);
static_assert(bo2zm::native::gpu::kDescriptorSampler == VK_DESCRIPTOR_TYPE_SAMPLER &&
              bo2zm::native::gpu::kDescriptorSampledImage == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
              bo2zm::native::gpu::kDescriptorUniformBuffer == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
              bo2zm::native::gpu::kDescriptorUniformBufferDynamic == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
static_assert(bo2zm::native::gpu::kLoadClear == VK_ATTACHMENT_LOAD_OP_CLEAR &&
              bo2zm::native::gpu::kLoadDontCare == VK_ATTACHMENT_LOAD_OP_DONT_CARE &&
              bo2zm::native::gpu::kStoreDontCare == VK_ATTACHMENT_STORE_OP_DONT_CARE &&
              bo2zm::native::gpu::kLayoutGeneral == VK_IMAGE_LAYOUT_GENERAL &&
              bo2zm::native::gpu::kLayoutColorAttachment == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
static_assert(sizeof(bo2zm::native::gpu::PushRange) == sizeof(VkPushConstantRange) &&
              offsetof(bo2zm::native::gpu::PushRange, offset) == offsetof(VkPushConstantRange, offset) &&
              offsetof(bo2zm::native::gpu::PushRange, size) == offsetof(VkPushConstantRange, size));

static_assert(bo2zm::native::gpu::kFilterLinear == VK_FILTER_LINEAR &&
              bo2zm::native::gpu::kFilterLinear == VK_SAMPLER_MIPMAP_MODE_LINEAR);
static_assert(bo2zm::native::gpu::kAddressClampToBorder == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER &&
              bo2zm::native::gpu::kAddressMirrorClampToEdge == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
static_assert(bo2zm::native::gpu::kBorderOpaqueWhite == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
static_assert(bo2zm::native::gpu::kLodUnclamped == VK_LOD_CLAMP_NONE);

namespace bo2zm::native::gpu {

inline VkRect2D ToVk(const Rect& r) { return VkRect2D{{r.x, r.y}, {r.width, r.height}}; }
static_assert(sizeof(Range) == sizeof(VkImageSubresourceRange));
static_assert(sizeof(ClearColor) == sizeof(VkClearColorValue));
static_assert(sizeof(ClearDepthStencil) == sizeof(VkClearDepthStencilValue));
inline VkImageSubresourceRange ToVk(const Range& r) {
  return VkImageSubresourceRange{r.aspects, r.base_level, r.levels, r.base_layer, r.layers};
}

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
inline VkFormat ToVkFormat(Format f) { return VkFormat(f); }

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

  void SetViewport(VkCommandBuffer c, const Viewport& v) const {
    dfn_.vkCmdSetViewport(c, 0, 1, ToVk(&v));
    BO2ZM_MIRROR(SetViewport(c, v));
  }
  void SetScissor(VkCommandBuffer c, const Rect& r) const {
    const VkRect2D s = ToVk(r);
    dfn_.vkCmdSetScissor(c, 0, 1, &s);
    BO2ZM_MIRROR(SetScissor(c, r));
  }
  void SetBlendConstants(VkCommandBuffer c, const float constants[4]) const {
    dfn_.vkCmdSetBlendConstants(c, constants);
    BO2ZM_MIRROR(SetBlendConstants(c, constants));
  }
  // No clamp: the renderer never clamps the bias.
  void SetDepthBias(VkCommandBuffer c, float constant, float slope) const {
    dfn_.vkCmdSetDepthBias(c, constant, 0.0f, slope);
    BO2ZM_MIRROR(SetDepthBias(c, constant, slope));
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
    BO2ZM_MIRROR(SetStencilValues(c, front, back));
  }

  // Binding and drawing. The handles are still Vulkan's (VkPipeline, VkPipelineLayout, VkDescriptorSet, VkBuffer)
  // until the resources move behind the layer.
  void BindGraphicsPipeline(VkCommandBuffer c, VkPipeline p) const {
    dfn_.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    BO2ZM_MIRROR(BindPipeline(c, p));
  }
  void BindComputePipeline(VkCommandBuffer c, VkPipeline p) const {
    dfn_.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, p);
  }
  // Graphics descriptor sets first .. first + count - 1, with the dynamic offsets of their dynamic UBOs.
  void BindSets(VkCommandBuffer c, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet* sets,
                uint32_t n_offsets = 0, const uint32_t* offsets = nullptr) const {
    dfn_.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, sets, n_offsets, offsets);
    BO2ZM_MIRROR(BindSets(c, first, count, sets, n_offsets, offsets));
  }
  // stages: VkShaderStageFlags (vertex 0x1, fragment 0x10, compute 0x20).
  void PushConstants(VkCommandBuffer c, VkPipelineLayout layout, uint32_t stages, uint32_t offset, uint32_t size,
                     const void* data) const {
    dfn_.vkCmdPushConstants(c, layout, VkShaderStageFlags(stages), offset, size, data);
    BO2ZM_MIRROR(PushConstants(c, stages, offset, size, data));
  }
  void BindVertexBuffers(VkCommandBuffer c, uint32_t count, const VkBuffer* buffers,
                         const VkDeviceSize* offsets) const {
    dfn_.vkCmdBindVertexBuffers(c, 0, count, buffers, offsets);
    BO2ZM_MIRROR(BindVertexBuffers(c, count, buffers, offsets));
  }
  void BindIndexBuffer(VkCommandBuffer c, VkBuffer buffer, bool indices_16) const {
    dfn_.vkCmdBindIndexBuffer(c, buffer, 0, indices_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    BO2ZM_MIRROR(BindIndexBuffer(c, buffer, indices_16));
  }
  // One instance, first instance 0: the renderer never instances.
  void Draw(VkCommandBuffer c, uint32_t count, uint32_t first_vertex) const {
    dfn_.vkCmdDraw(c, count, 1, first_vertex, 0);
    BO2ZM_MIRROR(Draw(c, count, first_vertex));
  }
  void DrawIndexed(VkCommandBuffer c, uint32_t count, uint32_t first_index, int32_t vertex_offset) const {
    dfn_.vkCmdDrawIndexed(c, count, 1, first_index, vertex_offset, 0);
    BO2ZM_MIRROR(DrawIndexed(c, count, first_index, vertex_offset));
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
  // Opens the pass over area, with the clear values of its CLEAR attachments (in attachment order).
  void BeginPass(VkCommandBuffer c, Pass pass, Framebuffer framebuffer, const Rect& area, uint32_t n_clears = 0,
                 const ClearValue* clears = nullptr) const {
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = ToVk(area);
    begin.clearValueCount = n_clears;
    begin.pClearValues = reinterpret_cast<const VkClearValue*>(clears);
    dfn_.vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    BO2ZM_MIRROR(BeginPass(c, pass, framebuffer, area, n_clears, clears));
  }
  void EndPass(VkCommandBuffer c) const {
    dfn_.vkCmdEndRenderPass(c);
    BO2ZM_MIRROR(EndPass(c));
  }
  // Clears area (layer 0) of one attachment of the open pass: color attachment color_index, or depth and stencil.
  void ClearAttachment(VkCommandBuffer c, Aspects aspects, uint32_t color_index, const ClearValue& value,
                       const Rect& area) const {
    VkClearAttachment attachment{};
    attachment.aspectMask = ToVk(aspects);
    attachment.colorAttachment = color_index;
    attachment.clearValue = ToVk(value);
    VkClearRect rectangle{};
    rectangle.rect = ToVk(area);
    rectangle.baseArrayLayer = 0;
    rectangle.layerCount = 1;
    dfn_.vkCmdClearAttachments(c, 1, &attachment, 1, &rectangle);
    BO2ZM_MIRROR(ClearAttachment(c, aspects, color_index, value, area));
  }
  void ClearColorImage(VkCommandBuffer c, VkImage image, Layout layout, const ClearColor* color, uint32_t count,
                       const Range* ranges) const {
    dfn_.vkCmdClearColorImage(c, image, VkImageLayout(layout), reinterpret_cast<const VkClearColorValue*>(color),
                              count, reinterpret_cast<const VkImageSubresourceRange*>(ranges));
    BO2ZM_MIRROR(ClearColorImage(c, image, *color, count, ranges));
  }
  void CopyBuffer(VkCommandBuffer c, VkBuffer source, VkBuffer target, uint32_t count,
                  const BufferCopy* regions) const {
    dfn_.vkCmdCopyBuffer(c, source, target, count, reinterpret_cast<const VkBufferCopy*>(regions));
    BO2ZM_MIRROR(CopyBuffer(c, source, target, count, regions));
  }
  void CopyBufferToImage(VkCommandBuffer c, VkBuffer source, VkImage target, Layout layout, uint32_t count,
                         const BufferImageCopy* regions) const {
    dfn_.vkCmdCopyBufferToImage(c, source, target, VkImageLayout(layout), count,
                                reinterpret_cast<const VkBufferImageCopy*>(regions));
    BO2ZM_MIRROR(CopyBufferToImage(c, source, target, count, regions));
  }
  void CopyImageToBuffer(VkCommandBuffer c, VkImage source, Layout layout, VkBuffer target, uint32_t count,
                         const BufferImageCopy* regions) const {
    dfn_.vkCmdCopyImageToBuffer(c, source, VkImageLayout(layout), target, count,
                                reinterpret_cast<const VkBufferImageCopy*>(regions));
    BO2ZM_MIRROR(Skipped("image to buffer copies"));
  }
  // memory: one memory barrier, or none (nullptr). Neither dependency flags nor buffer barriers are used.
  void Barrier(VkCommandBuffer c, Stages source, Stages target, const MemoryBarrier* memory, uint32_t images = 0,
               const ImageBarrier* image = nullptr) const {
    VkMemoryBarrier vk_memory{};
    if (memory) {
      vk_memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      vk_memory.srcAccessMask = memory->before;
      vk_memory.dstAccessMask = memory->after;
    }
    VkImageMemoryBarrier vk_images[4]{};
    assert(images <= 4);
    for (uint32_t i = 0; i < images && i < 4; ++i) {
      VkImageMemoryBarrier& b = vk_images[i];
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.srcAccessMask = image[i].before;
      b.dstAccessMask = image[i].after;
      b.oldLayout = VkImageLayout(image[i].old_layout);
      b.newLayout = VkImageLayout(image[i].new_layout);
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = image[i].image;
      b.subresourceRange = ToVk(image[i].range);
    }
    dfn_.vkCmdPipelineBarrier(c, source, target, 0, memory ? 1 : 0, memory ? &vk_memory : nullptr, 0, nullptr,
                              images, images ? vk_images : nullptr);
  }
  void BeginQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t index, VkQueryControlFlags flags) const {
    dfn_.vkCmdBeginQuery(c, pool, index, flags);
    BO2ZM_MIRROR(Skipped("occlusion queries"));
  }
  void EndQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t index) const { dfn_.vkCmdEndQuery(c, pool, index); }
  void CopyImage(VkCommandBuffer c, VkImage source, Layout source_layout, VkImage target,
                 Layout target_layout, uint32_t count, const ImageCopy* regions) const {
    loaded_.copy_image(c, source, VkImageLayout(source_layout), target, VkImageLayout(target_layout), count,
                       reinterpret_cast<const VkImageCopy*>(regions));
    BO2ZM_MIRROR(CopyImage(c, source, target, count, regions));
  }
  void ClearDepthStencilImage(VkCommandBuffer c, VkImage image, Layout layout, const ClearDepthStencil* value,
                              uint32_t count, const Range* ranges) const {
    loaded_.clear_depth(c, image, VkImageLayout(layout), reinterpret_cast<const VkClearDepthStencilValue*>(value),
                        count, reinterpret_cast<const VkImageSubresourceRange*>(ranges));
    BO2ZM_MIRROR(ClearDepthStencilImage(c, image, *value, count, ranges));
  }
  void Blit(VkCommandBuffer c, VkImage source, Layout source_layout, VkImage target,
            Layout target_layout, uint32_t count, const ImageBlit* regions, Filter filter) const {
    loaded_.blit(c, source, VkImageLayout(source_layout), target, VkImageLayout(target_layout), count,
                 reinterpret_cast<const VkImageBlit*>(regions), VkFilter(filter));
    BO2ZM_MIRROR(Blit(c, source, target, count, regions));
  }
  void WriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t index) const {
    loaded_.write_timestamp(c, stage, pool, index);
    BO2ZM_MIRROR(Skipped("timestamps"));
  }
  void ResetQueries(VkCommandBuffer c, VkQueryPool pool, uint32_t first, uint32_t count) const {
    dfn_.vkCmdResetQueryPool(c, pool, first, count);
  }

 private:
  const Functions& dfn_;
  Loaded loaded_;
};

// The work around the recorded commands: starting and ending a command buffer, submitting it with a fence, and
// waiting for or polling that fence. On Metal these are the command buffer's own commit and completion.
class Work {
 public:
  Work(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device) : dfn_(dfn), device_(device) {}

  // Its pool holds only this buffer: the whole pool is reset (the SDK table has no vkResetCommandBuffer).
  void Reset(VkCommandPool pool) const { dfn_.vkResetCommandPool(device_, pool, 0); }
  // For one submission.
  bool Begin(CommandList c) const {
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    BO2ZM_MIRROR(Begin(c));
    return dfn_.vkBeginCommandBuffer(c, &start) == VK_SUCCESS;
  }
  bool End(CommandList c) const { return dfn_.vkEndCommandBuffer(c) == VK_SUCCESS; }
  // The buffers in order, in one submission that signals `fence`.
  bool Submit(VkQueue queue, uint32_t count, const CommandList* buffers, VkFence fence) const {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = count;
    submit.pCommandBuffers = buffers;
    BO2ZM_MIRROR(Submit(count, buffers));
    return dfn_.vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS;
  }
  // Without waiting.
  bool Finished(VkFence fence) const { return dfn_.vkGetFenceStatus(device_, fence) == VK_SUCCESS; }
  void Wait(VkFence fence) const { dfn_.vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX); }
  // Unsignaled again, for its next submission.
  void Rearm(VkFence fence) const { dfn_.vkResetFences(device_, 1, &fence); }

  // A command pool on queue family `family`, for one primary command buffer (the whole pool is reset, see Reset).
  bool CreatePool(uint32_t family, VkCommandPool& pool) const {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.queueFamilyIndex = family;
    return dfn_.vkCreateCommandPool(device_, &info, nullptr, &pool) == VK_SUCCESS;
  }
  bool AllocateCommands(VkCommandPool pool, VkCommandBuffer& commands) const {
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    return dfn_.vkAllocateCommandBuffers(device_, &info, &commands) == VK_SUCCESS;
  }
  // Unsignaled.
  bool CreateFence(VkFence& fence) const {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return dfn_.vkCreateFence(device_, &info, nullptr, &fence) == VK_SUCCESS;
  }
  void DestroyPool(VkCommandPool pool) const {
    if (pool != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pool, nullptr);
  }
  void DestroyFence(VkFence fence) const {
    if (fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fence, nullptr);
  }

  // A pool of `count` queries, or VK_NULL_HANDLE if the driver could not make it.
  VkQueryPool CreateQueries(QueryKind kind, uint32_t count) const {
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    switch (kind) {
      case QueryKind::kTimestamp:
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        break;
      case QueryKind::kOcclusion:
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        break;
      case QueryKind::kPipelineStatistics:
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        break;
    }
    info.queryCount = count;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (dfn_.vkCreateQueryPool(device_, &info, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pool;
  }
  void DestroyQueries(VkQueryPool pool) const {
    if (pool != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, pool, nullptr);
  }

 private:
  const rex::ui::vulkan::VulkanDevice::Functions& dfn_;
  VkDevice device_;
};

// A buffer the CPU writes and the GPU reads, in its own allocation, mapped for as long as it lives.
struct HostBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* data = nullptr;
  VkDeviceSize size_real = 0;  // of the allocation (the memory requirements' size)
  VkDeviceAddress address = 0;  // with kBufferAddress
  uint32_t type = UINT32_MAX;   // the memory type
};

// The step at which CreateHostBuffer stopped.
enum class HostBufferResult : uint8_t {
  kDone,
  kNoBuffer,     // vkCreateBuffer failed
  kWrongMemory,  // no memory type for this buffer
  kNoMemory,     // vkAllocateMemory or vkBindBufferMemory failed
  kNoMap,        // vkMapMemory failed
  kNoAddress,    // the device address is 0
};

// vkCreateBuffer of `size` bytes for `usage`, then its memory from the type `choose(memoryTypeBits)` returns
// (UINT32_MAX: none fits), with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT if usage has kBufferAddress, bound, mapped whole,
// and with kBufferAddress its device address through `address_of`. `out.type` is the memory type. What was made
// before a failure stays in `out`, for the caller to destroy.
template <typename Choose>
HostBufferResult CreateHostBuffer(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                                  PFN_vkGetBufferDeviceAddress address_of, VkDeviceSize size, BufferUsage usage,
                                  Choose&& choose, HostBuffer& out) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
    return HostBufferResult::kNoBuffer;
  }
  VkMemoryRequirements requirements;
  dfn.vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
  out.type = choose(requirements.memoryTypeBits);
  if (out.type == UINT32_MAX) {
    return HostBufferResult::kWrongMemory;
  }
  const bool addressed = (usage & kBufferAddress) != 0;
  VkMemoryAllocateFlagsInfo flags{};
  flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo reserve{};
  reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  reserve.pNext = addressed ? &flags : nullptr;
  reserve.allocationSize = requirements.size;
  reserve.memoryTypeIndex = out.type;
  if (dfn.vkAllocateMemory(device, &reserve, nullptr, &out.memory) != VK_SUCCESS) {
    return HostBufferResult::kNoMemory;
  }
  out.size_real = requirements.size;
  if (dfn.vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
    return HostBufferResult::kNoMemory;
  }
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    return HostBufferResult::kNoMap;
  }
  out.data = static_cast<uint8_t*>(mapped);
  if (addressed) {
    VkBufferDeviceAddressInfo where{};
    where.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    where.buffer = out.buffer;
    out.address = address_of(device, &where);
    if (!out.address) {
      return HostBufferResult::kNoAddress;
    }
  }
  BO2ZM_MIRROR(BufferCreated(out.buffer, out.data, size));
  return HostBufferResult::kDone;
}

// The same in the one memory type `type`.
inline HostBufferResult CreateHostBuffer(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                                         PFN_vkGetBufferDeviceAddress address_of, VkDeviceSize size,
                                         BufferUsage usage, uint32_t type, HostBuffer& out) {
  return CreateHostBuffer(dfn, device, address_of, size, usage,
                          [type](uint32_t bits) { return ((bits >> type) & 1) ? type : UINT32_MAX; }, out);
}

// Host access to a whole allocation (offset 0, VK_WHOLE_SIZE). Metal's shared storage needs neither the mapping nor
// the flushes; its backend makes them no-ops.
inline bool Map(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDeviceMemory memory,
                void*& out) {
  return dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &out) == VK_SUCCESS;
}
inline void Unmap(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDeviceMemory memory) {
  dfn.vkUnmapMemory(device, memory);
}
// Makes the GPU's writes visible to the CPU (non-coherent memory).
inline void Invalidate(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDeviceMemory memory) {
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = memory;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
}
// Makes the CPU's writes visible to the GPU (non-coherent memory).
inline void Flush(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDeviceMemory memory) {
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = memory;
  range.size = VK_WHOLE_SIZE;
  dfn.vkFlushMappedMemoryRanges(device, 1, &range);
}

// Object destruction, one overload per handle type (each is its own type on the 64-bit targets the renderer builds
// for). Metal releases the matching objects; memory is freed with Free.
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkPipeline object) {
  BO2ZM_MIRROR(PipelineDestroyed(object));
  dfn.vkDestroyPipeline(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkPipelineLayout object) {
  dfn.vkDestroyPipelineLayout(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                    VkDescriptorSetLayout object) {
  dfn.vkDestroyDescriptorSetLayout(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDescriptorPool object) {
  dfn.vkDestroyDescriptorPool(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkRenderPass object) {
  dfn.vkDestroyRenderPass(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkImageView object) {
  BO2ZM_MIRROR(ViewDestroyed(object));
  dfn.vkDestroyImageView(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkImage object) {
  BO2ZM_MIRROR(ImageDestroyed(object));
  dfn.vkDestroyImage(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkBuffer object) {
  BO2ZM_MIRROR(BufferDestroyed(object));
  dfn.vkDestroyBuffer(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkCommandPool object) {
  dfn.vkDestroyCommandPool(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkFence object) {
  dfn.vkDestroyFence(device, object, nullptr);
}
inline void Free(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDeviceMemory memory) {
  dfn.vkFreeMemory(device, memory, nullptr);
}
static_assert(VkImageViewType(kViewCube) == VK_IMAGE_VIEW_TYPE_CUBE &&
                  VkImageViewType(kView3D) == VK_IMAGE_VIEW_TYPE_3D && kAllLevels == VK_REMAINING_MIP_LEVELS,
              "view numbering");
static_assert(sizeof(void*) == 8, "the Destroy overloads need distinct handle types");

// A render pass for `desc`, one subpass: the colour attachments in order, then the depth one. True and the pass
// in `out` if created. Metal has no pass object: the pass descriptor is filled per pass from the same desc.
inline bool CreatePass(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, const PassDesc& desc,
                       VkRenderPass& out) {
  VkAttachmentDescription attachments[5]{};
  VkAttachmentReference colors[4]{};
  VkAttachmentReference depth{};
  uint32_t n = 0;
  auto add = [&](const PassAttachment& a) {
    VkAttachmentDescription& v = attachments[n];
    v.format = VkFormat(a.format);
    v.samples = VK_SAMPLE_COUNT_1_BIT;
    v.loadOp = VkAttachmentLoadOp(a.load);
    v.storeOp = VkAttachmentStoreOp(a.store);
    v.stencilLoadOp = VkAttachmentLoadOp(a.stencil_load);
    v.stencilStoreOp = VkAttachmentStoreOp(a.stencil_store);
    v.initialLayout = VkImageLayout(a.initial);
    v.finalLayout = VkImageLayout(a.final);
    return VkAttachmentReference{n++, VkImageLayout(a.used)};
  };
  const uint32_t color_count = desc.color_count < 4 ? desc.color_count : 4;
  for (uint32_t i = 0; i < color_count; ++i) {
    colors[i] = add(desc.colors[i]);
  }
  if (desc.has_depth) {
    depth = add(desc.depth);
  }
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = color_count;
  subpass.pColorAttachments = colors;
  subpass.pDepthStencilAttachment = desc.has_depth ? &depth : nullptr;
  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = n;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  const bool made = dfn.vkCreateRenderPass(device, &info, nullptr, &out) == VK_SUCCESS;
  if (made) {
    BO2ZM_MIRROR(PassCreated(out, desc));
  }
  return made;
}

// The attachments of a render pass, `width` x `height`, one layer: true and the framebuffer in `out` if created.
// Metal has no framebuffer object: its render pass descriptor names the textures directly.
inline bool CreateFramebuffer(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkRenderPass pass,
                              uint32_t count, const VkImageView* views, uint32_t width, uint32_t height,
                              VkFramebuffer& out) {
  VkFramebufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = pass;
  info.attachmentCount = count;
  info.pAttachments = views;
  info.width = width;
  info.height = height;
  info.layers = 1;
  const bool made = dfn.vkCreateFramebuffer(device, &info, nullptr, &out) == VK_SUCCESS;
  if (made) {
    BO2ZM_MIRROR(FramebufferCreated(out, pass, count, views, width, height));
  }
  return made;
}

inline void DestroyFramebuffer(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                               VkFramebuffer framebuffer) {
  BO2ZM_MIRROR(FramebufferDestroyed(framebuffer));
  dfn.vkDestroyFramebuffer(device, framebuffer, nullptr);
}

// A descriptor set layout with `count` bindings (at most 8): true and the layout in `out` if created.
// `bindless` for the heaps: every binding partially bound, updated after bind and while unused but pending,
// and the layout for update-after-bind pools.
inline bool CreateSetLayout(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                            const SetBinding* bindings, uint32_t count, bool bindless, VkDescriptorSetLayout& out) {
  VkDescriptorSetLayoutBinding vk[8]{};
  VkDescriptorBindingFlags flags_binding[8]{};
  if (count > 8) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    vk[i].binding = bindings[i].binding;
    vk[i].descriptorType = VkDescriptorType(bindings[i].kind);
    vk[i].descriptorCount = bindings[i].count;
    vk[i].stageFlags = bindings[i].stages;
    vk[i].pImmutableSamplers = bindings[i].immutable_samplers;
    flags_binding[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                       VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
  }
  VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
  flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flags.bindingCount = count;
  flags.pBindingFlags = flags_binding;
  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (bindless) {
    info.pNext = &flags;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  }
  info.bindingCount = count;
  info.pBindings = vk;
  const bool made = dfn.vkCreateDescriptorSetLayout(device, &info, nullptr, &out) == VK_SUCCESS;
  if (made) {
    BO2ZM_MIRROR(SetLayoutCreated(out, bindings, count));
  }
  return made;
}

// A pipeline layout: `set_count` set layouts and `range_count` push constant ranges. True and the layout in
// `out` if created. Metal has no pipeline layout: the binding contract of tools/metal fixes the slots.
inline bool CreatePipelineLayout(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                                 uint32_t set_count, const VkDescriptorSetLayout* sets, uint32_t range_count,
                                 const PushRange* ranges, VkPipelineLayout& out) {
  VkPipelineLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = set_count;
  info.pSetLayouts = sets;
  info.pushConstantRangeCount = range_count;
  info.pPushConstantRanges = reinterpret_cast<const VkPushConstantRange*>(ranges);
  return dfn.vkCreatePipelineLayout(device, &info, nullptr, &out) == VK_SUCCESS;
}

// A descriptor pool for `counts`: true and the pool in `out` if created. The sizes are listed in a fixed
// order (sampled images, samplers, uniform buffers, dynamic uniform buffers), kinds with no descriptors left out.
inline bool CreatePool(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, const PoolCounts& counts,
                       VkDescriptorPool& out) {
  VkDescriptorPoolSize sizes[4];
  uint32_t n = 0;
  if (counts.sampled_images) sizes[n++] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, counts.sampled_images};
  if (counts.samplers) sizes[n++] = {VK_DESCRIPTOR_TYPE_SAMPLER, counts.samplers};
  if (counts.uniform_buffers) sizes[n++] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, counts.uniform_buffers};
  if (counts.uniform_buffers_dynamic) {
    sizes[n++] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, counts.uniform_buffers_dynamic};
  }
  VkDescriptorPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.flags = counts.update_after_bind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
  info.maxSets = counts.max_sets;
  info.poolSizeCount = n;
  info.pPoolSizes = sizes;
  return dfn.vkCreateDescriptorPool(device, &info, nullptr, &out) == VK_SUCCESS;
}

// `count` sets from `pool`, one per layout in `layouts`, into `out`: true if allocated.
inline bool AllocateSets(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDescriptorPool pool,
                         uint32_t count, const VkDescriptorSetLayout* layouts, VkDescriptorSet* out) {
  VkDescriptorSetAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  info.descriptorPool = pool;
  info.descriptorSetCount = count;
  info.pSetLayouts = layouts;
  const bool made = dfn.vkAllocateDescriptorSets(device, &info, out) == VK_SUCCESS;
  if (made) {
    BO2ZM_MIRROR(SetsAllocated(count, layouts, out));
  }
  return made;
}

// Descriptor writes, one vkUpdateDescriptorSets each. Metal writes the same slots into the heaps' argument buffers.
// A sampled image at `element` of `binding` (the bindless heaps use binding 0 and the slot as the element).
inline void WriteSampledImage(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDescriptorSet set,
                              uint32_t binding, uint32_t element, VkImageView view,
                              VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) {
  VkDescriptorImageInfo image{};
  image.imageView = view;
  image.imageLayout = layout;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = binding;
  write.dstArrayElement = element;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  BO2ZM_MIRROR(WriteSampledImage(set, binding, element, view));
}

inline void WriteSampler(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDescriptorSet set,
                         uint32_t binding, uint32_t element, VkSampler sampler) {
  VkDescriptorImageInfo image{};
  image.sampler = sampler;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = binding;
  write.dstArrayElement = element;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  write.pImageInfo = &image;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  BO2ZM_MIRROR(WriteSampler(set, binding, element, sampler));
}

// Uniform buffers at bindings 0 to count - 1 of `set`, in one update. `dynamic`: offsets given when the set is bound
// (set 4 of the pack shaders).
inline void WriteUniforms(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkDescriptorSet set,
                          uint32_t first_binding, uint32_t count, const VkDescriptorBufferInfo* buffers, bool dynamic) {
  VkWriteDescriptorSet writes[4]{};
  for (uint32_t b = 0; b < count; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = set;
    writes[b].dstBinding = first_binding + b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[b].pBufferInfo = &buffers[b];
  }
  dfn.vkUpdateDescriptorSets(device, count, writes, 0, nullptr);
  for (uint32_t b = 0; b < count; ++b) {
    BO2ZM_MIRROR(WriteUniform(set, first_binding + b, buffers[b].buffer, buffers[b].offset));
  }
}

// A sampler: true and the sampler in `out` if the driver created it. Every field the description does not have is
// zero, as in the create info the renderer filled itself.
inline bool CreateSampler(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                          const SamplerDesc& desc, VkSampler& out) {
  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = VkFilter(desc.mag);
  info.minFilter = VkFilter(desc.min);
  info.mipmapMode = VkSamplerMipmapMode(desc.mip);
  info.addressModeU = VkSamplerAddressMode(desc.u);
  info.addressModeV = VkSamplerAddressMode(desc.v);
  info.addressModeW = VkSamplerAddressMode(desc.w);
  info.mipLodBias = desc.lod_bias;
  info.anisotropyEnable = desc.anisotropy ? VK_TRUE : VK_FALSE;
  info.maxAnisotropy = desc.anisotropy_max;
  info.minLod = desc.lod_min;
  info.maxLod = desc.lod_max;
  info.borderColor = VkBorderColor(desc.border);
  const bool made = dfn.vkCreateSampler(device, &info, nullptr, &out) == VK_SUCCESS;
  if (made) {
    BO2ZM_MIRROR(SamplerCreated(out, desc));
  }
  return made;
}

inline void DestroySampler(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkSampler sampler) {
  BO2ZM_MIRROR(SamplerDestroyed(sampler));
  dfn.vkDestroySampler(device, sampler, nullptr);
}

// A shader from its SPIR-V (`bytes` long): true and the module in `out` if the driver created it. Metal's backend
// looks up the same shader, translated ahead of time, in its libraries.
inline bool CreateShader(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, const void* code,
                         size_t bytes, VkShaderModule& out) {
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = bytes;
  info.pCode = static_cast<const uint32_t*>(code);
  return dfn.vkCreateShaderModule(device, &info, nullptr, &out) == VK_SUCCESS;
}

inline void DestroyShader(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                          VkShaderModule shader) {
  BO2ZM_MIRROR(ShaderDestroyed(shader));
  dfn.vkDestroyShaderModule(device, shader, nullptr);
}

inline VkResult CreateView(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkImage image,
                           const ViewDesc& desc, VkImageView& out) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = VkImageViewType(desc.type);
  info.format = VkFormat(desc.format);
  info.components = {VkComponentSwizzle(desc.swizzle[0]), VkComponentSwizzle(desc.swizzle[1]),
                     VkComponentSwizzle(desc.swizzle[2]), VkComponentSwizzle(desc.swizzle[3])};
  info.subresourceRange = {desc.aspects, 0, desc.levels, 0, desc.layers};
  const VkResult result = dfn.vkCreateImageView(device, &info, nullptr, &out);
  if (result == VK_SUCCESS) {
    BO2ZM_MIRROR(ViewCreated(out, image, desc));
  }
  return result;
}

inline VkImageCreateInfo ImageInfo(const ImageDesc& desc) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = desc.volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.flags = desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.format = VkFormat(desc.format);
  info.extent = {desc.width, desc.height, desc.depth};
  info.mipLevels = desc.levels;
  info.arrayLayers = desc.layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = desc.usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return info;
}

// An image without memory (bound later, from a pool).
inline bool CreateImage(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, const ImageDesc& desc,
                        VkImage& out) {
  const VkImageCreateInfo info = ImageInfo(desc);
  const bool made = dfn.vkCreateImage(device, &info, nullptr, &out) == VK_SUCCESS;
  if (made) {
    BO2ZM_MIRROR(ImageCreated(out, desc));
  }
  return made;
}

// An image with its own device-local allocation.
inline bool CreateDedicatedImage(const rex::ui::vulkan::VulkanDevice* device, const ImageDesc& desc, VkImage& out,
                                 VkDeviceMemory& memory) {
  const bool made = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
      device, ImageInfo(desc), rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, out, memory);
  if (made) {
    BO2ZM_MIRROR(ImageCreated(out, desc));
  }
  return made;
}

static_assert(sizeof(Requirements) == sizeof(VkMemoryRequirements) &&
                  offsetof(Requirements, size) == offsetof(VkMemoryRequirements, size) &&
                  offsetof(Requirements, alignment) == offsetof(VkMemoryRequirements, alignment) &&
                  offsetof(Requirements, types) == offsetof(VkMemoryRequirements, memoryTypeBits),
              "gpu::Requirements has VkMemoryRequirements' layout");

inline gpu::Requirements Requirements(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                                      VkImage image) {
  VkMemoryRequirements req{};
  dfn.vkGetImageMemoryRequirements(device, image, &req);
  return gpu::Requirements{req.size, req.alignment, req.memoryTypeBits};
}

inline VkResult BindImage(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkImage image,
                          VkDeviceMemory memory, VkDeviceSize offset) {
  return dfn.vkBindImageMemory(device, image, memory, offset);
}

// A PipelineState as the create infos of a graphics pipeline. blend.pAttachments points into blends, so the
// structure cannot be copied.
struct FixedState {
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  VkPipelineViewportStateCreateInfo view{};
  VkPipelineRasterizationStateCreateInfo rasterization{};
  VkPipelineMultisampleStateCreateInfo sampling{};
  VkPipelineDepthStencilStateCreateInfo depth{};
  std::array<VkPipelineColorBlendAttachmentState, 4> blends{};
  VkPipelineColorBlendStateCreateInfo blend{};

  explicit FixedState(const PipelineState& s) {
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VkPrimitiveTopology(s.topology);
    assembly.primitiveRestartEnable = s.restart ? VK_TRUE : VK_FALSE;

    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;

    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = s.cull;
    rasterization.frontFace = VkFrontFace(s.front_face);
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = s.depth_bias ? VK_TRUE : VK_FALSE;
    rasterization.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;

    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const auto face = [](const StencilOps& o) {
      VkStencilOpState f{};
      f.failOp = VkStencilOp(o.fail);
      f.passOp = VkStencilOp(o.pass);
      f.depthFailOp = VkStencilOp(o.depth_fail);
      f.compareOp = VkCompareOp(o.compare);
      return f;
    };
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = s.depth_test;
    depth.depthWriteEnable = s.depth_write;
    depth.depthCompareOp = VkCompareOp(s.depth_compare);
    depth.stencilTestEnable = s.stencil_test;
    depth.front = face(s.front);
    depth.back = face(s.back);

    for (uint32_t i = 0; i < s.n_colors && i < 4; ++i) {
      const BlendTarget& t = s.blends[i];
      VkPipelineColorBlendAttachmentState& b = blends[i];
      b.blendEnable = t.enable;
      b.srcColorBlendFactor = VkBlendFactor(t.equation.src_color);
      b.dstColorBlendFactor = VkBlendFactor(t.equation.dst_color);
      b.colorBlendOp = VkBlendOp(t.equation.color_op);
      b.srcAlphaBlendFactor = VkBlendFactor(t.equation.src_alpha);
      b.dstAlphaBlendFactor = VkBlendFactor(t.equation.dst_alpha);
      b.alphaBlendOp = VkBlendOp(t.equation.alpha_op);
      b.colorWriteMask = t.mask;
    }
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = s.n_colors;
    blend.pAttachments = blends.data();
  }
  FixedState(const FixedState&) = delete;
  FixedState& operator=(const FixedState&) = delete;
};

static_assert(sizeof(VertexAttribute) == sizeof(VkVertexInputAttributeDescription) &&
              offsetof(VertexAttribute, offset) == offsetof(VkVertexInputAttributeDescription, offset));

/*
 * Creates a graphics pipeline from its description. The dynamic states are declared in a fixed order: viewport and
 * scissor; blend constants, stencil compare mask, write mask and reference, depth bias; then the extended groups.
 * flags is Vulkan's (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT for the cache probe).
 */
inline VkResult CreateGraphicsPipeline(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                                       PipelineCache cache, const GraphicsPipelineDesc& desc, Pipeline& out,
                                       VkPipelineCreateFlags flags = 0) {
  VkSpecializationMapEntry map[8]{};
  const uint32_t n_constants = desc.n_constants < 8 ? desc.n_constants : 8;
  for (uint32_t i = 0; i < n_constants; ++i) {
    map[i] = {i, uint32_t(i * sizeof(uint32_t)), sizeof(uint32_t)};
  }
  const VkSpecializationInfo specialization{n_constants, map, n_constants * sizeof(uint32_t), desc.constants};
  VkPipelineShaderStageCreateInfo stages[2]{};
  const Shader modules[2] = {desc.vs, desc.ps};
  const VkShaderStageFlagBits kinds[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
  for (uint32_t i = 0; i < 2; ++i) {
    stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[i].stage = kinds[i];
    stages[i].module = modules[i];
    stages[i].pName = "main";
    if (n_constants && (desc.constant_stages & kinds[i])) {
      stages[i].pSpecializationInfo = &specialization;
    }
  }

  std::vector<VkVertexInputBindingDescription> bindings;
  bindings.reserve(desc.n_bindings);
  for (uint32_t i = 0; i < desc.n_bindings; ++i) {
    bindings.push_back({i, desc.strides[i], VK_VERTEX_INPUT_RATE_VERTEX});
  }
  VkPipelineVertexInputStateCreateInfo vertices{};
  vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertices.vertexBindingDescriptionCount = desc.n_bindings;
  vertices.pVertexBindingDescriptions = bindings.data();
  vertices.vertexAttributeDescriptionCount = desc.n_attributes;
  vertices.pVertexAttributeDescriptions = reinterpret_cast<const VkVertexInputAttributeDescription*>(desc.attributes);

  const FixedState fixed(*desc.state);

  static constexpr VkDynamicState kView[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  static constexpr VkDynamicState kDraw[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                             VK_DYNAMIC_STATE_DEPTH_BIAS};
  static constexpr VkDynamicState kEds12[] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr VkDynamicState kEds3[] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                             VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                             VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
  VkDynamicState dynamic_states[24];
  uint32_t n_dynamic = 0;
  const auto add = [&](DynamicGroups group, const auto& states) {
    if (desc.dynamic & group) {
      for (const VkDynamicState state : states) {
        dynamic_states[n_dynamic++] = state;
      }
    }
  };
  add(kDynamicView, kView);
  add(kDynamicDraw, kDraw);
  add(kDynamicEds12, kEds12);
  add(kDynamicEds3, kEds3);
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = n_dynamic;
  dynamic.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = desc.ps ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &vertices;
  info.pInputAssemblyState = &fixed.assembly;
  info.pViewportState = &fixed.view;
  info.pRasterizationState = &fixed.rasterization;
  info.pMultisampleState = &fixed.sampling;
  info.pDepthStencilState = desc.with_depth ? &fixed.depth : nullptr;
  info.pColorBlendState = &fixed.blend;
  info.pDynamicState = &dynamic;
  info.layout = desc.layout;
  info.renderPass = desc.pass;
  info.basePipelineIndex = -1;
  info.flags = flags;
  out = VK_NULL_HANDLE;
  const VkResult result = dfn.vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &out);
  if (result == VK_SUCCESS) {
    BO2ZM_MIRROR(PipelineCreated(out, desc));
  }
  return result;
}

// A compute pipeline from one shader (entry main) and its layout.
inline bool CreateComputePipeline(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, Shader shader,
                                  PipelineLayout layout, Pipeline& out) {
  VkComputePipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = shader;
  info.stage.pName = "main";
  info.layout = layout;
  if (dfn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &out) != VK_SUCCESS) {
    out = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

// The on-disk pipeline cache's functions, which the device's function table does not load.
struct PipelineCacheFunctions {
  PFN_vkCreatePipelineCache create = nullptr;
  PFN_vkGetPipelineCacheData data = nullptr;
  PFN_vkDestroyPipelineCache destroy = nullptr;
};

inline PipelineCacheFunctions LoadPipelineCache(const rex::ui::vulkan::VulkanDevice& device) {
  const auto& ifn = device.vulkan_instance()->functions();
  PipelineCacheFunctions f;
  f.create = reinterpret_cast<PFN_vkCreatePipelineCache>(ifn.vkGetDeviceProcAddr(device.device(),
                                                                                  "vkCreatePipelineCache"));
  f.data = reinterpret_cast<PFN_vkGetPipelineCacheData>(ifn.vkGetDeviceProcAddr(device.device(),
                                                                                 "vkGetPipelineCacheData"));
  f.destroy = reinterpret_cast<PFN_vkDestroyPipelineCache>(ifn.vkGetDeviceProcAddr(device.device(),
                                                                                    "vkDestroyPipelineCache"));
  return f;
}

// A pipeline cache seeded with data (empty: a new one). false if the driver refuses it.
inline bool CreatePipelineCache(const PipelineCacheFunctions& f, VkDevice device, const void* data, size_t bytes,
                                PipelineCache& out) {
  VkPipelineCacheCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = bytes;
  info.pInitialData = bytes ? data : nullptr;
  return f.create(device, &info, nullptr, &out) == VK_SUCCESS;
}

// The cache's data size (data == nullptr) or the data itself; false on failure. Incomplete data counts as data.
inline bool PipelineCacheData(const PipelineCacheFunctions& f, VkDevice device, PipelineCache cache, size_t& bytes,
                              void* data) {
  const VkResult result = f.data(device, cache, &bytes, data);
  return result == VK_SUCCESS || (data && result == VK_INCOMPLETE);
}

inline void DestroyPipelineCache(const PipelineCacheFunctions& f, VkDevice device, PipelineCache cache) {
  f.destroy(device, cache, nullptr);
}

// The device's capabilities. queue_family is the family the renderer submits to (its timestamps). The depth format
// is the first of D24S8 and D32S8 that can be drawn in and also be sampled, copied from and copied to; if neither
// can do all of that, the first one that can be drawn in.
inline Caps CapsOf(const rex::ui::vulkan::VulkanDevice& device, uint32_t queue_family) {
  const auto& p = device.properties();
  Caps caps;
  std::strncpy(caps.device_name, p.deviceName, sizeof(caps.device_name) - 1);
  caps.int64 = p.shaderInt64;
  caps.buffer_address = p.bufferDeviceAddress;
  caps.runtime_arrays = p.runtimeDescriptorArray;
  caps.dynamic_indexing = p.shaderSampledImageArrayDynamicIndexing;
  caps.partially_bound = p.descriptorBindingPartiallyBound;
  caps.update_after_bind = p.descriptorBindingSampledImageUpdateAfterBind;
  caps.update_unused_pending = p.descriptorBindingUpdateUnusedWhilePending;
  caps.mirror_clamp_to_edge = p.samplerMirrorClampToEdge;
  caps.anisotropy = p.samplerAnisotropy;
  caps.max_anisotropy = p.maxSamplerAnisotropy;
  caps.uniform_alignment = p.minUniformBufferOffsetAlignment;
  caps.occlusion_precise = p.occlusionQueryPrecise;
  caps.pipeline_statistics = p.pipelineStatisticsQuery;
  caps.pipeline_cache_control = p.pipelineCreationCacheControl;
  const PipelineCacheFunctions cache = LoadPipelineCache(device);
  caps.pipeline_cache = cache.create && cache.data && cache.destroy;
  caps.api_version = p.apiVersion;
  caps.dynamic_state12 = p.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0);
#if defined(REX_UI_VULKAN_DYNAMIC_STATE3)
  caps.dynamic_state3 = device.extensions().ext_EXT_extended_dynamic_state3 &&
                        p.extendedDynamicState3ColorBlendEnable && p.extendedDynamicState3ColorBlendEquation &&
                        p.extendedDynamicState3ColorWriteMask;
#endif
  const auto& ifn = device.vulkan_instance()->functions();
  const VkPhysicalDevice physical = device.physical_device();
  uint32_t families = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
  std::vector<VkQueueFamilyProperties> queues(families);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues.data());
  VkPhysicalDeviceProperties physical_properties{};
  ifn.vkGetPhysicalDeviceProperties(physical, &physical_properties);
  if (queue_family < families && queues[queue_family].timestampValidBits &&
      physical_properties.limits.timestampPeriod > 0.0f) {
    caps.timestamp_period_ns = physical_properties.limits.timestampPeriod;
  }
  // Attachment, copy and clear source and destination, and sampling of the resolved ones (shadows).
  const VkFormatFeatureFlags kUsesDepth = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  // Drawing the shadow map smaller needs a blit on depth to scale it up when resolving it.
  const VkFormatFeatureFlags kUsesScaled = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
  VkFormatFeatureFlags uses_chosen = 0;
  for (const VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    VkFormatProperties properties{};
    ifn.vkGetPhysicalDeviceFormatProperties(physical, candidate, &properties);
    const VkFormatFeatureFlags uses = properties.optimalTilingFeatures & kUsesDepth;
    if (!(uses & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      continue;
    }
    if (caps.depth_format == VK_FORMAT_UNDEFINED || uses == kUsesDepth) {
      caps.depth_format = candidate;
      uses_chosen = uses;
      caps.depth_scalable = (properties.optimalTilingFeatures & kUsesScaled) == kUsesScaled;
    }
    if (uses == kUsesDepth) {
      break;
    }
  }
  caps.depth_sampled = (uses_chosen & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
  caps.depth_copy_source = (uses_chosen & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
  caps.depth_copy_target = (uses_chosen & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
  return caps;
}

}  // namespace vulkan

}  // namespace bo2zm::native::gpu
