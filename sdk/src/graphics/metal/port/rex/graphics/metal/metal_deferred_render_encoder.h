// Render encoder recording for a second encode thread
// (metal_encode_thread).
//
// The command processor spends a large share of each split screen frame inside
// Metal's render encoder calls (state setters, useResource, draws). This class
// stands in for the MTL::RenderCommandEncoder the command processor used to
// hold: with metal_encode_thread off every call goes straight to the real
// encoder; with it on, calls are recorded into chunks and a worker thread
// replays them into the real encoder while the command processor carries on
// preparing the next draws.
//
// The split follows mtld3d's threading model (Alexander Theissen, zlib
// licence, licenses/mtld3d-LICENSE): the thread that sees the API records
// compact typed commands into chunked scratch memory, and a native encoder
// thread turns them into Metal calls; anything that needs the real encoder
// synchronously drains the worker first. No mtld3d code is used.
//
// Rules:
//  * Only the command processor thread calls into this object.
//  * endEncoding() and release() drain the worker, so once the command
//    processor ends the encoder every recorded call has reached Metal and the
//    command buffer is free for other encoders or commit.
//  * Sync() drains and returns the real encoder, for the rare paths that hand
//    the encoder to other code (tile resolve, draw pass transfers, geometry and
//    tessellation emulation).
//  * Resources passed to useResource(s) and useHeap are retained until the
//    worker has replayed them, since caches may drop a texture right after
//    declaring it. Buffers (vertex, fragment, index) are retained too, with a
//    small per-chunk dedupe so repeated pool buffers cost one retain.
//    Pipelines and states live in caches for the session and are not
//    retained.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace rex::graphics::metal {

class DeferredRenderEncoder {
 public:
  DeferredRenderEncoder();
  ~DeferredRenderEncoder();
  DeferredRenderEncoder(const DeferredRenderEncoder&) = delete;
  DeferredRenderEncoder& operator=(const DeferredRenderEncoder&) = delete;

  // Takes over a retained encoder. deferred selects the encode thread for
  // this encoder's lifetime. Returns this.
  DeferredRenderEncoder* Open(MTL::RenderCommandEncoder* real, bool deferred);

  // Drains and returns the real encoder.
  MTL::RenderCommandEncoder* Sync();
  bool deferred() const { return deferred_; }

  // Lifetime (same meaning as on MTL::RenderCommandEncoder).
  void retain() {}
  void release();
  void endEncoding();
  void setLabel(NS::String* label);

  // State.
  void setRenderPipelineState(const MTL::RenderPipelineState* state);
  void setDepthStencilState(const MTL::DepthStencilState* state);
  void setStencilReferenceValue(uint32_t value);
  void setViewport(MTL::Viewport viewport);
  void setScissorRect(MTL::ScissorRect rect);
  void setBlendColor(float r, float g, float b, float a);
  void setCullMode(MTL::CullMode mode);
  void setFrontFacingWinding(MTL::Winding winding);
  void setTriangleFillMode(MTL::TriangleFillMode mode);
  void setDepthBias(float bias, float slope, float clamp);
  void setDepthClipMode(MTL::DepthClipMode mode);
  void setVisibilityResultMode(MTL::VisibilityResultMode mode,
                               NS::UInteger offset);
  void setTessellationFactorBuffer(const MTL::Buffer* buffer,
                                   NS::UInteger offset, NS::UInteger stride);

  // Buffers.
  void setVertexBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                       NS::UInteger index);
  void setFragmentBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                         NS::UInteger index);
  void setObjectBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                       NS::UInteger index);
  void setMeshBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                     NS::UInteger index);
  void setVertexBufferOffset(NS::UInteger offset, NS::UInteger index);
  void setFragmentBufferOffset(NS::UInteger offset, NS::UInteger index);
  void setObjectBufferOffset(NS::UInteger offset, NS::UInteger index);
  void setMeshBufferOffset(NS::UInteger offset, NS::UInteger index);
  void setVertexBytes(const void* bytes, NS::UInteger length,
                      NS::UInteger index);
  void setObjectBytes(const void* bytes, NS::UInteger length,
                      NS::UInteger index);
  void setMeshBytes(const void* bytes, NS::UInteger length, NS::UInteger index);

  // Residency and synchronization.
  void useResource(const MTL::Resource* resource, MTL::ResourceUsage usage,
                   MTL::RenderStages stages);
  void useResources(const MTL::Resource* const resources[], NS::UInteger count,
                    MTL::ResourceUsage usage, MTL::RenderStages stages);
  void useHeap(const MTL::Heap* heap);
  void memoryBarrier(MTL::BarrierScope scope, MTL::RenderStages after,
                     MTL::RenderStages before);
  void waitForFence(const MTL::Fence* fence, MTL::RenderStages stages);
  void updateFence(const MTL::Fence* fence, MTL::RenderStages stages);
  void insertDebugSignpost(NS::String* string);

  // Draws.
  void drawPrimitives(MTL::PrimitiveType type, NS::UInteger start,
                      NS::UInteger count, NS::UInteger instances);
  void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger count,
                             MTL::IndexType index_type,
                             const MTL::Buffer* index_buffer,
                             NS::UInteger index_offset);
  void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger count,
                             MTL::IndexType index_type,
                             const MTL::Buffer* index_buffer,
                             NS::UInteger index_offset, NS::UInteger instances,
                             NS::Integer base_vertex,
                             NS::UInteger base_instance);
  void drawPatches(NS::UInteger control_points, NS::UInteger patch_start,
                   NS::UInteger patch_count, const MTL::Buffer* index_buffer,
                   NS::UInteger index_offset, NS::UInteger instances,
                   NS::UInteger base_instance);
  void drawMeshThreadgroups(MTL::Size groups, MTL::Size object_threads,
                            MTL::Size mesh_threads);
  // Metal shader converter runtime draws (IRRuntimeDrawPrimitives /
  // IRRuntimeDrawIndexedPrimitives, non-instanced forms used by IssueDraw).
  void IRDrawPrimitives(MTL::PrimitiveType type, uint64_t start,
                        uint64_t count);
  void IRDrawIndexedPrimitives(MTL::PrimitiveType type, uint64_t count,
                               MTL::IndexType index_type,
                               const MTL::Buffer* index_buffer,
                               uint64_t index_offset, uint64_t instances,
                               int64_t base_vertex, uint64_t base_instance);

  struct Stats {
    uint64_t chunks = 0;
    uint64_t commands = 0;
    uint64_t drains = 0;
    uint64_t drain_wait_us = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  enum class Op : uint16_t;
  struct Chunk {
    std::vector<uint8_t> bytes;
    std::vector<const NS::Object*> retained;
    uint32_t draws = 0;
  };

  template <typename T>
  T* Record(Op op, size_t extra_bytes = 0);
  void Retain(const NS::Object* object);
  void RetainBuffer(const NS::Object* buffer);
  void PublishChunk();
  void Drain();
  void StartWorker();
  void WorkerMain();
  void Replay(Chunk& chunk);

  MTL::RenderCommandEncoder* real_ = nullptr;
  bool deferred_ = false;
  Stats stats_;

  bool worker_waiting_ = false;  // Guarded by mutex_.
  std::unique_ptr<Chunk> current_;
  const NS::Object* recent_buffers_[8] = {};
  uint32_t recent_buffer_next_ = 0;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  std::deque<std::unique_ptr<Chunk>> queue_;
  std::vector<std::unique_ptr<Chunk>> free_chunks_;
  uint64_t published_ = 0;
  std::atomic<uint64_t> completed_{0};
  bool stop_ = false;
};

}  // namespace rex::graphics::metal
