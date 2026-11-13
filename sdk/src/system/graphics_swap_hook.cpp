#include <atomic>
#include <chrono>
#include <string>
#include <pthread.h>
#include <string_view>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <rex/thread_qos.h>

#include <rex/graphics/swap_hook.h>
#include <rex/graphics/metal_runtime.h>
#include <rex/cvar.h>

namespace rex::graphics {

namespace {
std::atomic<SwapHook> g_hook{nullptr};
}

void SetSwapHook(SwapHook hook) {
  g_hook.store(hook, std::memory_order_release);
}

namespace {
std::atomic<GpuJobHook> g_job_hook{nullptr};
}
PerfStats& GetPerfStats() {
  static PerfStats stats;
  return stats;
}
void SetGpuJobHook(GpuJobHook hook) { g_job_hook.store(hook, std::memory_order_release); }
void NotifyGpuJob(uint64_t order, uint64_t ns, bool measured) {
  if (GpuJobHook hook = g_job_hook.load(std::memory_order_acquire)) hook(order, ns, measured);
}

void NotifySwap(uint32_t width, uint32_t height) {
  rex::thread::NoteFrameSwap();
  if (SwapHook hook = g_hook.load(std::memory_order_acquire)) {
    hook(width, height);
  }
}

namespace {
std::atomic<GuestWriteHook> g_guest_write_hook{nullptr};
}
void SetGuestWriteHook(GuestWriteHook hook) { g_guest_write_hook.store(hook, std::memory_order_release); }
void NotifyGuestWrite() {
  if (GuestWriteHook hook = g_guest_write_hook.load(std::memory_order_acquire)) hook();
}

}  // namespace rex::graphics

#if defined(__APPLE__)
namespace rex::graphics::metal {
namespace {
std::atomic<bool>& EncodeThreadSwitch() {
  // First read by the Metal CP after its plugin cvars have been registered/configured.
  static std::atomic<bool> requested{rex::cvar::Query<bool>("metal_encode_thread")};
  return requested;
}
}
bool EncodeThreadRequested() { return EncodeThreadSwitch().load(std::memory_order_relaxed); }
void RequestEncodeThread(bool on) { EncodeThreadSwitch().store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool>& Type0BatchingSwitch() {
  static std::atomic<bool> requested{rex::cvar::Query<bool>("metal_type0_register_batching")};
  return requested;
}
}
bool Type0BatchingRequested() { return Type0BatchingSwitch().load(std::memory_order_relaxed); }
void RequestType0Batching(bool on) { Type0BatchingSwitch().store(on, std::memory_order_relaxed); }
}  // namespace rex::graphics::metal
#endif

namespace rex::thread {
namespace {
std::atomic<uint32_t> g_qos_generation{0};
std::atomic<int64_t> g_last_swap_ns{0};
std::atomic<bool>& QosSw() { static std::atomic<bool> a{rex::cvar::Query<bool>("thread_qos_critical")}; return a; }
std::atomic<bool>& ServerSw() { static std::atomic<bool> a{rex::cvar::Query<bool>("server_yield_sleep")}; return a; }
std::atomic<bool>& ShaderSw() { static std::atomic<bool> a{rex::cvar::Query<bool>("shader_pool_cap")}; return a; }
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace
bool QosCriticalRequested() { return QosSw().load(std::memory_order_relaxed); }
void RequestQosCritical(bool on) {
  if (QosSw().exchange(on) != on) g_qos_generation.fetch_add(1, std::memory_order_release);
}
bool ServerYieldSleepRequested() { return ServerSw().load(std::memory_order_relaxed); }
void RequestServerYieldSleep(bool on) { ServerSw().store(on, std::memory_order_relaxed); }
bool ShaderPoolCapRequested() { return ShaderSw().load(std::memory_order_relaxed); }
void RequestShaderPoolCap(bool on) { ShaderSw().store(on, std::memory_order_relaxed); }
uint32_t QosGeneration() { return g_qos_generation.load(std::memory_order_acquire); }
void ApplyGuestThreadQos(std::string_view name) {
#if defined(__APPLE__)
  // Audio threads keep their own real-time policy; critical threads stay user-interactive.
  if (name.find("Audio") != std::string_view::npos || name.find("Sound") != std::string_view::npos ||
      name.rfind("Main XThread", 0) == 0 || name.rfind("GPU Commands", 0) == 0) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return;
  }
  pthread_set_qos_class_self_np(QosCriticalRequested() ? QOS_CLASS_USER_INITIATED : QOS_CLASS_USER_INTERACTIVE, 0);
#else
  (void)name;
#endif
}
namespace {
const std::string& CurrentName() {
  thread_local std::string name;
  if (name.empty()) {
    char buf[64] = {};
    pthread_getname_np(pthread_self(), buf, sizeof(buf));
    name = buf;
  }
  return name;
}
}  // namespace
bool CurrentThreadIsServer() { return CurrentName().rfind("Server", 0) == 0; }
void ReapplyQosIfFlipped() {
  thread_local uint32_t seen = UINT32_MAX;
  const uint32_t g = QosGeneration();
  if (seen == g) return;
  seen = g;
  // Only act when the switch is on, or was on: off-at-start threads already have the default class.
  if (g != 0 || QosCriticalRequested()) ApplyGuestThreadQos(CurrentName());
}
void NoteFrameSwap() { g_last_swap_ns.store(NowNs(), std::memory_order_relaxed); }
bool FrameInFlight() { return NowNs() - g_last_swap_ns.load(std::memory_order_relaxed) < 100'000'000; }
}  // namespace rex::thread
