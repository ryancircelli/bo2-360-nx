// Native renderer: the types of bo2zm_gpu.h converted to Vulkan's, for the Vulkan backend.

#pragma once

#include "bo2zm_gpu.h"

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

static_assert(std::is_same_v<bo2zm::native::gpu::CommandList, VkCommandBuffer>);
static_assert(std::is_same_v<bo2zm::native::gpu::Image, VkImage>);
static_assert(std::is_same_v<bo2zm::native::gpu::ImageView, VkImageView>);
static_assert(std::is_same_v<bo2zm::native::gpu::Memory, VkDeviceMemory>);
static_assert(std::is_same_v<bo2zm::native::gpu::Sampler, VkSampler>);
static_assert(std::is_same_v<bo2zm::native::gpu::SetLayout, VkDescriptorSetLayout>);
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
    return dfn_.vkBeginCommandBuffer(c, &start) == VK_SUCCESS;
  }
  bool End(CommandList c) const { return dfn_.vkEndCommandBuffer(c) == VK_SUCCESS; }
  // The buffers in order, in one submission that signals `fence`.
  bool Submit(VkQueue queue, uint32_t count, const CommandList* buffers, VkFence fence) const {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = count;
    submit.pCommandBuffers = buffers;
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
  dfn.vkDestroyImageView(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkImage object) {
  dfn.vkDestroyImage(device, object, nullptr);
}
inline void Destroy(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkBuffer object) {
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
  return dfn.vkCreateRenderPass(device, &info, nullptr, &out) == VK_SUCCESS;
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
  return dfn.vkCreateFramebuffer(device, &info, nullptr, &out) == VK_SUCCESS;
}

inline void DestroyFramebuffer(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                               VkFramebuffer framebuffer) {
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
  return dfn.vkCreateDescriptorSetLayout(device, &info, nullptr, &out) == VK_SUCCESS;
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
  return dfn.vkAllocateDescriptorSets(device, &info, out) == VK_SUCCESS;
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
  return dfn.vkCreateSampler(device, &info, nullptr, &out) == VK_SUCCESS;
}

inline void DestroySampler(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkSampler sampler) {
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
  return dfn.vkCreateImageView(device, &info, nullptr, &out);
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
  return dfn.vkCreateImage(device, &info, nullptr, &out) == VK_SUCCESS;
}

// An image with its own device-local allocation.
inline bool CreateDedicatedImage(const rex::ui::vulkan::VulkanDevice* device, const ImageDesc& desc, VkImage& out,
                                 VkDeviceMemory& memory) {
  return rex::ui::vulkan::util::CreateDedicatedAllocationImage(
      device, ImageInfo(desc), rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, out, memory);
}

inline VkMemoryRequirements Requirements(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device,
                                         VkImage image) {
  VkMemoryRequirements req{};
  dfn.vkGetImageMemoryRequirements(device, image, &req);
  return req;
}

inline VkResult BindImage(const rex::ui::vulkan::VulkanDevice::Functions& dfn, VkDevice device, VkImage image,
                          VkDeviceMemory memory, VkDeviceSize offset) {
  return dfn.vkBindImageMemory(device, image, memory, offset);
}

}  // namespace vulkan

}  // namespace bo2zm::native::gpu
