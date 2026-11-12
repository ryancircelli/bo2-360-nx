#include <atomic>

#include <rex/graphics/swap_hook.h>

namespace rex::graphics {

namespace {
std::atomic<SwapHook> g_hook{nullptr};
std::atomic<GuestFenceWriteHook> g_fence_hook{nullptr};
}

void SetGuestFenceWriteHook(GuestFenceWriteHook hook) { g_fence_hook.store(hook, std::memory_order_release); }
void NotifyGuestFenceWrite(uint32_t address, uint32_t value) {
  if (auto hook = g_fence_hook.load(std::memory_order_acquire)) hook(address, value);
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
