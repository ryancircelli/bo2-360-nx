#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>

#include <rex/graphics/swap_hook.h>
#include <rex/graphics/metal_runtime.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/patch.h>
#include <rex/system/savepoint.h>
#include <rex/system/bench.h>

// Diagnostic patch slot, called once per swap: proves live patch reload in a running game. The
// original returns 0; a patch library may replace it (REX_PATCH(rex_diag_swap_probe, ...)) and set
// ctx.r3 to any value, which is logged when it changes.
REX_PATCH_STUB(rex_diag_swap_probe, rex_diag_swap_probe_orig)
extern "C" void rex_diag_swap_probe_orig(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }

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

namespace {
struct DropSlot {
  std::atomic<const char*> name{nullptr};
  std::atomic<uint64_t> count{0};
};
DropSlot g_drops[128];
}  // namespace
uint64_t CountDrop(const char* reason, uint64_t n) {
  for (DropSlot& s : g_drops) {
    const char* name = s.name.load(std::memory_order_acquire);
    // An empty slot is claimed; a lost race leaves the winner's name in `name`.
    if (!name && s.name.compare_exchange_strong(name, reason, std::memory_order_acq_rel)) name = reason;
    // Same literal = same pointer (hot path); the plugin's copy of a literal is matched by text.
    if (name == reason || std::strcmp(name, reason) == 0) {
      return s.count.fetch_add(n, std::memory_order_relaxed) + n;
    }
  }
  return 0;  // shortcut: table full (128 reasons, ~40 exist) drops the count; grow it if reasons multiply.
}
std::string DropCountersText() {
  std::map<std::string, uint64_t> rows;  // merges a reason claimed twice in a race
  for (DropSlot& s : g_drops) {
    const char* name = s.name.load(std::memory_order_acquire);
    if (!name) break;
    rows[name] += s.count.load(std::memory_order_relaxed);
  }
  std::string out;
  for (const auto& [name, count] : rows) {
    if (!count) continue;
    if (!out.empty()) out += ' ';
    out += name + "=" + std::to_string(count);
  }
  return out.empty() ? "none" : out;
}
void SetGpuJobHook(GpuJobHook hook) { g_job_hook.store(hook, std::memory_order_release); }
void NotifyGpuJob(uint64_t order, uint64_t ns, bool measured) {
  if (measured) rex::bench::AddGpuNs(ns);
  if (GpuJobHook hook = g_job_hook.load(std::memory_order_acquire)) hook(order, ns, measured);
}

void NotifySwap(uint32_t width, uint32_t height) {
  rex::bench::OnFrame();  // frame-locked guest clock step + deterministic bench window
  // Frame boundary: bind a freshly prepared patch library (no-op unless one is pending).
  if (rex::patch::CommitPendingPatch()) {
    REXSYS_INFO("[patch-reload] version {} ok", rex::patch::CommittedPatchVersion());
  }
  rex::system::NoteSavepointSwap();
  {
    static thread_local PPCContext probe_ctx{};
    static uint64_t last = 0;
    probe_ctx.r3.u64 = 0;
    rex_diag_swap_probe(probe_ctx, nullptr);
    if (probe_ctx.r3.u64 != last) {
      last = probe_ctx.r3.u64;
      REXSYS_INFO("[patch-probe] swap probe now returns {}", last);
    }
  }
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
std::atomic<bool>& EncodeNosyncSwitch() {
  static std::atomic<bool> requested{[] {
    const char* env = std::getenv("REX_METAL_ENCODE_NOSYNC");
    if (env && env[0]) return env[0] == '1';
    return rex::cvar::Query<bool>("metal_encode_nosync");
  }()};
  return requested;
}
}
bool EncodeNosyncRequested() { return EncodeNosyncSwitch().load(std::memory_order_relaxed); }
void RequestEncodeNosync(bool on) { EncodeNosyncSwitch().store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool>& RegEarlyOutSwitch() {
  static std::atomic<bool> requested{[] {
    const char* env = std::getenv("REX_METAL_REG_EARLY_OUT");
    if (env && env[0]) return env[0] == '1';
    return rex::cvar::Query<bool>("metal_reg_early_out");
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
    if (env && env[0]) return env[0] == '1';
    return rex::cvar::Query<bool>("metal_bindless_cache");
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
    if (env && env[0]) return env[0] == '1';
    return rex::cvar::Query<bool>("metal_draw_consts_dirty");
  }()};
  return requested;
}
}
bool DrawConstsDirtyRequested() { return DrawConstsDirtySwitch().load(std::memory_order_relaxed); }
void RequestDrawConstsDirty(bool on) { DrawConstsDirtySwitch().store(on, std::memory_order_relaxed); }
}  // namespace rex::graphics::metal
#endif
