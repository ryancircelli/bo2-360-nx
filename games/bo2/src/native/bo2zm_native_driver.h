// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The part both native graphics drivers share (the skeleton in bo2zm_native_system.cpp and the full renderer in
// render/bo2zm_native_render.cpp): v0.11's rex::gpu::IXenosGraphicsDriver, with its own Vulkan provider and the SDK's
// presentation. The SDK routes the guest's GPU register accesses (the 0x7FC80000 range) to ReadRegister and
// WriteRegister, starts nothing itself, and calls RaiseGraphicsInterrupt for the guest's interrupt callback. The two
// drivers read the PM4 ring on their own host thread, as in v0.10.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/guest.h>
#include <rex/memory.h>
#include <rex/title.h>

#include "bo2zm_native_vk.h"
#include "gpu/xenos/host/xenos_presentation.h"
#include "gpu/xenos/xenos_settings.h"

namespace rex::memory {
class Memory;
}

namespace bo2zm::native {

class NativeDriverBase : public rex::gpu::IXenosGraphicsDriver, public rex::gpu::PresenterDriver {
 public:
  ~NativeDriverBase() override = default;

  std::string_view GetName() const override { return "bo2native"; }
  [[nodiscard]] rex::Result<void> Initialize(rex::IDriverContext& context) override;
  void Shutdown() override;
  [[nodiscard]] rex::Result<void> SetSurface(const rex::NativeWindow& window) override;
  [[nodiscard]] rex::Result<void> AttachTitle(const rex::TitleInfo& info, rex::IPhysicalMemory& memory,
                                              rex::IPhysicalWriteWatch& watch,
                                              rex::IGuestInterrupts& interrupts) override;
  void DetachTitle() override;

  rex::gpu::FrameStatus WaitForFrame(uint32_t timeout_ms) override;
  void ResizeSurface(uint32_t width, uint32_t height) override;
  [[nodiscard]] bool BeginPresent() override;
  void EndPresent() override;
  void RenderOverlays(ImDrawData& draw_data) override;

  bool IsVSyncEnabled() const override { return true; }
  const rex::gpu::XenosSettings& GetXenosSettings() const override { return settings_; }

 protected:
  // The driver's own start and stop, after the base has the title's memory and interrupts (AttachTitle) and before it
  // lets go of them (DetachTitle). OnAttach starts the host threads with StartThread.
  virtual bool OnAttach() = 0;
  virtual void OnDetach() = 0;

  // Runs body on a host thread that may raise guest interrupts; false if the SDK refused. Stopped with DetachTitle.
  bool StartThread(const char* name, std::function<void()> body);

  // The guest's interrupt callback, on the calling host thread (one made by StartThread).
  void Interrupt(uint32_t source, uint32_t cpu);

  // A swap finished: WaitForFrame returns.
  void NotifySwap();

  rex::gpu::Presenter* presenter() const { return presentation_ ? presentation_->GetPresenter() : nullptr; }
  rex::gpu::vulkan::VulkanProvider* provider() const { return provider_.get(); }
  bool CreateProvider(bool with_presentation);

  rex::memory::Memory* memory_ = nullptr;  // the title's address space, for TranslatePhysical
  rex::IGuestInterrupts* interrupts_ = nullptr;
  rex::TitleInfo title_info_;
  std::unique_ptr<rex::gpu::vulkan::VulkanProvider> provider_;

 private:
  struct Settings : rex::gpu::XenosSettings {};
  Settings settings_;
  rex::ISettingsRegistry* registry_ = nullptr;
  std::unique_ptr<rex::gpu::XenosPresentation> presentation_;
  std::vector<rex::ScopeGuard> threads_;
  std::mutex frame_mutex_;
  std::condition_variable frame_signal_;
  uint64_t swap_count_ = 0;
  uint64_t swap_seen_ = 0;
};

}  // namespace bo2zm::native
