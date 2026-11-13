// See bo2zm_native_deferred.h.
//
// What it relies on
// -----------------
//   - One producer: the ring thread records the job buffer (draws, passes, copies, barriers, queries) and nobody
//     else does; a wrapper called with the job buffer from another thread says so once in the log and calls the
//     real function (that would be a bug elsewhere: the order could not be kept).
//   - Vulkan's rules: a command buffer may be recorded by any thread as long as one thread at a time does it. Here
//     the recorder makes the queued calls, and the ring thread only touches the job buffer itself (a call without a
//     queued form, vkEndCommandBuffer) after waiting until the recorder has caught up (Drain).
//   - What a call reads when it is made is copied when it is queued: every array and structure passed by pointer.
//     Handles (images, views, pipelines, buffers) are only referenced; they already have to live until the GPU has
//     run the frame, which is later than the recorder makes the call. Structures with a pNext chain are not queued
//     (none is used today): such a call takes the waiting path.
//   - Calls that are not vkCmd* (descriptor writes, image and pipeline creation, the upload buffer's contents) stay
//     on the ring thread at once. Descriptor sets are written with update-after-bind, so a set written before the
//     recorder binds it is as valid as one written after.
//
// The queue: a single-producer, single-consumer ring of 8 MB of records {op, size} plus arguments, 8-byte aligned.
// A frame of ~3,000 draws is ~0.3-0.5 MB of records. The recorder does not spin: once it has made every published
// call it sleeps on a condition variable, and the ring thread wakes it when kWakeBytes more have been queued since it
// last looked (and in Drain). Spinning between records was tried first, up to 30 us at a time; on the PC that was 88 %
// of the recorder's samples (22 % of a core for about 2 % of real work), and on the console, with all three cores
// busy, a spinning thread at the ring's priority takes time from the game. Waking in batches costs one wake per ~4 KB
// of records (about 70 per frame) and leaves at most that much to be made at the frame's end.

#include "bo2zm_native_deferred.h"

#include "../../bo2zm_benchmark.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread.h>

#if REX_PLATFORM_SWITCH
#include "core/threading_switch.h"  // the SDK's src/ is on the include path (CMakeLists.txt)
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_recording, true, "BO2",
                    "Native renderer: the work command buffer's Vulkan calls are made by a recorder thread, in order, "
                    "while the ring thread goes on decoding (bo2zm_native_deferred.h). false = all on the ring thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_recorder_priority, 0x2D, "BO2",
                     "Switch: Horizon priority of the recorder thread (0x1C-0x3B); the ring thread's by default")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_recorder_core, -2, "BO2",
                     "Switch: preferred core of the recorder thread. -2 = the one neither the ring nor the vertex copy "
                     "thread prefers; -1 = no preference; 0-2 = that one")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_recorder_priority_b, -1, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: the recorder's Horizon priority (0x1C-0x3B) in the A/B "
                     "arms with factor 1 on; 0x3B is the game threads' band, where it shares the cores with Main "
                     "instead of preempting it. -1 = not part of the A/B")
    .range(-1, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_recorder_core_b, -3, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: the recorder's preferred core (0-2, -1 = none) in the "
                     "A/B arms with factor 1 on. -3 = not part of the A/B")
    .range(-3, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::native::deferred {
namespace {

using Clock = std::chrono::steady_clock;

Functions g_real;   // the device's table
Functions g_table;  // the renderer's
std::once_flag g_once;
bool g_enabled = false;

// --- Separately loaded functions (Wrap) -------------------------------------------------------------------------
struct Loaded {
  PFN_vkCmdSetCullMode cull = nullptr;
  PFN_vkCmdSetFrontFace front = nullptr;
  PFN_vkCmdSetPrimitiveTopology topology = nullptr;
  PFN_vkCmdSetPrimitiveRestartEnable restart = nullptr;
  PFN_vkCmdSetDepthBiasEnable bias = nullptr;
  PFN_vkCmdSetDepthTestEnable test_z = nullptr;
  PFN_vkCmdSetDepthWriteEnable write_z = nullptr;
  PFN_vkCmdSetDepthCompareOp compare_z = nullptr;
  PFN_vkCmdSetStencilTestEnable stencil = nullptr;
  PFN_vkCmdSetStencilOp stencil_op = nullptr;
  PFN_vkCmdSetColorBlendEnableEXT blend = nullptr;
  PFN_vkCmdSetColorBlendEquationEXT equation = nullptr;
  PFN_vkCmdSetColorWriteMaskEXT mask = nullptr;
  PFN_vkCmdWriteTimestamp timestamp = nullptr;
  PFN_vkCmdCopyImage copy_image = nullptr;
  PFN_vkCmdClearDepthStencilImage clear_depth = nullptr;
  PFN_vkCmdBlitImage blit = nullptr;
} g_loaded;

// --- The queue ----------------------------------------------------------------------------------------------------
constexpr uint64_t kBytes = uint64_t(8) << 20;

struct Header {
  uint32_t op;
  uint32_t size;  // with the header, a multiple of 8
};

std::unique_ptr<uint64_t[]> g_storage;  // kBytes, 8-byte aligned
uint8_t* g_buffer = nullptr;

alignas(64) std::atomic<uint64_t> g_written{0};  // published by the ring thread
alignas(64) std::atomic<uint64_t> g_done{0};     // records made by the recorder
alignas(64) uint64_t g_cursor = 0;               // the ring thread's position (published or not)
uint32_t g_pending = 0;                          // size of the record being written
uint64_t g_looked = 0;                           // g_cursor at the ring thread's last look at the recorder's sleep
// Publishing in batches (bo2zm_native_deferred_batch, PublishInBatches). g_written and g_done each sit on a cache line
// the other thread keeps using, so every store to one and load of the other is a miss between cores; a console
// profile put about 3 % of the ring thread's busy time on Commit's store. With batches, the ring thread publishes
// once kPublishBytes more are queued (and in Publish, at every Drain and room wait), and reads g_done only when the
// room it last saw (g_done_seen, never more than the real room) is not enough.
bool g_batch = false;
uint64_t g_published = 0;
uint64_t g_done_seen = 0;
constexpr uint64_t kPublishBytes = 512;
std::atomic<VkCommandBuffer> g_target{VK_NULL_HANDLE};
thread_local bool t_producer = false;
VkCommandBuffer g_recording = VK_NULL_HANDLE;    // the recorder's command buffer (set by the kTarget record)

std::atomic<bool> g_recorder_sleeping{false};
std::atomic<bool> g_producer_waiting{false};
std::atomic<bool> g_stop{false};
std::mutex g_mutex;
std::condition_variable g_cv_recorder;
std::condition_variable g_cv_producer;
// Never freed and never detached: on Horizon std::thread::detach() throws (newlib has no pthread_detach) and the
// game closes, and a joinable std::thread destroyed at exit would terminate it too.
std::thread* g_thread = nullptr;
bool g_thread_started = false;

// Counters for the report (the ring thread's, except where noted).
uint64_t g_records = 0;
uint64_t g_bytes = 0;
uint64_t g_finishes = 0;
uint64_t g_ns_finish = 0;
uint64_t g_drains_direct = 0;  // calls without a queued form, made after a drain
uint64_t g_room_waits = 0;
uint64_t g_wakes = 0;          // notifications of a sleeping recorder
std::atomic<uint64_t> g_foreign{0};  // the job buffer from another thread
std::atomic<uint64_t> g_ns_recorder{0};  // recorder: time making calls
Clock::time_point g_report_last{};
uint64_t g_report_previous[8] = {};

enum Op : uint32_t {
  kSkip,
  kTarget,
  kBindPipeline,
  kBindDescriptorSets,
  kPushConstants,
  kSetViewport,
  kSetScissor,
  kSetBlendConstants,
  kSetDepthBias,
  kSetStencilReference,
  kSetStencilCompareMask,
  kSetStencilWriteMask,
  kBindVertexBuffers,
  kBindIndexBuffer,
  kDraw,
  kDrawIndexed,
  kBeginRenderPass,
  kEndRenderPass,
  kPipelineBarrier,
  kClearColorImage,
  kClearDepthStencilImage,
  kClearAttachments,
  kCopyBufferToImage,
  kCopyImageToBuffer,
  kCopyImage,
  kBlitImage,
  kCopyBuffer,
  kResetQueryPool,
  kBeginQuery,
  kEndQuery,
  kWriteTimestamp,
  kSetCullMode,
  kSetFrontFace,
  kSetPrimitiveTopology,
  kSetPrimitiveRestartEnable,
  kSetDepthBiasEnable,
  kSetDepthTestEnable,
  kSetDepthWriteEnable,
  kSetDepthCompareOp,
  kSetStencilTestEnable,
  kSetStencilOp,
  kSetColorBlendEnable,
  kSetColorBlendEquation,
  kSetColorWriteMask,
  kPrefetch,
};

// Wakes the recorder if it went to sleep. The fence orders the ring thread's last g_written store before its look at
// g_recorder_sleeping; the recorder sets g_recorder_sleeping before its last look at g_written (both seq_cst), so one
// of the two sees the other's store.
void Wake() {
  g_looked = g_cursor;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (g_recorder_sleeping.load(std::memory_order_relaxed)) {
    ++g_wakes;
    std::lock_guard lock(g_mutex);
    g_cv_recorder.notify_one();
  }
}

void Publish() {
  g_written.store(g_cursor, std::memory_order_release);
  g_published = g_cursor;
  Wake();
}

void CpuRelax() {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#endif
}

// Waits until 'bytes' more fit (the recorder frees room as it goes).
void WaitRoom(uint64_t bytes) {
  if (g_batch && g_cursor + bytes - g_done_seen <= kBytes) [[likely]] {
    return;
  }
  g_done_seen = g_done.load(std::memory_order_acquire);
  if (g_cursor + bytes - g_done_seen <= kBytes) [[likely]] {
    return;
  }
  ++g_room_waits;
  Publish();
  while (g_cursor + bytes - g_done.load(std::memory_order_acquire) > kBytes) {
    std::this_thread::yield();
  }
  g_done_seen = g_done.load(std::memory_order_acquire);
}

// Room for one record of 'payload' bytes of arguments; Commit() publishes it.
uint8_t* Reserve(uint32_t op, size_t payload) {
  const uint64_t size = (sizeof(Header) + payload + 7) & ~uint64_t(7);
  uint64_t index = g_cursor % kBytes;
  if (index + size > kBytes) {  // not before the end: a skip record to the end, then from the start
    const uint64_t rest = kBytes - index;
    WaitRoom(rest);
    auto* skip = reinterpret_cast<Header*>(g_buffer + index);
    skip->op = kSkip;
    skip->size = uint32_t(rest);
    g_cursor += rest;
    index = 0;
  }
  WaitRoom(size);
  auto* header = reinterpret_cast<Header*>(g_buffer + index);
  header->op = op;
  header->size = uint32_t(size);
  g_pending = uint32_t(size);
  return g_buffer + index + sizeof(Header);
}

// A record is published at once (the recorder, when awake, takes it); whether the recorder must be woken is looked at
// once kWakeBytes more were queued (see the header comment) and in Drain.
constexpr uint64_t kWakeBytes = 4096;
void Commit() {
  g_cursor += g_pending;
  g_bytes += g_pending;
  ++g_records;
  if (!g_batch || g_cursor - g_published >= kPublishBytes) {
    g_written.store(g_cursor, std::memory_order_release);
    g_published = g_cursor;
  }
  if (g_cursor - g_looked >= kWakeBytes) {
    if (g_published != g_cursor) {  // the recorder may be asleep: it must see everything before the wake
      g_written.store(g_cursor, std::memory_order_release);
      g_published = g_cursor;
    }
    Wake();
  }
}

template <typename T>
T* Put(uint32_t op, size_t extra = 0) {
  static_assert(alignof(T) <= 8 && sizeof(T) % 8 == 0, "arguments are 8-byte records");
  return reinterpret_cast<T*>(Reserve(op, sizeof(T) + extra));
}

template <typename T, typename A>
T* After(A* args, size_t skip_bytes = 0) {
  return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(args + 1) + skip_bytes);
}

// The ring thread waits until the recorder made every call queued so far.
void Drain() {
  if (g_done.load(std::memory_order_acquire) == g_cursor) {
    return;
  }
  Publish();
  // A short spin: on the Switch the two threads share a priority, and a spin on the recorder's core would hold it off.
  const auto spin_until = Clock::now() + std::chrono::microseconds(10);
  for (int i = 0;; ++i) {
    if (g_done.load(std::memory_order_acquire) == g_cursor) {
      return;
    }
    CpuRelax();
    if ((i & 63) == 63 && Clock::now() >= spin_until) {
      break;
    }
  }
  std::unique_lock lock(g_mutex);
  g_producer_waiting.store(true, std::memory_order_seq_cst);
  g_cv_producer.wait(lock, [] { return g_done.load(std::memory_order_seq_cst) == g_cursor; });
  g_producer_waiting.store(false, std::memory_order_seq_cst);
}

// Is this call on the deferred command buffer (and from the ring thread)?
bool Mine(VkCommandBuffer commands) {
  if (commands != g_target.load(std::memory_order_relaxed) || commands == VK_NULL_HANDLE) [[likely]] {
    return false;
  }
  if (!t_producer) [[unlikely]] {
    if (g_foreign.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_ERROR("[native] deferred recording: the job buffer was recorded from another thread than the ring's; "
                   "that call was made at once (its order against the queued ones is not kept)");
    }
    return false;
  }
  return true;
}

// --- Calls without a queued form: drain, then call at once --------------------------------------------------------
template <typename Fn>
struct FirstIsCommandBuffer : std::false_type {};
template <typename... A>
struct FirstIsCommandBuffer<void(VKAPI_PTR*)(VkCommandBuffer, A...)> : std::true_type {};

template <auto Member, typename Fn>
struct Direct;
template <auto Member, typename... A>
struct Direct<Member, void(VKAPI_PTR*)(VkCommandBuffer, A...)> {
  static void VKAPI_PTR Call(VkCommandBuffer commands, A... args) {
    if (Mine(commands)) {
      Drain();
      ++g_drains_direct;
    }
    (g_real.*Member)(commands, args...);
  }
};

template <auto Member, typename Fn>
void InstallDirect(Functions& table) {
  if constexpr (FirstIsCommandBuffer<Fn>::value) {
    if (g_real.*Member) {
      table.*Member = &Direct<Member, Fn>::Call;
    }
  }
}

// --- Queued calls -----------------------------------------------------------------------------------------------
struct alignas(8) ABindPipeline {
  VkPipelineBindPoint point;
  VkPipeline pipeline;
};
void VKAPI_PTR CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline pipeline) {
  if (!Mine(c)) {
    return g_real.vkCmdBindPipeline(c, point, pipeline);
  }
  auto* a = Put<ABindPipeline>(kBindPipeline);
  a->point = point;
  a->pipeline = pipeline;
  Commit();
}

struct alignas(8) ABindDescriptorSets {
  VkPipelineBindPoint point;
  VkPipelineLayout layout;
  uint32_t first, count, dynamic;
};
void VKAPI_PTR CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout,
                                     uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dynamic,
                                     const uint32_t* offsets) {
  if (!Mine(c)) {
    return g_real.vkCmdBindDescriptorSets(c, point, layout, first, count, sets, dynamic, offsets);
  }
  auto* a = Put<ABindDescriptorSets>(kBindDescriptorSets, count * sizeof(VkDescriptorSet) + dynamic * 4);
  a->point = point;
  a->layout = layout;
  a->first = first;
  a->count = count;
  a->dynamic = dynamic;
  std::memcpy(After<VkDescriptorSet>(a), sets, count * sizeof(VkDescriptorSet));
  if (dynamic) {
    std::memcpy(After<uint32_t>(a, count * sizeof(VkDescriptorSet)), offsets, dynamic * 4);
  }
  Commit();
}

struct alignas(8) APushConstants {
  VkPipelineLayout layout;
  VkShaderStageFlags stages;
  uint32_t offset, size;
};
void VKAPI_PTR CmdPushConstants(VkCommandBuffer c, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                uint32_t size, const void* values) {
  if (!Mine(c)) {
    return g_real.vkCmdPushConstants(c, layout, stages, offset, size, values);
  }
  auto* a = Put<APushConstants>(kPushConstants, size);
  a->layout = layout;
  a->stages = stages;
  a->offset = offset;
  a->size = size;
  std::memcpy(After<uint8_t>(a), values, size);
  Commit();
}

struct alignas(8) ARange {
  uint32_t first, count;
};
void VKAPI_PTR CmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t count, const VkViewport* viewports) {
  if (!Mine(c)) {
    return g_real.vkCmdSetViewport(c, first, count, viewports);
  }
  auto* a = Put<ARange>(kSetViewport, count * sizeof(VkViewport));
  a->first = first;
  a->count = count;
  std::memcpy(After<VkViewport>(a), viewports, count * sizeof(VkViewport));
  Commit();
}
void VKAPI_PTR CmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t count, const VkRect2D* scissors) {
  if (!Mine(c)) {
    return g_real.vkCmdSetScissor(c, first, count, scissors);
  }
  auto* a = Put<ARange>(kSetScissor, count * sizeof(VkRect2D));
  a->first = first;
  a->count = count;
  std::memcpy(After<VkRect2D>(a), scissors, count * sizeof(VkRect2D));
  Commit();
}

struct alignas(8) AFloats4 {
  float v[4];
};
void VKAPI_PTR CmdSetBlendConstants(VkCommandBuffer c, const float constants[4]) {
  if (!Mine(c)) {
    return g_real.vkCmdSetBlendConstants(c, constants);
  }
  auto* a = Put<AFloats4>(kSetBlendConstants);
  std::memcpy(a->v, constants, sizeof(a->v));
  Commit();
}
void VKAPI_PTR CmdSetDepthBias(VkCommandBuffer c, float constant, float clamp, float slope) {
  if (!Mine(c)) {
    return g_real.vkCmdSetDepthBias(c, constant, clamp, slope);
  }
  auto* a = Put<AFloats4>(kSetDepthBias);
  a->v[0] = constant;
  a->v[1] = clamp;
  a->v[2] = slope;
  Commit();
}

struct alignas(8) AFaceValue {
  VkStencilFaceFlags faces;
  uint32_t value;
};
void VKAPI_PTR CmdSetStencilReference(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t value) {
  if (!Mine(c)) {
    return g_real.vkCmdSetStencilReference(c, faces, value);
  }
  auto* a = Put<AFaceValue>(kSetStencilReference);
  a->faces = faces;
  a->value = value;
  Commit();
}
void VKAPI_PTR CmdSetStencilCompareMask(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t value) {
  if (!Mine(c)) {
    return g_real.vkCmdSetStencilCompareMask(c, faces, value);
  }
  auto* a = Put<AFaceValue>(kSetStencilCompareMask);
  a->faces = faces;
  a->value = value;
  Commit();
}
void VKAPI_PTR CmdSetStencilWriteMask(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t value) {
  if (!Mine(c)) {
    return g_real.vkCmdSetStencilWriteMask(c, faces, value);
  }
  auto* a = Put<AFaceValue>(kSetStencilWriteMask);
  a->faces = faces;
  a->value = value;
  Commit();
}

void VKAPI_PTR CmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                    const VkDeviceSize* offsets) {
  if (!Mine(c)) {
    return g_real.vkCmdBindVertexBuffers(c, first, count, buffers, offsets);
  }
  auto* a = Put<ARange>(kBindVertexBuffers, count * (sizeof(VkBuffer) + sizeof(VkDeviceSize)));
  a->first = first;
  a->count = count;
  std::memcpy(After<VkBuffer>(a), buffers, count * sizeof(VkBuffer));
  std::memcpy(After<VkDeviceSize>(a, count * sizeof(VkBuffer)), offsets, count * sizeof(VkDeviceSize));
  Commit();
}

struct alignas(8) ABindIndexBuffer {
  VkBuffer buffer;
  VkDeviceSize offset;
  VkIndexType type;
};
void VKAPI_PTR CmdBindIndexBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
  if (!Mine(c)) {
    return g_real.vkCmdBindIndexBuffer(c, buffer, offset, type);
  }
  auto* a = Put<ABindIndexBuffer>(kBindIndexBuffer);
  a->buffer = buffer;
  a->offset = offset;
  a->type = type;
  Commit();
}

struct alignas(8) ADraw {
  uint32_t a, b, c, d, e;
};
void VKAPI_PTR CmdDraw(VkCommandBuffer c, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance) {
  if (!Mine(c)) {
    return g_real.vkCmdDraw(c, vertices, instances, first_vertex, first_instance);
  }
  auto* a = Put<ADraw>(kDraw);
  a->a = vertices;
  a->b = instances;
  a->c = first_vertex;
  a->d = first_instance;
  Commit();
}
void VKAPI_PTR CmdDrawIndexed(VkCommandBuffer c, uint32_t indices, uint32_t instances, uint32_t first_index,
                              int32_t vertex_offset, uint32_t first_instance) {
  if (!Mine(c)) {
    return g_real.vkCmdDrawIndexed(c, indices, instances, first_index, vertex_offset, first_instance);
  }
  auto* a = Put<ADraw>(kDrawIndexed);
  a->a = indices;
  a->b = instances;
  a->c = first_index;
  a->d = uint32_t(vertex_offset);
  a->e = first_instance;
  Commit();
}

struct alignas(8) ABeginRenderPass {
  VkRenderPass pass;
  VkFramebuffer framebuffer;
  VkRect2D area;
  uint32_t clears;
  VkSubpassContents contents;
};
void VKAPI_PTR CmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
  if (!Mine(c) || info->pNext) {
    if (Mine(c)) {
      Drain();
      ++g_drains_direct;
    }
    return g_real.vkCmdBeginRenderPass(c, info, contents);
  }
  auto* a = Put<ABeginRenderPass>(kBeginRenderPass, info->clearValueCount * sizeof(VkClearValue));
  a->pass = info->renderPass;
  a->framebuffer = info->framebuffer;
  a->area = info->renderArea;
  a->clears = info->clearValueCount;
  a->contents = contents;
  if (info->clearValueCount) {
    std::memcpy(After<VkClearValue>(a), info->pClearValues, info->clearValueCount * sizeof(VkClearValue));
  }
  Commit();
}
void VKAPI_PTR CmdEndRenderPass(VkCommandBuffer c) {
  if (!Mine(c)) {
    return g_real.vkCmdEndRenderPass(c);
  }
  Reserve(kEndRenderPass, 0);
  Commit();
}

struct alignas(8) APipelineBarrier {
  VkPipelineStageFlags source, destination;
  VkDependencyFlags dependencies;
  uint32_t memory, buffers, images;
};
void VKAPI_PTR CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags source, VkPipelineStageFlags destination,
                                  VkDependencyFlags dependencies, uint32_t memory_count, const VkMemoryBarrier* memory,
                                  uint32_t buffer_count, const VkBufferMemoryBarrier* buffers, uint32_t image_count,
                                  const VkImageMemoryBarrier* images) {
  bool chains = false;
  for (uint32_t i = 0; i < memory_count; ++i) {
    chains |= memory[i].pNext != nullptr;
  }
  for (uint32_t i = 0; i < buffer_count; ++i) {
    chains |= buffers[i].pNext != nullptr;
  }
  for (uint32_t i = 0; i < image_count; ++i) {
    chains |= images[i].pNext != nullptr;
  }
  if (!Mine(c) || chains) {
    if (chains && Mine(c)) {
      Drain();
      ++g_drains_direct;
    }
    return g_real.vkCmdPipelineBarrier(c, source, destination, dependencies, memory_count, memory, buffer_count,
                                       buffers, image_count, images);
  }
  const size_t m = memory_count * sizeof(VkMemoryBarrier);
  const size_t b = buffer_count * sizeof(VkBufferMemoryBarrier);
  const size_t i = image_count * sizeof(VkImageMemoryBarrier);
  auto* a = Put<APipelineBarrier>(kPipelineBarrier, m + b + i);
  a->source = source;
  a->destination = destination;
  a->dependencies = dependencies;
  a->memory = memory_count;
  a->buffers = buffer_count;
  a->images = image_count;
  std::memcpy(After<uint8_t>(a), memory, m);
  std::memcpy(After<uint8_t>(a, m), buffers, b);
  std::memcpy(After<uint8_t>(a, m + b), images, i);
  Commit();
}

struct alignas(8) AClearColorImage {
  VkImage image;
  VkImageLayout layout;
  VkClearColorValue color;
  uint32_t ranges;
};
void VKAPI_PTR CmdClearColorImage(VkCommandBuffer c, VkImage image, VkImageLayout layout,
                                  const VkClearColorValue* color, uint32_t count,
                                  const VkImageSubresourceRange* ranges) {
  if (!Mine(c)) {
    return g_real.vkCmdClearColorImage(c, image, layout, color, count, ranges);
  }
  auto* a = Put<AClearColorImage>(kClearColorImage, count * sizeof(VkImageSubresourceRange));
  a->image = image;
  a->layout = layout;
  a->color = *color;
  a->ranges = count;
  std::memcpy(After<VkImageSubresourceRange>(a), ranges, count * sizeof(VkImageSubresourceRange));
  Commit();
}

struct alignas(8) AClearDepthStencilImage {
  VkImage image;
  VkImageLayout layout;
  VkClearDepthStencilValue value;
  uint32_t ranges;
};
void VKAPI_PTR CmdClearDepthStencilImage(VkCommandBuffer c, VkImage image, VkImageLayout layout,
                                         const VkClearDepthStencilValue* value, uint32_t count,
                                         const VkImageSubresourceRange* ranges) {
  if (!Mine(c)) {
    return g_loaded.clear_depth(c, image, layout, value, count, ranges);
  }
  auto* a = Put<AClearDepthStencilImage>(kClearDepthStencilImage, count * sizeof(VkImageSubresourceRange));
  a->image = image;
  a->layout = layout;
  a->value = *value;
  a->ranges = count;
  std::memcpy(After<VkImageSubresourceRange>(a), ranges, count * sizeof(VkImageSubresourceRange));
  Commit();
}

struct alignas(8) AClearAttachments {
  uint32_t attachments, rects;
};
void VKAPI_PTR CmdClearAttachments(VkCommandBuffer c, uint32_t attachment_count, const VkClearAttachment* attachments,
                                   uint32_t rect_count, const VkClearRect* rects) {
  if (!Mine(c)) {
    return g_real.vkCmdClearAttachments(c, attachment_count, attachments, rect_count, rects);
  }
  const size_t na = attachment_count * sizeof(VkClearAttachment);
  const size_t nr = rect_count * sizeof(VkClearRect);
  auto* a = Put<AClearAttachments>(kClearAttachments, na + ((na + 7) & ~size_t(7)) - na + nr);
  a->attachments = attachment_count;
  a->rects = rect_count;
  std::memcpy(After<uint8_t>(a), attachments, na);
  std::memcpy(After<uint8_t>(a, (na + 7) & ~size_t(7)), rects, nr);
  Commit();
}

struct alignas(8) ABufferImage {
  VkBuffer buffer;
  VkImage image;
  VkImageLayout layout;
  uint32_t regions;
};
void VKAPI_PTR CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer buffer, VkImage image, VkImageLayout layout,
                                    uint32_t count, const VkBufferImageCopy* regions) {
  if (!Mine(c)) {
    return g_real.vkCmdCopyBufferToImage(c, buffer, image, layout, count, regions);
  }
  auto* a = Put<ABufferImage>(kCopyBufferToImage, count * sizeof(VkBufferImageCopy));
  a->buffer = buffer;
  a->image = image;
  a->layout = layout;
  a->regions = count;
  std::memcpy(After<VkBufferImageCopy>(a), regions, count * sizeof(VkBufferImageCopy));
  Commit();
}
void VKAPI_PTR CmdCopyImageToBuffer(VkCommandBuffer c, VkImage image, VkImageLayout layout, VkBuffer buffer,
                                    uint32_t count, const VkBufferImageCopy* regions) {
  if (!Mine(c)) {
    return g_real.vkCmdCopyImageToBuffer(c, image, layout, buffer, count, regions);
  }
  auto* a = Put<ABufferImage>(kCopyImageToBuffer, count * sizeof(VkBufferImageCopy));
  a->buffer = buffer;
  a->image = image;
  a->layout = layout;
  a->regions = count;
  std::memcpy(After<VkBufferImageCopy>(a), regions, count * sizeof(VkBufferImageCopy));
  Commit();
}

struct alignas(8) ACopyImage {
  VkImage source;
  VkImageLayout source_layout;
  VkImage destination;
  VkImageLayout destination_layout;
  uint32_t regions;
};
void VKAPI_PTR CmdCopyImage(VkCommandBuffer c, VkImage source, VkImageLayout source_layout, VkImage destination,
                            VkImageLayout destination_layout, uint32_t count, const VkImageCopy* regions) {
  if (!Mine(c)) {
    return g_loaded.copy_image(c, source, source_layout, destination, destination_layout, count, regions);
  }
  auto* a = Put<ACopyImage>(kCopyImage, count * sizeof(VkImageCopy));
  a->source = source;
  a->source_layout = source_layout;
  a->destination = destination;
  a->destination_layout = destination_layout;
  a->regions = count;
  std::memcpy(After<VkImageCopy>(a), regions, count * sizeof(VkImageCopy));
  Commit();
}

struct alignas(8) ABlitImage {
  VkImage source;
  VkImageLayout source_layout;
  VkImage destination;
  VkImageLayout destination_layout;
  uint32_t regions;
  VkFilter filter;
};
void VKAPI_PTR CmdBlitImage(VkCommandBuffer c, VkImage source, VkImageLayout source_layout, VkImage destination,
                            VkImageLayout destination_layout, uint32_t count, const VkImageBlit* regions,
                            VkFilter filter) {
  if (!Mine(c)) {
    return g_loaded.blit(c, source, source_layout, destination, destination_layout, count, regions, filter);
  }
  auto* a = Put<ABlitImage>(kBlitImage, count * sizeof(VkImageBlit));
  a->source = source;
  a->source_layout = source_layout;
  a->destination = destination;
  a->destination_layout = destination_layout;
  a->regions = count;
  a->filter = filter;
  std::memcpy(After<VkImageBlit>(a), regions, count * sizeof(VkImageBlit));
  Commit();
}

struct alignas(8) ACopyBuffer {
  VkBuffer source, destination;
  uint32_t regions;
};
void VKAPI_PTR CmdCopyBuffer(VkCommandBuffer c, VkBuffer source, VkBuffer destination, uint32_t count,
                             const VkBufferCopy* regions) {
  if (!Mine(c)) {
    return g_real.vkCmdCopyBuffer(c, source, destination, count, regions);
  }
  auto* a = Put<ACopyBuffer>(kCopyBuffer, count * sizeof(VkBufferCopy));
  a->source = source;
  a->destination = destination;
  a->regions = count;
  std::memcpy(After<VkBufferCopy>(a), regions, count * sizeof(VkBufferCopy));
  Commit();
}

struct alignas(8) AQuery {
  VkQueryPool pool;
  uint32_t first, count;
  VkQueryControlFlags flags;
  VkPipelineStageFlagBits stage;
};
void VKAPI_PTR CmdResetQueryPool(VkCommandBuffer c, VkQueryPool pool, uint32_t first, uint32_t count) {
  if (!Mine(c)) {
    return g_real.vkCmdResetQueryPool(c, pool, first, count);
  }
  auto* a = Put<AQuery>(kResetQueryPool);
  a->pool = pool;
  a->first = first;
  a->count = count;
  Commit();
}
void VKAPI_PTR CmdBeginQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags) {
  if (!Mine(c)) {
    return g_real.vkCmdBeginQuery(c, pool, query, flags);
  }
  auto* a = Put<AQuery>(kBeginQuery);
  a->pool = pool;
  a->first = query;
  a->flags = flags;
  Commit();
}
void VKAPI_PTR CmdEndQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t query) {
  if (!Mine(c)) {
    return g_real.vkCmdEndQuery(c, pool, query);
  }
  auto* a = Put<AQuery>(kEndQuery);
  a->pool = pool;
  a->first = query;
  Commit();
}
void VKAPI_PTR CmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
  if (!Mine(c)) {
    return g_loaded.timestamp(c, stage, pool, query);
  }
  auto* a = Put<AQuery>(kWriteTimestamp);
  a->pool = pool;
  a->first = query;
  a->stage = stage;
  Commit();
}

// The extended dynamic state setters (loaded separately).
struct alignas(8) AValue {
  uint32_t value;
};
template <uint32_t OpCode, auto Field, typename T>
void VKAPI_PTR SetOne(VkCommandBuffer c, T value) {
  if (!Mine(c)) {
    return (g_loaded.*Field)(c, value);
  }
  auto* a = Put<AValue>(OpCode);
  a->value = uint32_t(value);
  Commit();
}

struct alignas(8) AStencilOp {
  VkStencilFaceFlags faces;
  VkStencilOp fail, pass, depth_fail;
  VkCompareOp compare;
};
void VKAPI_PTR CmdSetStencilOp(VkCommandBuffer c, VkStencilFaceFlags faces, VkStencilOp fail, VkStencilOp pass,
                               VkStencilOp depth_fail, VkCompareOp compare) {
  if (!Mine(c)) {
    return g_loaded.stencil_op(c, faces, fail, pass, depth_fail, compare);
  }
  auto* a = Put<AStencilOp>(kSetStencilOp);
  a->faces = faces;
  a->fail = fail;
  a->pass = pass;
  a->depth_fail = depth_fail;
  a->compare = compare;
  Commit();
}

template <uint32_t OpCode, auto Field, typename T>
void VKAPI_PTR SetMany(VkCommandBuffer c, uint32_t first, uint32_t count, const T* values) {
  if (!Mine(c)) {
    return (g_loaded.*Field)(c, first, count, values);
  }
  auto* a = Put<ARange>(OpCode, count * sizeof(T));
  a->first = first;
  a->count = count;
  std::memcpy(After<T>(a), values, count * sizeof(T));
  Commit();
}

struct alignas(8) APrefetch {
  void (*prefetch)(VkPipeline);
  VkPipeline pipeline;
};

// --- The recorder -------------------------------------------------------------------------------------------------
void Execute(const Header* h) {
  const VkCommandBuffer c = g_recording;
  const auto* p = reinterpret_cast<const uint8_t*>(h + 1);
  switch (h->op) {
    case kSkip:
      break;
    case kTarget: {
      std::memcpy(&g_recording, p, sizeof(g_recording));
      break;
    }
    case kBindPipeline: {
      const auto* a = reinterpret_cast<const ABindPipeline*>(p);
      g_real.vkCmdBindPipeline(c, a->point, a->pipeline);
      break;
    }
    case kBindDescriptorSets: {
      const auto* a = reinterpret_cast<const ABindDescriptorSets*>(p);
      const auto* sets = reinterpret_cast<const VkDescriptorSet*>(a + 1);
      g_real.vkCmdBindDescriptorSets(c, a->point, a->layout, a->first, a->count, sets, a->dynamic,
                                     reinterpret_cast<const uint32_t*>(sets + a->count));
      break;
    }
    case kPushConstants: {
      const auto* a = reinterpret_cast<const APushConstants*>(p);
      g_real.vkCmdPushConstants(c, a->layout, a->stages, a->offset, a->size, a + 1);
      break;
    }
    case kSetViewport: {
      const auto* a = reinterpret_cast<const ARange*>(p);
      g_real.vkCmdSetViewport(c, a->first, a->count, reinterpret_cast<const VkViewport*>(a + 1));
      break;
    }
    case kSetScissor: {
      const auto* a = reinterpret_cast<const ARange*>(p);
      g_real.vkCmdSetScissor(c, a->first, a->count, reinterpret_cast<const VkRect2D*>(a + 1));
      break;
    }
    case kSetBlendConstants: {
      g_real.vkCmdSetBlendConstants(c, reinterpret_cast<const AFloats4*>(p)->v);
      break;
    }
    case kSetDepthBias: {
      const auto* a = reinterpret_cast<const AFloats4*>(p);
      g_real.vkCmdSetDepthBias(c, a->v[0], a->v[1], a->v[2]);
      break;
    }
    case kSetStencilReference: {
      const auto* a = reinterpret_cast<const AFaceValue*>(p);
      g_real.vkCmdSetStencilReference(c, a->faces, a->value);
      break;
    }
    case kSetStencilCompareMask: {
      const auto* a = reinterpret_cast<const AFaceValue*>(p);
      g_real.vkCmdSetStencilCompareMask(c, a->faces, a->value);
      break;
    }
    case kSetStencilWriteMask: {
      const auto* a = reinterpret_cast<const AFaceValue*>(p);
      g_real.vkCmdSetStencilWriteMask(c, a->faces, a->value);
      break;
    }
    case kBindVertexBuffers: {
      const auto* a = reinterpret_cast<const ARange*>(p);
      const auto* buffers = reinterpret_cast<const VkBuffer*>(a + 1);
      g_real.vkCmdBindVertexBuffers(c, a->first, a->count, buffers,
                                    reinterpret_cast<const VkDeviceSize*>(buffers + a->count));
      break;
    }
    case kBindIndexBuffer: {
      const auto* a = reinterpret_cast<const ABindIndexBuffer*>(p);
      g_real.vkCmdBindIndexBuffer(c, a->buffer, a->offset, a->type);
      break;
    }
    case kDraw: {
      const auto* a = reinterpret_cast<const ADraw*>(p);
      g_real.vkCmdDraw(c, a->a, a->b, a->c, a->d);
      break;
    }
    case kDrawIndexed: {
      const auto* a = reinterpret_cast<const ADraw*>(p);
      g_real.vkCmdDrawIndexed(c, a->a, a->b, a->c, int32_t(a->d), a->e);
      break;
    }
    case kBeginRenderPass: {
      const auto* a = reinterpret_cast<const ABeginRenderPass*>(p);
      VkRenderPassBeginInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      info.renderPass = a->pass;
      info.framebuffer = a->framebuffer;
      info.renderArea = a->area;
      info.clearValueCount = a->clears;
      info.pClearValues = a->clears ? reinterpret_cast<const VkClearValue*>(a + 1) : nullptr;
      g_real.vkCmdBeginRenderPass(c, &info, a->contents);
      break;
    }
    case kEndRenderPass:
      g_real.vkCmdEndRenderPass(c);
      break;
    case kPipelineBarrier: {
      const auto* a = reinterpret_cast<const APipelineBarrier*>(p);
      const auto* memory = reinterpret_cast<const VkMemoryBarrier*>(a + 1);
      const auto* buffers = reinterpret_cast<const VkBufferMemoryBarrier*>(memory + a->memory);
      const auto* images = reinterpret_cast<const VkImageMemoryBarrier*>(buffers + a->buffers);
      g_real.vkCmdPipelineBarrier(c, a->source, a->destination, a->dependencies, a->memory,
                                  a->memory ? memory : nullptr, a->buffers, a->buffers ? buffers : nullptr,
                                  a->images, a->images ? images : nullptr);
      break;
    }
    case kClearColorImage: {
      const auto* a = reinterpret_cast<const AClearColorImage*>(p);
      g_real.vkCmdClearColorImage(c, a->image, a->layout, &a->color, a->ranges,
                                  reinterpret_cast<const VkImageSubresourceRange*>(a + 1));
      break;
    }
    case kClearDepthStencilImage: {
      const auto* a = reinterpret_cast<const AClearDepthStencilImage*>(p);
      g_loaded.clear_depth(c, a->image, a->layout, &a->value, a->ranges,
                                         reinterpret_cast<const VkImageSubresourceRange*>(a + 1));
      break;
    }
    case kClearAttachments: {
      const auto* a = reinterpret_cast<const AClearAttachments*>(p);
      const size_t na = a->attachments * sizeof(VkClearAttachment);
      const auto* attachments = reinterpret_cast<const VkClearAttachment*>(a + 1);
      const auto* rects =
          reinterpret_cast<const VkClearRect*>(reinterpret_cast<const uint8_t*>(a + 1) + ((na + 7) & ~size_t(7)));
      g_real.vkCmdClearAttachments(c, a->attachments, attachments, a->rects, rects);
      break;
    }
    case kCopyBufferToImage: {
      const auto* a = reinterpret_cast<const ABufferImage*>(p);
      g_real.vkCmdCopyBufferToImage(c, a->buffer, a->image, a->layout, a->regions,
                                    reinterpret_cast<const VkBufferImageCopy*>(a + 1));
      break;
    }
    case kCopyImageToBuffer: {
      const auto* a = reinterpret_cast<const ABufferImage*>(p);
      g_real.vkCmdCopyImageToBuffer(c, a->image, a->layout, a->buffer, a->regions,
                                    reinterpret_cast<const VkBufferImageCopy*>(a + 1));
      break;
    }
    case kCopyImage: {
      const auto* a = reinterpret_cast<const ACopyImage*>(p);
      g_loaded.copy_image(c, a->source, a->source_layout, a->destination, a->destination_layout, a->regions,
                          reinterpret_cast<const VkImageCopy*>(a + 1));
      break;
    }
    case kBlitImage: {
      const auto* a = reinterpret_cast<const ABlitImage*>(p);
      g_loaded.blit(c, a->source, a->source_layout, a->destination, a->destination_layout, a->regions,
                    reinterpret_cast<const VkImageBlit*>(a + 1), a->filter);
      break;
    }
    case kCopyBuffer: {
      const auto* a = reinterpret_cast<const ACopyBuffer*>(p);
      g_real.vkCmdCopyBuffer(c, a->source, a->destination, a->regions, reinterpret_cast<const VkBufferCopy*>(a + 1));
      break;
    }
    case kResetQueryPool: {
      const auto* a = reinterpret_cast<const AQuery*>(p);
      g_real.vkCmdResetQueryPool(c, a->pool, a->first, a->count);
      break;
    }
    case kBeginQuery: {
      const auto* a = reinterpret_cast<const AQuery*>(p);
      g_real.vkCmdBeginQuery(c, a->pool, a->first, a->flags);
      break;
    }
    case kEndQuery: {
      const auto* a = reinterpret_cast<const AQuery*>(p);
      g_real.vkCmdEndQuery(c, a->pool, a->first);
      break;
    }
    case kWriteTimestamp: {
      const auto* a = reinterpret_cast<const AQuery*>(p);
      g_loaded.timestamp(c, a->stage, a->pool, a->first);
      break;
    }
    case kSetCullMode:
      g_loaded.cull(c, reinterpret_cast<const AValue*>(p)->value);
      break;
    case kSetFrontFace:
      g_loaded.front(c, VkFrontFace(reinterpret_cast<const AValue*>(p)->value));
      break;
    case kSetPrimitiveTopology:
      g_loaded.topology(c, VkPrimitiveTopology(reinterpret_cast<const AValue*>(p)->value));
      break;
    case kSetPrimitiveRestartEnable:
      g_loaded.restart(c, reinterpret_cast<const AValue*>(p)->value);
      break;
    case kSetDepthBiasEnable:
      g_loaded.bias(c, reinterpret_cast<const AValue*>(p)->value);
      break;
    case kSetDepthTestEnable:
      g_loaded.test_z(c, reinterpret_cast<const AValue*>(p)->value);
      break;
    case kSetDepthWriteEnable:
      g_loaded.write_z(c, reinterpret_cast<const AValue*>(p)->value);
      break;
    case kSetDepthCompareOp:
      g_loaded.compare_z(c, VkCompareOp(reinterpret_cast<const AValue*>(p)->value));
      break;
    case kSetStencilTestEnable:
      g_loaded.stencil(c, reinterpret_cast<const AValue*>(p)->value);
      break;
    case kSetStencilOp: {
      const auto* a = reinterpret_cast<const AStencilOp*>(p);
      g_loaded.stencil_op(c, a->faces, a->fail, a->pass, a->depth_fail, a->compare);
      break;
    }
    case kSetColorBlendEnable: {
      const auto* a = reinterpret_cast<const ARange*>(p);
      g_loaded.blend(c, a->first, a->count, reinterpret_cast<const VkBool32*>(a + 1));
      break;
    }
    case kSetColorBlendEquation: {
      const auto* a = reinterpret_cast<const ARange*>(p);
      g_loaded.equation(c, a->first, a->count, reinterpret_cast<const VkColorBlendEquationEXT*>(a + 1));
      break;
    }
    case kSetColorWriteMask: {
      const auto* a = reinterpret_cast<const ARange*>(p);
      g_loaded.mask(c, a->first, a->count, reinterpret_cast<const VkColorComponentFlags*>(a + 1));
      break;
    }
    case kPrefetch: {
      const auto* a = reinterpret_cast<const APrefetch*>(p);
      a->prefetch(a->pipeline);
      break;
    }
    default:
      REXLOG_ERROR("[native] deferred recording: unknown record {}", h->op);
      break;
  }
}

#if REX_PLATFORM_SWITCH
// The recorder's preferred core: the usual one, or bo2zm_native_recorder_core_b in the benchmark A/B's arms with
// factor 1 on. Checked before each sleep (a time read; the core is set only when it changes).
int g_core_usual = -1;
int g_core_applied = -1;
// The same for its priority (bo2zm_native_recorder_priority_b).
int g_priority_usual = -1;
int g_priority_applied = -1;
void ApplyAbPriority() {
  const int32_t b = REXCVAR_GET(bo2zm_native_recorder_priority_b);
  if (b < 0) {
    return;
  }
  const int arm = bo2zm::benchmark::AbArm();
  const int wanted = arm >= 0 && (arm & 1) ? int(b) : g_priority_usual;
  if (wanted == g_priority_applied) {
    return;
  }
  g_priority_applied = wanted;
  RexSwitchSetCurrentThreadPriorityOk(wanted);
}

void ApplyAbCore() {
  const int32_t b = REXCVAR_GET(bo2zm_native_recorder_core_b);
  if (b < -1) {
    return;
  }
  const int arm = bo2zm::benchmark::AbArm();
  const int wanted = arm >= 0 && (arm & 1) ? int(b) : g_core_usual;
  if (wanted == g_core_applied) {
    return;
  }
  g_core_applied = wanted;
  if (wanted >= 0) {
    RexSwitchSetCurrentThreadCore(wanted);
  }
}
#endif

void Loop(int ring_core) {
  rex::thread::set_current_thread_name("GPU native recorder");
#if REX_PLATFORM_SWITCH
  {
    const int32_t priority = REXCVAR_GET(bo2zm_native_recorder_priority);
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
    g_priority_usual = int(priority);
    g_priority_applied = int(priority);
    const int32_t priority_b = REXCVAR_GET(bo2zm_native_recorder_priority_b);
    if (priority_b >= 0) {
      REXLOG_INFO("[native] deferred recording: A/B factor 1 = recorder at priority {:#x} instead of {:#x} "
                  "(bo2zm_native_recorder_priority_b)", uint32_t(priority_b), uint32_t(priority));
    }
    // The vertex copy thread prefers 2, or 1 when the ring is on 2 (bo2zm_native_upload_thread_core = -2): by
    // default the recorder prefers the third core.
    const int32_t requested = REXCVAR_GET(bo2zm_native_recorder_core);
    int core = int(requested);
    if (requested == -2) {
      core = ring_core >= 0 && ring_core <= 2 ? 3 - ring_core - (ring_core == 2 ? 1 : 2) : -1;
    }
    const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
    g_core_usual = core;
    g_core_applied = core;
    const int32_t core_b = REXCVAR_GET(bo2zm_native_recorder_core_b);
    if (core_b >= -1) {
      REXLOG_INFO("[native] deferred recording: A/B factor 1 = recorder on core {} instead of {} "
                  "(bo2zm_native_recorder_core_b)", core_b, core);
    }
    REXLOG_INFO("[native] deferred recording: recorder thread at priority {:#x}{}; preferred core {}{} (the ring runs "
                "on {}), now on {}",
                uint32_t(priority), priority_ok ? "" : " (NOT accepted)", core, core_ok ? "" : " (NOT accepted)",
                ring_core, RexSwitchCurrentCore());
  }
#else
  (void)ring_core;
#endif
  uint64_t read = 0;
  for (;;) {
    uint64_t written = g_written.load(std::memory_order_acquire);
    if (read == written) {
      // A few looks only (well under a microsecond), then sleep until the ring thread has queued a batch.
      for (int i = 0; i < 16 && read == written; ++i) {
        CpuRelax();
        written = g_written.load(std::memory_order_acquire);
      }
      if (read == written) {
#if REX_PLATFORM_SWITCH
        ApplyAbCore();
        ApplyAbPriority();
#endif
        std::unique_lock lock(g_mutex);
        g_recorder_sleeping.store(true, std::memory_order_seq_cst);
        g_cv_recorder.wait(lock, [&] {
          return g_written.load(std::memory_order_seq_cst) != read || g_stop.load(std::memory_order_relaxed);
        });
        g_recorder_sleeping.store(false, std::memory_order_relaxed);
        if (g_stop.load(std::memory_order_relaxed) && g_written.load(std::memory_order_seq_cst) == read) {
          return;
        }
        continue;
      }
    }
    const auto t0 = Clock::now();
    while (read != written) {
      const auto* h = reinterpret_cast<const Header*>(g_buffer + read % kBytes);
      Execute(h);
      read += h->size;
      g_done.store(read, std::memory_order_release);
      if (read == written) {
        written = g_written.load(std::memory_order_acquire);  // more may have come meanwhile
      }
    }
    g_ns_recorder.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()),
        std::memory_order_relaxed);
    // Same pairing as Wake, the other way round: the ring thread sets g_producer_waiting before its last look at g_done.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_producer_waiting.load(std::memory_order_relaxed)) {
      std::lock_guard lock(g_mutex);
      g_cv_producer.notify_all();
    }
  }
}

void Build(const rex::ui::vulkan::VulkanDevice& device) {
  g_real = device.functions();
  g_table = g_real;
  g_enabled = REXCVAR_GET(bo2zm_native_deferred_recording);
  if (!g_enabled) {
    REXLOG_INFO("[native] deferred recording: off (bo2zm_native_deferred_recording = false): the ring thread records");
    return;
  }
  // Every vkCmd* entry: drain and call at once on the job buffer, call at once on any other.
#define XE_UI_VULKAN_FUNCTION(name) InstallDirect<&Functions::name, PFN_##name>(g_table);
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  InstallDirect<&Functions::core_name, PFN_##core_name>(g_table);
#include <rex/ui/vulkan/functions/device_1_0.inc>
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_maintenance4.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
#undef XE_UI_VULKAN_FUNCTION
  // The ones the renderer uses per draw and per pass: queued.
  g_table.vkCmdBindPipeline = &CmdBindPipeline;
  g_table.vkCmdBindDescriptorSets = &CmdBindDescriptorSets;
  g_table.vkCmdPushConstants = &CmdPushConstants;
  g_table.vkCmdSetViewport = &CmdSetViewport;
  g_table.vkCmdSetScissor = &CmdSetScissor;
  g_table.vkCmdSetBlendConstants = &CmdSetBlendConstants;
  g_table.vkCmdSetDepthBias = &CmdSetDepthBias;
  g_table.vkCmdSetStencilReference = &CmdSetStencilReference;
  g_table.vkCmdSetStencilCompareMask = &CmdSetStencilCompareMask;
  g_table.vkCmdSetStencilWriteMask = &CmdSetStencilWriteMask;
  g_table.vkCmdBindVertexBuffers = &CmdBindVertexBuffers;
  g_table.vkCmdBindIndexBuffer = &CmdBindIndexBuffer;
  g_table.vkCmdDraw = &CmdDraw;
  g_table.vkCmdDrawIndexed = &CmdDrawIndexed;
  g_table.vkCmdBeginRenderPass = &CmdBeginRenderPass;
  g_table.vkCmdEndRenderPass = &CmdEndRenderPass;
  g_table.vkCmdPipelineBarrier = &CmdPipelineBarrier;
  g_table.vkCmdClearColorImage = &CmdClearColorImage;
  g_table.vkCmdClearAttachments = &CmdClearAttachments;
  g_table.vkCmdCopyBufferToImage = &CmdCopyBufferToImage;
  g_table.vkCmdCopyImageToBuffer = &CmdCopyImageToBuffer;
  g_table.vkCmdCopyBuffer = &CmdCopyBuffer;
  g_table.vkCmdResetQueryPool = &CmdResetQueryPool;
  g_table.vkCmdBeginQuery = &CmdBeginQuery;
  g_table.vkCmdEndQuery = &CmdEndQuery;
  g_storage = std::make_unique<uint64_t[]>(kBytes / 8);
  g_buffer = reinterpret_cast<uint8_t*>(g_storage.get());
  REXLOG_INFO("[native] deferred recording: on (a recorder thread makes the job buffer's calls; {} MB queue)",
              kBytes >> 20);
}

// The recorder is created by the ring thread at its first Begin, so on the Switch it knows the ring's core.
bool StartRecorder() {
#if REX_PLATFORM_SWITCH
  const int ring_core = RexSwitchCurrentCore();
#else
  const int ring_core = -1;
#endif
  try {
    g_thread = new std::thread(Loop, ring_core);
  } catch (const std::system_error& error) {
    REXLOG_ERROR("[native] deferred recording: the recorder thread could not be created ({}): the ring thread records",
                 error.what());
    return false;
  }
  return true;
}

}  // namespace

const Functions& Table(const rex::ui::vulkan::VulkanDevice& device) {
  std::call_once(g_once, [&] { Build(device); });
  return g_table;
}

PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real) {
  if (!g_enabled || !real) {
    return real;
  }
  const std::string_view n(name);
  const auto as = [&](auto& slot, auto thunk) {
    slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(real);
    return reinterpret_cast<PFN_vkVoidFunction>(thunk);
  };
  if (n == "vkCmdSetCullMode") {
    return as(g_loaded.cull, &SetOne<kSetCullMode, &Loaded::cull, VkCullModeFlags>);
  }
  if (n == "vkCmdSetFrontFace") {
    return as(g_loaded.front, &SetOne<kSetFrontFace, &Loaded::front, VkFrontFace>);
  }
  if (n == "vkCmdSetPrimitiveTopology") {
    return as(g_loaded.topology, &SetOne<kSetPrimitiveTopology, &Loaded::topology, VkPrimitiveTopology>);
  }
  if (n == "vkCmdSetPrimitiveRestartEnable") {
    return as(g_loaded.restart, &SetOne<kSetPrimitiveRestartEnable, &Loaded::restart, VkBool32>);
  }
  if (n == "vkCmdSetDepthBiasEnable") {
    return as(g_loaded.bias, &SetOne<kSetDepthBiasEnable, &Loaded::bias, VkBool32>);
  }
  if (n == "vkCmdSetDepthTestEnable") {
    return as(g_loaded.test_z, &SetOne<kSetDepthTestEnable, &Loaded::test_z, VkBool32>);
  }
  if (n == "vkCmdSetDepthWriteEnable") {
    return as(g_loaded.write_z, &SetOne<kSetDepthWriteEnable, &Loaded::write_z, VkBool32>);
  }
  if (n == "vkCmdSetDepthCompareOp") {
    return as(g_loaded.compare_z, &SetOne<kSetDepthCompareOp, &Loaded::compare_z, VkCompareOp>);
  }
  if (n == "vkCmdSetStencilTestEnable") {
    return as(g_loaded.stencil, &SetOne<kSetStencilTestEnable, &Loaded::stencil, VkBool32>);
  }
  if (n == "vkCmdSetStencilOp") {
    return as(g_loaded.stencil_op, &CmdSetStencilOp);
  }
  if (n == "vkCmdSetColorBlendEnableEXT") {
    return as(g_loaded.blend, &SetMany<kSetColorBlendEnable, &Loaded::blend, VkBool32>);
  }
  if (n == "vkCmdSetColorBlendEquationEXT") {
    return as(g_loaded.equation, &SetMany<kSetColorBlendEquation, &Loaded::equation, VkColorBlendEquationEXT>);
  }
  if (n == "vkCmdSetColorWriteMaskEXT") {
    return as(g_loaded.mask, &SetMany<kSetColorWriteMask, &Loaded::mask, VkColorComponentFlags>);
  }
  if (n == "vkCmdWriteTimestamp") {
    return as(g_loaded.timestamp, &CmdWriteTimestamp);
  }
  if (n == "vkCmdCopyImage") {
    return as(g_loaded.copy_image, &CmdCopyImage);
  }
  if (n == "vkCmdClearDepthStencilImage") {
    return as(g_loaded.clear_depth, &CmdClearDepthStencilImage);
  }
  if (n == "vkCmdBlitImage") {
    return as(g_loaded.blit, &CmdBlitImage);
  }
  REXLOG_ERROR("[native] deferred recording: {} has no wrapper: calls with it on the job buffer would not keep their "
               "order (deferred recording must be turned off)",
               name);
  return real;
}

void Begin(VkCommandBuffer commands) {
  if (!g_enabled) {
    return;
  }
  if (!g_thread_started) {
    g_thread_started = true;
    if (!StartRecorder()) {
      g_enabled = false;  // the wrappers stay; with no target they call the real functions at once
      return;
    }
  }
  Finish();  // a recording that was never submitted: its calls are made before the pool is reused
  t_producer = true;
  auto* target = Reserve(kTarget, sizeof(VkCommandBuffer));
  std::memcpy(target, &commands, sizeof(commands));
  Commit();
  g_target.store(commands, std::memory_order_relaxed);
}

void Finish() {
  if (g_target.load(std::memory_order_relaxed) == VK_NULL_HANDLE) {
    return;
  }
  const auto t0 = Clock::now();
  Drain();
  g_target.store(VK_NULL_HANDLE, std::memory_order_relaxed);
  ++g_finishes;
  g_ns_finish += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

bool Deferring() {
  return g_target.load(std::memory_order_relaxed) != VK_NULL_HANDLE;
}

void PublishInBatches(bool on) {
  g_batch = on;
}

void Prefetch(void (*prefetch)(VkPipeline), VkPipeline pipeline) {
  if (!prefetch) {
    return;
  }
  if (!Deferring()) {
    prefetch(pipeline);
    return;
  }
  auto* a = Put<APrefetch>(kPrefetch);
  a->prefetch = prefetch;
  a->pipeline = pipeline;
  Commit();
}

void Report() {
  if (!g_enabled) {
    return;
  }
  const auto now = Clock::now();
  if (now - g_report_last < std::chrono::seconds(10)) {
    return;
  }
  const double seconds = g_report_last == Clock::time_point{}
                             ? 10.0
                             : std::chrono::duration<double>(now - g_report_last).count();
  g_report_last = now;
  const uint64_t figures[8] = {g_records,      g_bytes,
                               g_finishes,     g_ns_finish,
                               g_drains_direct, g_room_waits,
                               g_ns_recorder.load(std::memory_order_relaxed), g_wakes};
  uint64_t d[8];
  for (int i = 0; i < 8; ++i) {
    d[i] = figures[i] - g_report_previous[i];
    g_report_previous[i] = figures[i];
  }
  REXLOG_INFO("[native] deferred recording, last {:.0f} s: {} calls queued ({:.1f} MB), recorder busy {:.1f} ms/s, "
              "woken {} times; {} finishes waiting {:.3f} ms each for the recorder; {} calls made at once after a "
              "drain; {} waits for room; {} calls on the job buffer from another thread (total)",
              seconds, d[0], double(d[1]) / 1e6, double(d[6]) / 1e6 / seconds, d[7], d[2],
              d[2] ? double(d[3]) / 1e6 / double(d[2]) : 0.0, d[4], d[5], g_foreign.load(std::memory_order_relaxed));
}

}  // namespace bo2zm::native::deferred
