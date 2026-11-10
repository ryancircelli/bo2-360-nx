// Native renderer, Metal backend: the mirror, the bring-up path of the Metal drawing.
//
// With bo2zm_native_metal_mirror the Vulkan renderer runs as always, and every call the layer makes (the
// creation functions of gpu::vulkan and the methods of its Recorder and Work) is also given to the mirror, which
// does the same work on Metal objects of its own, keyed by the Vulkan handles. Both images exist in one run, so the
// Metal frame can be compared with the Vulkan one, and the renderer's logic is not touched while the Metal side
// grows. Without the cvar Get() is null and the layer makes one test per call.
//
// What the mirror does not do yet is logged once and skipped: queries and timestamps (stubs), scaled blits, the
// renderer's own shaders that are not in the shader pack (output, conversion copies, GPU skinning,
// volume gathers), and anything that writes guest memory back. Barriers are no-ops: Metal tracks the hazards of
// the objects the mirror makes.
//
// Every entry point takes one lock, so lists recorded on several threads stay coherent. Submit commits the lists in
// order and waits for them: slow, but nothing the CPU writes later can reach a Metal command still running.

#pragma once

#include "../bo2zm_gpu.h"

#include <cstdint>

namespace bo2zm::native::gpu::metal {

class Device;

class Mirror {
 public:
  // The mirror, or null when it is off.
  static Mirror* Get() { return instance_; }
  // Turns it on (before the renderer makes anything). The device must outlive it.
  static void Enable(Device& device);
  static void Disable();

  // Resources. Buffers are host memory: the mirror reads the same bytes, from a Metal buffer made over them.
  void BufferCreated(Buffer buffer, void* data, Size size);
  void BufferDestroyed(Buffer buffer);
  void ImageCreated(Image image, const ImageDesc& desc);
  void ImageDestroyed(Image image);
  void ViewCreated(ImageView view, Image image, const ViewDesc& desc);
  void ViewDestroyed(ImageView view);
  void SamplerCreated(Sampler sampler, const SamplerDesc& desc);
  void SamplerDestroyed(Sampler sampler);
  // Which pack shader a module holds (its container hash; early_z for the EarlyFragmentTests copy). Modules
  // without a tag (the renderer's own shaders) give pipelines the mirror skips.
  void ShaderTagged(Shader shader, bool vertex, uint64_t hash, bool early_z);
  void ShaderDestroyed(Shader shader);
  void SetLayoutCreated(SetLayout layout, const SetBinding* bindings, uint32_t count);
  void SetsAllocated(uint32_t count, const SetLayout* layouts, const Set* sets);
  void WriteSampledImage(Set set, uint32_t binding, uint32_t element, ImageView view);
  void WriteSampler(Set set, uint32_t binding, uint32_t element, Sampler sampler);
  void WriteUniform(Set set, uint32_t binding, Buffer buffer, Size offset);
  void PassCreated(Pass pass, const PassDesc& desc);
  void FramebufferCreated(Framebuffer framebuffer, Pass pass, uint32_t count, const ImageView* views,
                          uint32_t width, uint32_t height);
  void FramebufferDestroyed(Framebuffer framebuffer);
  void PipelineCreated(Pipeline pipeline, const GraphicsPipelineDesc& desc);
  void PipelineDestroyed(Pipeline pipeline);

  // Recording, in the order the renderer records.
  void Begin(CommandList list);
  void Submit(uint32_t count, const CommandList* lists);
  void BeginPass(CommandList list, Pass pass, Framebuffer framebuffer, const Rect& area, uint32_t n_clears,
                 const ClearValue* clears);
  void EndPass(CommandList list);
  void SetViewport(CommandList list, const Viewport& viewport);
  void SetScissor(CommandList list, const Rect& scissor);
  void SetBlendConstants(CommandList list, const float constants[4]);
  void SetDepthBias(CommandList list, float constant, float slope);
  void SetStencilValues(CommandList list, uint32_t front, uint32_t back);
  void BindPipeline(CommandList list, Pipeline pipeline);
  void BindSets(CommandList list, uint32_t first, uint32_t count, const Set* sets, uint32_t n_offsets,
                const uint32_t* offsets);
  void BindVertexBuffers(CommandList list, uint32_t count, const Buffer* buffers, const Size* offsets);
  void BindIndexBuffer(CommandList list, Buffer buffer, bool indices_16);
  void Draw(CommandList list, uint32_t count, uint32_t first_vertex);
  void DrawIndexed(CommandList list, uint32_t count, uint32_t first_index, int32_t vertex_offset);
  void ClearAttachment(CommandList list, Aspects aspects, uint32_t color_index, const ClearValue& value,
                       const Rect& area);
  void ClearColorImage(CommandList list, Image image, const ClearColor& color, uint32_t count, const Range* ranges);
  void ClearDepthStencilImage(CommandList list, Image image, const ClearDepthStencil& value, uint32_t count,
                              const Range* ranges);
  void CopyBuffer(CommandList list, Buffer source, Buffer target, uint32_t count, const BufferCopy* regions);
  void CopyBufferToImage(CommandList list, Buffer source, Image target, uint32_t count,
                         const BufferImageCopy* regions);
  void CopyImage(CommandList list, Image source, Image target, uint32_t count, const ImageCopy* regions);
  void Blit(CommandList list, Image source, Image target, uint32_t count, const ImageBlit* regions);
  // What the mirror cannot do yet, logged once per kind (queries, timestamps, compute, guest write-backs).
  void Skipped(const char* what);

  // The image the renderer showed last (its output pass's source), as an id<MTLTexture>, retained until the next
  // one; nullptr if there is none yet. Presentation reads it. The frame is the top-left width x height of the image
  // (front buffers and resolve targets can be larger than the frame, as in the Vulkan output pass), and Shown
  // gives that size, not the image's.
  void Showed(ImageView view, uint32_t width, uint32_t height);
  void* Shown(uint32_t& width, uint32_t& height);

  struct State;

 private:
  explicit Mirror(Device& device);
  ~Mirror();
  static Mirror* instance_;
  State* state_;
};

}  // namespace bo2zm::native::gpu::metal
