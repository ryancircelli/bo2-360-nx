// Render encoder recording for a second encode thread. See
// metal_deferred_render_encoder.h; threading design after mtld3d
// (licenses/mtld3d-LICENSE).

#include "rex/graphics/metal/metal_deferred_render_encoder.h"

#include <pthread.h>

#include <chrono>
#include <cstring>

#ifndef IR_RUNTIME_METALCPP
#define IR_RUNTIME_METALCPP
#endif
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"

namespace rex::graphics::metal {

enum class DeferredRenderEncoder::Op : uint16_t {
  kSetLabel,
  kSetRenderPipelineState,
  kSetDepthStencilState,
  kSetStencilReferenceValue,
  kSetViewport,
  kSetScissorRect,
  kSetBlendColor,
  kSetCullMode,
  kSetFrontFacingWinding,
  kSetTriangleFillMode,
  kSetDepthBias,
  kSetDepthClipMode,
  kSetVisibilityResultMode,
  kSetTessellationFactorBuffer,
  kSetVertexBuffer,
  kSetFragmentBuffer,
  kSetObjectBuffer,
  kSetMeshBuffer,
  kSetVertexBufferOffset,
  kSetFragmentBufferOffset,
  kSetObjectBufferOffset,
  kSetMeshBufferOffset,
  kSetVertexBytes,
  kSetObjectBytes,
  kSetMeshBytes,
  kUseResource,
  kUseResources,
  kUseHeap,
  kMemoryBarrier,
  kWaitForFence,
  kUpdateFence,
  kInsertDebugSignpost,
  kDrawPrimitives,
  kDrawIndexedPrimitives,
  kDrawIndexedPrimitivesFull,
  kDrawPatches,
  kDrawMeshThreadgroups,
  kIRDrawPrimitives,
  kIRDrawIndexedPrimitives,
  kEndEncoding,
};

namespace {

// Commands start with this header; the payload struct follows, then any
// variable bytes. Everything is 8-byte aligned.
struct Header {
  uint16_t op;
  uint16_t pad;
  uint32_t size;  // Whole command including the header.
};

constexpr size_t Align8(size_t n) { return (n + 7) & ~size_t(7); }

// Hand a chunk to the worker after this many draws or bytes, so it starts on
// a pass long before the pass ends.
constexpr uint32_t kChunkDraws = 48;
constexpr size_t kChunkBytes = 48 * 1024;

struct PtrArg {
  const void* object;
};
struct BufferArg {
  const MTL::Buffer* buffer;
  NS::UInteger offset;
  NS::UInteger index;
};
struct OffsetArg {
  NS::UInteger offset;
  NS::UInteger index;
};
struct BytesArg {
  NS::UInteger length;
  NS::UInteger index;
};
struct U32Arg {
  uint32_t value;
};
struct FloatsArg {
  float v[4];
};
struct ResourceArg {
  const MTL::Resource* resource;
  MTL::ResourceUsage usage;
  MTL::RenderStages stages;
};
struct ResourcesArg {
  NS::UInteger count;
  MTL::ResourceUsage usage;
  MTL::RenderStages stages;
};
struct BarrierArg {
  MTL::BarrierScope scope;
  MTL::RenderStages after;
  MTL::RenderStages before;
};
struct FenceArg {
  const MTL::Fence* fence;
  MTL::RenderStages stages;
};
struct VisibilityArg {
  MTL::VisibilityResultMode mode;
  NS::UInteger offset;
};
struct DrawArg {
  MTL::PrimitiveType type;
  NS::UInteger start;
  NS::UInteger count;
  NS::UInteger instances;
};
struct DrawIndexedArg {
  MTL::PrimitiveType type;
  NS::UInteger count;
  MTL::IndexType index_type;
  const MTL::Buffer* index_buffer;
  NS::UInteger index_offset;
  NS::UInteger instances;
  NS::Integer base_vertex;
  NS::UInteger base_instance;
};
struct PatchesArg {
  NS::UInteger control_points;
  NS::UInteger patch_start;
  NS::UInteger patch_count;
  const MTL::Buffer* index_buffer;
  NS::UInteger index_offset;
  NS::UInteger instances;
  NS::UInteger base_instance;
};
struct MeshArg {
  MTL::Size groups;
  MTL::Size object_threads;
  MTL::Size mesh_threads;
};
struct EmptyArg {};

}  // namespace

DeferredRenderEncoder::DeferredRenderEncoder() = default;

DeferredRenderEncoder::~DeferredRenderEncoder() {
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    work_cv_.notify_one();
    worker_.join();
  }
}

DeferredRenderEncoder* DeferredRenderEncoder::Open(
    MTL::RenderCommandEncoder* real, bool deferred) {
  real_ = real;
  deferred_ = deferred;
  if (deferred_ && !worker_.joinable()) {
    StartWorker();
  }
  return this;
}

void DeferredRenderEncoder::StartWorker() {
  worker_ = std::thread([this]() {
    pthread_setname_np("GPU Encode");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    WorkerMain();
  });
}

template <typename T>
T* DeferredRenderEncoder::Record(Op op, size_t extra_bytes) {
  if (!current_) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!free_chunks_.empty()) {
      current_ = std::move(free_chunks_.back());
      free_chunks_.pop_back();
    }
  }
  if (!current_) {
    current_ = std::make_unique<Chunk>();
    current_->bytes.reserve(kChunkBytes + 4096);
  }
  const size_t size = sizeof(Header) + Align8(sizeof(T)) + Align8(extra_bytes);
  std::vector<uint8_t>& bytes = current_->bytes;
  const size_t at = bytes.size();
  bytes.resize(at + size);
  auto* header = reinterpret_cast<Header*>(bytes.data() + at);
  header->op = uint16_t(op);
  header->pad = 0;
  header->size = uint32_t(size);
  ++stats_.commands;
  return reinterpret_cast<T*>(bytes.data() + at + sizeof(Header));
}

void DeferredRenderEncoder::Retain(const NS::Object* object) {
  if (object) {
    const_cast<NS::Object*>(object)->retain();
    current_->retained.push_back(object);
  }
}

void DeferredRenderEncoder::RetainBuffer(const NS::Object* buffer) {
  if (!buffer) {
    return;
  }
  for (const NS::Object* recent : recent_buffers_) {
    if (recent == buffer) {
      return;
    }
  }
  Retain(buffer);
  recent_buffers_[recent_buffer_next_++ & 7] = buffer;
}

void DeferredRenderEncoder::PublishChunk() {
  if (!current_ || current_->bytes.empty()) {
    return;
  }
  std::memset(recent_buffers_, 0, sizeof(recent_buffers_));
  bool wake;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(current_));
    ++published_;
    wake = worker_waiting_;
  }
  ++stats_.chunks;
  // Skip the wakeup syscall while the worker is still busy with earlier
  // chunks; it rechecks the queue before it sleeps.
  if (wake) {
    work_cv_.notify_one();
  }
}

void DeferredRenderEncoder::Drain() {
  PublishChunk();
  if (completed_.load(std::memory_order_acquire) == published_) {
    return;
  }
  ++stats_.drains;
  auto start = std::chrono::steady_clock::now();
  // Most drains finish within a few microseconds of the last chunk; spin
  // briefly before sleeping.
  for (int i = 0; i < 256; ++i) {
    if (completed_.load(std::memory_order_acquire) == published_) {
      break;
    }
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
  }
  if (completed_.load(std::memory_order_acquire) != published_) {
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this]() {
      return completed_.load(std::memory_order_acquire) == published_;
    });
  }
  stats_.drain_wait_us += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

void DeferredRenderEncoder::WorkerMain() {
  for (;;) {
    std::unique_ptr<Chunk> chunk;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!stop_ && queue_.empty()) {
        worker_waiting_ = true;
        work_cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        worker_waiting_ = false;
      }
      if (queue_.empty()) {
        return;
      }
      chunk = std::move(queue_.front());
      queue_.pop_front();
    }
    Replay(*chunk);
    for (const NS::Object* object : chunk->retained) {
      const_cast<NS::Object*>(object)->release();
    }
    chunk->retained.clear();
    chunk->bytes.clear();
    chunk->draws = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      free_chunks_.push_back(std::move(chunk));
      completed_.fetch_add(1, std::memory_order_release);
    }
    done_cv_.notify_one();
  }
}

MTL::RenderCommandEncoder* DeferredRenderEncoder::Sync() {
  if (deferred_) {
    Drain();
  }
  return real_;
}

void DeferredRenderEncoder::release() {
  if (deferred_) {
    Drain();
  }
  if (real_) {
    real_->release();
    real_ = nullptr;
  }
}

void DeferredRenderEncoder::endEncoding() {
  if (deferred_) {
    Record<EmptyArg>(Op::kEndEncoding);
    Drain();
    return;
  }
  real_->endEncoding();
}

void DeferredRenderEncoder::setLabel(NS::String* label) {
  if (!deferred_) {
    real_->setLabel(label);
    return;
  }
  Record<PtrArg>(Op::kSetLabel)->object = label;
  Retain(label);
}

void DeferredRenderEncoder::insertDebugSignpost(NS::String* string) {
  if (!deferred_) {
    real_->insertDebugSignpost(string);
    return;
  }
  Record<PtrArg>(Op::kInsertDebugSignpost)->object = string;
  Retain(string);
}

#define REX_DEFERRED_PTR(method, op, type)               \
  void DeferredRenderEncoder::method(const type* value) { \
    if (!deferred_) {                                     \
      real_->method(value);                               \
      return;                                             \
    }                                                     \
    Record<PtrArg>(Op::op)->object = value;               \
  }
REX_DEFERRED_PTR(setRenderPipelineState, kSetRenderPipelineState,
                 MTL::RenderPipelineState)
REX_DEFERRED_PTR(setDepthStencilState, kSetDepthStencilState,
                 MTL::DepthStencilState)
#undef REX_DEFERRED_PTR

#define REX_DEFERRED_U32(method, op, type)            \
  void DeferredRenderEncoder::method(type value) {    \
    if (!deferred_) {                                 \
      real_->method(value);                           \
      return;                                         \
    }                                                 \
    Record<U32Arg>(Op::op)->value = uint32_t(value);  \
  }
REX_DEFERRED_U32(setStencilReferenceValue, kSetStencilReferenceValue, uint32_t)
REX_DEFERRED_U32(setCullMode, kSetCullMode, MTL::CullMode)
REX_DEFERRED_U32(setFrontFacingWinding, kSetFrontFacingWinding, MTL::Winding)
REX_DEFERRED_U32(setTriangleFillMode, kSetTriangleFillMode,
                 MTL::TriangleFillMode)
REX_DEFERRED_U32(setDepthClipMode, kSetDepthClipMode, MTL::DepthClipMode)
#undef REX_DEFERRED_U32

void DeferredRenderEncoder::setViewport(MTL::Viewport viewport) {
  if (!deferred_) {
    real_->setViewport(viewport);
    return;
  }
  *Record<MTL::Viewport>(Op::kSetViewport) = viewport;
}

void DeferredRenderEncoder::setScissorRect(MTL::ScissorRect rect) {
  if (!deferred_) {
    real_->setScissorRect(rect);
    return;
  }
  *Record<MTL::ScissorRect>(Op::kSetScissorRect) = rect;
}

void DeferredRenderEncoder::setBlendColor(float r, float g, float b, float a) {
  if (!deferred_) {
    real_->setBlendColor(r, g, b, a);
    return;
  }
  *Record<FloatsArg>(Op::kSetBlendColor) = FloatsArg{{r, g, b, a}};
}

void DeferredRenderEncoder::setDepthBias(float bias, float slope,
                                         float clamp) {
  if (!deferred_) {
    real_->setDepthBias(bias, slope, clamp);
    return;
  }
  *Record<FloatsArg>(Op::kSetDepthBias) = FloatsArg{{bias, slope, clamp, 0}};
}

void DeferredRenderEncoder::setVisibilityResultMode(
    MTL::VisibilityResultMode mode, NS::UInteger offset) {
  if (!deferred_) {
    real_->setVisibilityResultMode(mode, offset);
    return;
  }
  *Record<VisibilityArg>(Op::kSetVisibilityResultMode) =
      VisibilityArg{mode, offset};
}

void DeferredRenderEncoder::setTessellationFactorBuffer(
    const MTL::Buffer* buffer, NS::UInteger offset, NS::UInteger stride) {
  if (!deferred_) {
    real_->setTessellationFactorBuffer(buffer, offset, stride);
    return;
  }
  *Record<BufferArg>(Op::kSetTessellationFactorBuffer) =
      BufferArg{buffer, offset, stride};
  RetainBuffer(buffer);
}

#define REX_DEFERRED_BUFFER(method, op)                                     \
  void DeferredRenderEncoder::method(const MTL::Buffer* buffer,             \
                                     NS::UInteger offset,                   \
                                     NS::UInteger index) {                  \
    if (!deferred_) {                                                       \
      real_->method(buffer, offset, index);                                 \
      return;                                                               \
    }                                                                       \
    *Record<BufferArg>(Op::op) = BufferArg{buffer, offset, index};          \
    RetainBuffer(buffer);                                                   \
  }
REX_DEFERRED_BUFFER(setVertexBuffer, kSetVertexBuffer)
REX_DEFERRED_BUFFER(setFragmentBuffer, kSetFragmentBuffer)
REX_DEFERRED_BUFFER(setObjectBuffer, kSetObjectBuffer)
REX_DEFERRED_BUFFER(setMeshBuffer, kSetMeshBuffer)
#undef REX_DEFERRED_BUFFER

#define REX_DEFERRED_OFFSET(method, op)                                     \
  void DeferredRenderEncoder::method(NS::UInteger offset,                   \
                                     NS::UInteger index) {                  \
    if (!deferred_) {                                                       \
      real_->method(offset, index);                                         \
      return;                                                               \
    }                                                                       \
    *Record<OffsetArg>(Op::op) = OffsetArg{offset, index};                  \
  }
REX_DEFERRED_OFFSET(setVertexBufferOffset, kSetVertexBufferOffset)
REX_DEFERRED_OFFSET(setFragmentBufferOffset, kSetFragmentBufferOffset)
REX_DEFERRED_OFFSET(setObjectBufferOffset, kSetObjectBufferOffset)
REX_DEFERRED_OFFSET(setMeshBufferOffset, kSetMeshBufferOffset)
#undef REX_DEFERRED_OFFSET

#define REX_DEFERRED_BYTES(method, op)                                      \
  void DeferredRenderEncoder::method(const void* bytes, NS::UInteger length, \
                                     NS::UInteger index) {                  \
    if (!deferred_) {                                                       \
      real_->method(bytes, length, index);                                  \
      return;                                                               \
    }                                                                       \
    auto* arg = Record<BytesArg>(Op::op, length);                           \
    arg->length = length;                                                   \
    arg->index = index;                                                     \
    std::memcpy(reinterpret_cast<uint8_t*>(arg) + Align8(sizeof(BytesArg)), \
                bytes, length);                                             \
  }
REX_DEFERRED_BYTES(setVertexBytes, kSetVertexBytes)
REX_DEFERRED_BYTES(setObjectBytes, kSetObjectBytes)
REX_DEFERRED_BYTES(setMeshBytes, kSetMeshBytes)
#undef REX_DEFERRED_BYTES

void DeferredRenderEncoder::useResource(const MTL::Resource* resource,
                                        MTL::ResourceUsage usage,
                                        MTL::RenderStages stages) {
  if (!deferred_) {
    real_->useResource(resource, usage, stages);
    return;
  }
  *Record<ResourceArg>(Op::kUseResource) = ResourceArg{resource, usage, stages};
  Retain(resource);
}

void DeferredRenderEncoder::useResources(
    const MTL::Resource* const resources[], NS::UInteger count,
    MTL::ResourceUsage usage, MTL::RenderStages stages) {
  if (!deferred_) {
    real_->useResources(resources, count, usage, stages);
    return;
  }
  const size_t list_bytes = sizeof(const MTL::Resource*) * count;
  auto* arg = Record<ResourcesArg>(Op::kUseResources, list_bytes);
  *arg = ResourcesArg{count, usage, stages};
  std::memcpy(reinterpret_cast<uint8_t*>(arg) + Align8(sizeof(ResourcesArg)),
              resources, list_bytes);
  for (NS::UInteger i = 0; i < count; ++i) {
    Retain(resources[i]);
  }
}

void DeferredRenderEncoder::useHeap(const MTL::Heap* heap) {
  if (!deferred_) {
    real_->useHeap(heap);
    return;
  }
  Record<PtrArg>(Op::kUseHeap)->object = heap;
  Retain(heap);
}

void DeferredRenderEncoder::memoryBarrier(MTL::BarrierScope scope,
                                          MTL::RenderStages after,
                                          MTL::RenderStages before) {
  if (!deferred_) {
    real_->memoryBarrier(scope, after, before);
    return;
  }
  *Record<BarrierArg>(Op::kMemoryBarrier) = BarrierArg{scope, after, before};
}

void DeferredRenderEncoder::waitForFence(const MTL::Fence* fence,
                                         MTL::RenderStages stages) {
  if (!deferred_) {
    real_->waitForFence(fence, stages);
    return;
  }
  *Record<FenceArg>(Op::kWaitForFence) = FenceArg{fence, stages};
}

void DeferredRenderEncoder::updateFence(const MTL::Fence* fence,
                                        MTL::RenderStages stages) {
  if (!deferred_) {
    real_->updateFence(fence, stages);
    return;
  }
  *Record<FenceArg>(Op::kUpdateFence) = FenceArg{fence, stages};
}

#define REX_DEFERRED_DRAW_DONE()                                    \
  if (++current_->draws >= kChunkDraws ||                           \
      current_->bytes.size() >= kChunkBytes) {                      \
    PublishChunk();                                                 \
  }

void DeferredRenderEncoder::drawPrimitives(MTL::PrimitiveType type,
                                           NS::UInteger start,
                                           NS::UInteger count,
                                           NS::UInteger instances) {
  if (!deferred_) {
    real_->drawPrimitives(type, start, count, instances);
    return;
  }
  *Record<DrawArg>(Op::kDrawPrimitives) = DrawArg{type, start, count, instances};
  REX_DEFERRED_DRAW_DONE()
}

void DeferredRenderEncoder::drawIndexedPrimitives(
    MTL::PrimitiveType type, NS::UInteger count, MTL::IndexType index_type,
    const MTL::Buffer* index_buffer, NS::UInteger index_offset) {
  if (!deferred_) {
    real_->drawIndexedPrimitives(type, count, index_type, index_buffer,
                                 index_offset);
    return;
  }
  *Record<DrawIndexedArg>(Op::kDrawIndexedPrimitives) = DrawIndexedArg{
      type, count, index_type, index_buffer, index_offset, 1, 0, 0};
  RetainBuffer(index_buffer);
  REX_DEFERRED_DRAW_DONE()
}

void DeferredRenderEncoder::drawIndexedPrimitives(
    MTL::PrimitiveType type, NS::UInteger count, MTL::IndexType index_type,
    const MTL::Buffer* index_buffer, NS::UInteger index_offset,
    NS::UInteger instances, NS::Integer base_vertex,
    NS::UInteger base_instance) {
  if (!deferred_) {
    real_->drawIndexedPrimitives(type, count, index_type, index_buffer,
                                 index_offset, instances, base_vertex,
                                 base_instance);
    return;
  }
  *Record<DrawIndexedArg>(Op::kDrawIndexedPrimitivesFull) =
      DrawIndexedArg{type,      count,       index_type,  index_buffer,
                     index_offset, instances, base_vertex, base_instance};
  RetainBuffer(index_buffer);
  REX_DEFERRED_DRAW_DONE()
}

void DeferredRenderEncoder::drawPatches(NS::UInteger control_points,
                                        NS::UInteger patch_start,
                                        NS::UInteger patch_count,
                                        const MTL::Buffer* index_buffer,
                                        NS::UInteger index_offset,
                                        NS::UInteger instances,
                                        NS::UInteger base_instance) {
  if (!deferred_) {
    real_->drawPatches(control_points, patch_start, patch_count, index_buffer,
                       index_offset, instances, base_instance);
    return;
  }
  *Record<PatchesArg>(Op::kDrawPatches) =
      PatchesArg{control_points, patch_start, patch_count, index_buffer,
                 index_offset,   instances,   base_instance};
  RetainBuffer(index_buffer);
  REX_DEFERRED_DRAW_DONE()
}

void DeferredRenderEncoder::drawMeshThreadgroups(MTL::Size groups,
                                                 MTL::Size object_threads,
                                                 MTL::Size mesh_threads) {
  if (!deferred_) {
    real_->drawMeshThreadgroups(groups, object_threads, mesh_threads);
    return;
  }
  *Record<MeshArg>(Op::kDrawMeshThreadgroups) =
      MeshArg{groups, object_threads, mesh_threads};
  REX_DEFERRED_DRAW_DONE()
}

void DeferredRenderEncoder::IRDrawPrimitives(MTL::PrimitiveType type,
                                             uint64_t start, uint64_t count) {
  if (!deferred_) {
    IRRuntimeDrawPrimitives(real_, type, start, count);
    return;
  }
  *Record<DrawArg>(Op::kIRDrawPrimitives) =
      DrawArg{type, NS::UInteger(start), NS::UInteger(count), 1};
  REX_DEFERRED_DRAW_DONE()
}

void DeferredRenderEncoder::IRDrawIndexedPrimitives(
    MTL::PrimitiveType type, uint64_t count, MTL::IndexType index_type,
    const MTL::Buffer* index_buffer, uint64_t index_offset,
    uint64_t instances, int64_t base_vertex, uint64_t base_instance) {
  if (!deferred_) {
    IRRuntimeDrawIndexedPrimitives(
        real_, type, count, index_type, const_cast<MTL::Buffer*>(index_buffer),
        index_offset, instances, base_vertex, base_instance);
    return;
  }
  *Record<DrawIndexedArg>(Op::kIRDrawIndexedPrimitives) = DrawIndexedArg{
      type,      NS::UInteger(count),       index_type,
      index_buffer, NS::UInteger(index_offset), NS::UInteger(instances),
      NS::Integer(base_vertex), NS::UInteger(base_instance)};
  RetainBuffer(index_buffer);
  REX_DEFERRED_DRAW_DONE()
}

#undef REX_DEFERRED_DRAW_DONE

void DeferredRenderEncoder::Replay(Chunk& chunk) {
  MTL::RenderCommandEncoder* enc = real_;
  const uint8_t* at = chunk.bytes.data();
  const uint8_t* end = at + chunk.bytes.size();
  while (at < end) {
    const auto* header = reinterpret_cast<const Header*>(at);
    const uint8_t* payload = at + sizeof(Header);
    at += header->size;
    auto arg = [payload](auto* type_tag) {
      return reinterpret_cast<const std::remove_pointer_t<decltype(type_tag)>*>(
          payload);
    };
    switch (Op(header->op)) {
      case Op::kSetLabel:
        enc->setLabel(static_cast<NS::String*>(const_cast<void*>(
            arg((PtrArg*)nullptr)->object)));
        break;
      case Op::kInsertDebugSignpost:
        enc->insertDebugSignpost(static_cast<NS::String*>(
            const_cast<void*>(arg((PtrArg*)nullptr)->object)));
        break;
      case Op::kSetRenderPipelineState:
        enc->setRenderPipelineState(static_cast<const MTL::RenderPipelineState*>(
            arg((PtrArg*)nullptr)->object));
        break;
      case Op::kSetDepthStencilState:
        enc->setDepthStencilState(static_cast<const MTL::DepthStencilState*>(
            arg((PtrArg*)nullptr)->object));
        break;
      case Op::kSetStencilReferenceValue:
        enc->setStencilReferenceValue(arg((U32Arg*)nullptr)->value);
        break;
      case Op::kSetCullMode:
        enc->setCullMode(MTL::CullMode(arg((U32Arg*)nullptr)->value));
        break;
      case Op::kSetFrontFacingWinding:
        enc->setFrontFacingWinding(MTL::Winding(arg((U32Arg*)nullptr)->value));
        break;
      case Op::kSetTriangleFillMode:
        enc->setTriangleFillMode(
            MTL::TriangleFillMode(arg((U32Arg*)nullptr)->value));
        break;
      case Op::kSetDepthClipMode:
        enc->setDepthClipMode(MTL::DepthClipMode(arg((U32Arg*)nullptr)->value));
        break;
      case Op::kSetViewport:
        enc->setViewport(*arg((MTL::Viewport*)nullptr));
        break;
      case Op::kSetScissorRect:
        enc->setScissorRect(*arg((MTL::ScissorRect*)nullptr));
        break;
      case Op::kSetBlendColor: {
        const FloatsArg* a = arg((FloatsArg*)nullptr);
        enc->setBlendColor(a->v[0], a->v[1], a->v[2], a->v[3]);
        break;
      }
      case Op::kSetDepthBias: {
        const FloatsArg* a = arg((FloatsArg*)nullptr);
        enc->setDepthBias(a->v[0], a->v[1], a->v[2]);
        break;
      }
      case Op::kSetVisibilityResultMode: {
        const VisibilityArg* a = arg((VisibilityArg*)nullptr);
        enc->setVisibilityResultMode(a->mode, a->offset);
        break;
      }
      case Op::kSetTessellationFactorBuffer: {
        const BufferArg* a = arg((BufferArg*)nullptr);
        enc->setTessellationFactorBuffer(a->buffer, a->offset, a->index);
        break;
      }
      case Op::kSetVertexBuffer: {
        const BufferArg* a = arg((BufferArg*)nullptr);
        enc->setVertexBuffer(a->buffer, a->offset, a->index);
        break;
      }
      case Op::kSetFragmentBuffer: {
        const BufferArg* a = arg((BufferArg*)nullptr);
        enc->setFragmentBuffer(a->buffer, a->offset, a->index);
        break;
      }
      case Op::kSetObjectBuffer: {
        const BufferArg* a = arg((BufferArg*)nullptr);
        enc->setObjectBuffer(a->buffer, a->offset, a->index);
        break;
      }
      case Op::kSetMeshBuffer: {
        const BufferArg* a = arg((BufferArg*)nullptr);
        enc->setMeshBuffer(a->buffer, a->offset, a->index);
        break;
      }
      case Op::kSetVertexBufferOffset: {
        const OffsetArg* a = arg((OffsetArg*)nullptr);
        enc->setVertexBufferOffset(a->offset, a->index);
        break;
      }
      case Op::kSetFragmentBufferOffset: {
        const OffsetArg* a = arg((OffsetArg*)nullptr);
        enc->setFragmentBufferOffset(a->offset, a->index);
        break;
      }
      case Op::kSetObjectBufferOffset: {
        const OffsetArg* a = arg((OffsetArg*)nullptr);
        enc->setObjectBufferOffset(a->offset, a->index);
        break;
      }
      case Op::kSetMeshBufferOffset: {
        const OffsetArg* a = arg((OffsetArg*)nullptr);
        enc->setMeshBufferOffset(a->offset, a->index);
        break;
      }
      case Op::kSetVertexBytes:
      case Op::kSetObjectBytes:
      case Op::kSetMeshBytes: {
        const BytesArg* a = arg((BytesArg*)nullptr);
        const void* bytes = payload + Align8(sizeof(BytesArg));
        if (Op(header->op) == Op::kSetVertexBytes) {
          enc->setVertexBytes(bytes, a->length, a->index);
        } else if (Op(header->op) == Op::kSetObjectBytes) {
          enc->setObjectBytes(bytes, a->length, a->index);
        } else {
          enc->setMeshBytes(bytes, a->length, a->index);
        }
        break;
      }
      case Op::kUseResource: {
        const ResourceArg* a = arg((ResourceArg*)nullptr);
        enc->useResource(a->resource, a->usage, a->stages);
        break;
      }
      case Op::kUseResources: {
        const ResourcesArg* a = arg((ResourcesArg*)nullptr);
        enc->useResources(reinterpret_cast<const MTL::Resource* const*>(
                              payload + Align8(sizeof(ResourcesArg))),
                          a->count, a->usage, a->stages);
        break;
      }
      case Op::kUseHeap:
        enc->useHeap(
            static_cast<const MTL::Heap*>(arg((PtrArg*)nullptr)->object));
        break;
      case Op::kMemoryBarrier: {
        const BarrierArg* a = arg((BarrierArg*)nullptr);
        enc->memoryBarrier(a->scope, a->after, a->before);
        break;
      }
      case Op::kWaitForFence: {
        const FenceArg* a = arg((FenceArg*)nullptr);
        enc->waitForFence(a->fence, a->stages);
        break;
      }
      case Op::kUpdateFence: {
        const FenceArg* a = arg((FenceArg*)nullptr);
        enc->updateFence(a->fence, a->stages);
        break;
      }
      case Op::kDrawPrimitives: {
        const DrawArg* a = arg((DrawArg*)nullptr);
        enc->drawPrimitives(a->type, a->start, a->count, a->instances);
        break;
      }
      case Op::kDrawIndexedPrimitives: {
        const DrawIndexedArg* a = arg((DrawIndexedArg*)nullptr);
        enc->drawIndexedPrimitives(a->type, a->count, a->index_type,
                                   a->index_buffer, a->index_offset);
        break;
      }
      case Op::kDrawIndexedPrimitivesFull: {
        const DrawIndexedArg* a = arg((DrawIndexedArg*)nullptr);
        enc->drawIndexedPrimitives(a->type, a->count, a->index_type,
                                   a->index_buffer, a->index_offset,
                                   a->instances, a->base_vertex,
                                   a->base_instance);
        break;
      }
      case Op::kDrawPatches: {
        const PatchesArg* a = arg((PatchesArg*)nullptr);
        enc->drawPatches(a->control_points, a->patch_start, a->patch_count,
                         a->index_buffer, a->index_offset, a->instances,
                         a->base_instance);
        break;
      }
      case Op::kDrawMeshThreadgroups: {
        const MeshArg* a = arg((MeshArg*)nullptr);
        enc->drawMeshThreadgroups(a->groups, a->object_threads,
                                  a->mesh_threads);
        break;
      }
      case Op::kIRDrawPrimitives: {
        const DrawArg* a = arg((DrawArg*)nullptr);
        IRRuntimeDrawPrimitives(enc, a->type, uint64_t(a->start),
                                uint64_t(a->count));
        break;
      }
      case Op::kIRDrawIndexedPrimitives: {
        const DrawIndexedArg* a = arg((DrawIndexedArg*)nullptr);
        IRRuntimeDrawIndexedPrimitives(
            enc, a->type, uint64_t(a->count), a->index_type,
            const_cast<MTL::Buffer*>(a->index_buffer),
            uint64_t(a->index_offset), uint64_t(a->instances),
            int64_t(a->base_vertex), uint64_t(a->base_instance));
        break;
      }
      case Op::kEndEncoding:
        enc->endEncoding();
        break;
    }
  }
}

}  // namespace rex::graphics::metal
