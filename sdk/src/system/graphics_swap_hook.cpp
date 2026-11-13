#include <atomic>
#include <cstdlib>

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
namespace {
std::atomic<bool>& CmdcostAllSwitch() {
  static std::atomic<bool> requested{[] {
    const char* env = std::getenv("REX_METAL_CMDCOST_ALL");
    return rex::cvar::Query<bool>("metal_cmdcost_all") ||
           (env && env[0] == '1');
  }()};
  return requested;
}
}
bool CmdcostAllRequested() { return CmdcostAllSwitch().load(std::memory_order_relaxed); }
void RequestCmdcostAll(bool on) { CmdcostAllSwitch().store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool>& RegEarlyOutSwitch() {
  static std::atomic<bool> requested{[] {
    const char* env = std::getenv("REX_METAL_REG_EARLY_OUT");
    return rex::cvar::Query<bool>("metal_reg_early_out") || (env && env[0] == '1');
  }()};
  return requested;
}
}
bool RegEarlyOutRequested() { return RegEarlyOutSwitch().load(std::memory_order_relaxed); }
void RequestRegEarlyOut(bool on) { RegEarlyOutSwitch().store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool>& BindlessCacheSwitch() {
  static std::atomic<bool> requested{[] {
    const char* env = std::getenv("REX_METAL_BINDLESS_CACHE");
    return rex::cvar::Query<bool>("metal_bindless_cache") || (env && env[0] == '1');
  }()};
  return requested;
}
}
bool BindlessCacheRequested() { return BindlessCacheSwitch().load(std::memory_order_relaxed); }
void RequestBindlessCache(bool on) { BindlessCacheSwitch().store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool>& DrawConstsDirtySwitch() {
  static std::atomic<bool> requested{[] {
    const char* env = std::getenv("REX_METAL_DRAW_CONSTS_DIRTY");
    return rex::cvar::Query<bool>("metal_draw_consts_dirty") || (env && env[0] == '1');
  }()};
  return requested;
}
}
bool DrawConstsDirtyRequested() { return DrawConstsDirtySwitch().load(std::memory_order_relaxed); }
void RequestDrawConstsDirty(bool on) { DrawConstsDirtySwitch().store(on, std::memory_order_relaxed); }
}  // namespace rex::graphics::metal
#endif
