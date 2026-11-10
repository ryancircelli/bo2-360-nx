#pragma once

// The Metal presenter: the SDK's Presenter (mailbox of guest output images, paint thread, surface connection)
// on a CAMetalLayer, without MoltenVK. The guest output images are Metal textures; the renderer writes them
// through RefreshGuestOutput (the context hands out the texture) and the paint thread puts the newest one on
// the layer's drawable.
//
// The drawable is cleared and the guest output is scaled into it (aspect fit, centred, bilinear, with
// MetalPerformanceShaders' prebuilt kernel, so no shader is compiled). No UI layer yet: the SDK's ImGui
// drawers record Vulkan commands and are not run here.

#include "bo2zm_gpu_metal.h"

#include <rex/ui/presenter.h>

#include <array>
#include <memory>
#include <mutex>

namespace bo2zm::native::gpu::metal {

class Presenter final : public rex::ui::Presenter {
 public:
  // The guest output image to write: an id<MTLTexture> (BGRA8Unorm, private storage, render target and
  // shader read) of the frontbuffer's size, and the device's queue to write it with. Work committed to that
  // queue before RefreshGuestOutput returns is seen by the paint that shows the image.
  class RefreshContext final : public GuestOutputRefreshContext {
   public:
    RefreshContext(bool& is_8bpc_out_ref, void* texture, uint32_t width, uint32_t height, void* queue)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          texture_(texture),
          width_(width),
          height_(height),
          queue_(queue) {}
    void* texture() const { return texture_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void* queue() const { return queue_; }

   private:
    void* texture_;
    uint32_t width_;
    uint32_t height_;
    void* queue_;
  };

  static constexpr uint32_t kGuestOutputPixelFormat = 80;  // MTLPixelFormatBGRA8Unorm

  static std::unique_ptr<Presenter> Create(Device& device,
                                           HostGpuLossCallback host_gpu_loss_callback =
                                               FatalErrorHostGpuLossCallback);
  ~Presenter() override;

  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(rex::ui::RawImage& image_out) override;

  // Writes the guest output with a clear of `rgba` (0 to 1): the first Metal-drawn frames, before the
  // renderer draws on Metal.
  bool RefreshWithClear(uint32_t width, uint32_t height, const float rgba[4]);
  // Writes the guest output from a frame drawn on Metal (an id<MTLTexture>, `width` by `height`).
  bool RefreshWithImage(void* image, uint32_t width, uint32_t height);

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      rex::ui::Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct State;
  Presenter(Device& device, HostGpuLossCallback host_gpu_loss_callback);

  Device& device_;
  std::unique_ptr<State> state_;
};

}  // namespace bo2zm::native::gpu::metal
