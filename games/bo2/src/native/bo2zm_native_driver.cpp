// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
#include "bo2zm_native_driver.h"

#include <condition_variable>
#include <functional>
#include <vector>

#include <rex/cvar.h>
#include <rex/driver_context.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace bo2zm::native {

rex::Result<void> NativeDriverBase::Initialize(rex::IDriverContext& context) {
  if (registry_) {
    return {};
  }
  if (auto registered = context.GetSettingsRegistry().Register(settings_); !registered) {
    return rex::Err(rex::ErrorCategory::kConfig, "gpu.xenos settings: " + registered.error().message);
  }
  registry_ = &context.GetSettingsRegistry();
  return {};
}

void NativeDriverBase::Shutdown() {
  DetachTitle();
  presentation_.reset();
  provider_.reset();
  if (registry_) {
    registry_->Unregister(settings_);
    registry_ = nullptr;
  }
}

bool NativeDriverBase::CreateProvider(bool with_presentation) {
  if (provider_) {
    return true;
  }
  static const rex::gpu::vulkan::VulkanSettings kVulkanSettings;
  // The shader library's SPIR-V (from XenosRecomp) needs capabilities that the VkDevice only enables if they are
  // requested before it is created.
  rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
  provider_ = rex::gpu::vulkan::VulkanProvider::Create(true, with_presentation, -1, kVulkanSettings);
  if (!provider_) {
    REXLOG_ERROR("[native] could not create the Vulkan device");
  }
  return provider_ != nullptr;
}

rex::Result<void> NativeDriverBase::SetSurface(const rex::NativeWindow& window) {
  if (!CreateProvider(true)) {
    return rex::Err(rex::ErrorCategory::kRuntime, "Unable to create graphics provider");
  }
  presentation_ = rex::gpu::XenosPresentation::Create(
      window, *provider_, *this, [](bool, bool) { REXLOG_ERROR("[native] graphics device lost"); });
  if (!presentation_) {
    provider_.reset();
    return rex::Err(rex::ErrorCategory::kRuntime, "Unable to create presenter");
  }
  return {};
}

rex::Result<void> NativeDriverBase::AttachTitle(const rex::TitleInfo& info, rex::IPhysicalMemory&,
                                                rex::IPhysicalWriteWatch&, rex::IGuestInterrupts& interrupts) {
  title_info_ = info;
  interrupts_ = &interrupts;
  auto* runtime = rex::Runtime::instance();
  memory_ = runtime ? runtime->memory() : nullptr;
  if (!memory_) {
    return rex::Err(rex::ErrorCategory::kRuntime, "no guest address space");
  }
  if (!provider_ && !CreateProvider(false)) {
    return rex::Err(rex::ErrorCategory::kRuntime, "Unable to create graphics provider");
  }
  if (!OnAttach()) {
    return rex::Err(rex::ErrorCategory::kRuntime, "native renderer start failed");
  }
  return {};
}

void NativeDriverBase::DetachTitle() {
  if (!interrupts_) {
    return;
  }
  OnDetach();            // the loops end
  threads_.clear();      // joins them
  interrupts_ = nullptr;
  memory_ = nullptr;
}

bool NativeDriverBase::StartThread(const char* name, std::function<void()> body) {
  auto thread = interrupts_->StartHostThread(name, [body = std::move(body)](std::stop_token) { body(); });
  if (!thread) {
    REXLOG_ERROR("[native] could not start {}: {}", name, thread.error().message);
    return false;
  }
  threads_.push_back(std::move(*thread));
  return true;
}

void NativeDriverBase::Interrupt(uint32_t source, uint32_t cpu) {
  if (interrupts_) {
    (void)interrupts_->RaiseGraphicsInterrupt(source, cpu);
  }
}

void NativeDriverBase::NotifySwap() {
  {
    std::lock_guard lock(frame_mutex_);
    ++swap_count_;
  }
  frame_signal_.notify_all();
}

rex::gpu::FrameStatus NativeDriverBase::WaitForFrame(uint32_t timeout_ms) {
  std::unique_lock lock(frame_mutex_);
  frame_signal_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return swap_count_ != swap_seen_; });
  rex::gpu::FrameStatus status;
  status.new_frame = swap_count_ != swap_seen_;
  swap_seen_ = swap_count_;
  return status;
}

void NativeDriverBase::ResizeSurface(uint32_t width, uint32_t height) {
  if (presentation_) {
    presentation_->Resize(width, height);
  }
}

bool NativeDriverBase::BeginPresent() {
  return presentation_ && presentation_->ConnectToWindow();
}

void NativeDriverBase::RenderOverlays(ImDrawData& draw_data) {
  if (presentation_) {
    presentation_->SetOverlays(draw_data);
  }
}

void NativeDriverBase::EndPresent() {
  if (presentation_) {
    presentation_->Present();
  }
}

}  // namespace bo2zm::native
