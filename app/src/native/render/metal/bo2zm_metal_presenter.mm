#include "bo2zm_metal_presenter.h"

#include <rex/logging.h>
#include <rex/ui/surface_mac.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstring>

namespace bo2zm::native::gpu::metal {

struct Presenter::State {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  // Guarded by `lock`: the layer (set and cleared on the UI thread, used by the paint thread) and the guest
  // output textures (replaced by the refreshing thread, read by the paint and capture threads). Command
  // buffers retain what they use, so a texture replaced while a paint still reads it stays alive until then.
  std::mutex lock;
  CAMetalLayer* layer = nil;
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<id<MTLTexture>, kGuestOutputMailboxSize> outputs{};
};

Presenter::Presenter(Device& device, HostGpuLossCallback host_gpu_loss_callback)
    : rex::ui::Presenter(host_gpu_loss_callback), device_(device), state_(std::make_unique<State>()) {
  state_->device = (__bridge id<MTLDevice>)device.native();
  state_->queue = (__bridge id<MTLCommandQueue>)device.queue();
}

Presenter::~Presenter() {
  // The paint thread calls PaintAndPresentImpl: stop it while this part of the object still exists.
  ShutdownPaintThread();
}

std::unique_ptr<Presenter> Presenter::Create(Device& device, HostGpuLossCallback host_gpu_loss_callback) {
  std::unique_ptr<Presenter> presenter(new Presenter(device, host_gpu_loss_callback));
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    REXLOG_ERROR("[native] Metal presenter: could not initialize");
    return nullptr;
  }
  REXLOG_INFO("[native] Metal presenter on {}", device.name());
  return presenter;
}

rex::ui::Surface::TypeFlags Presenter::GetSupportedSurfaceTypes() const {
  return rex::ui::Surface::kTypeFlag_CAMetalLayer;
}

Presenter::SurfacePaintConnectResult Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    rex::ui::Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height, bool was_paintable,
    bool& is_vsync_implicit_out) {
  if (new_surface.GetType() != rex::ui::Surface::kTypeIndex_CAMetalLayer) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  CAMetalLayer* layer =
      (__bridge CAMetalLayer*)static_cast<rex::ui::CAMetalLayerSurface&>(new_surface).layer();
  if (!layer) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  State& s = *state_;
  std::lock_guard<std::mutex> guard(s.lock);
  const bool unchanged = was_paintable && s.layer == layer && s.width == new_surface_width &&
                         s.height == new_surface_height;
  if (s.layer != layer) {
    layer.device = s.device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The guest output is copied into the drawable (a blit), so the drawable is not render-target only.
    layer.framebufferOnly = NO;
    // At most three frames in flight: nextDrawable waits for one to come back.
    layer.maximumDrawableCount = 3;
    layer.displaySyncEnabled = YES;
  }
  layer.drawableSize = CGSizeMake(new_surface_width, new_surface_height);
  s.layer = layer;
  s.width = new_surface_width;
  s.height = new_surface_height;
  is_vsync_implicit_out = true;
  return unchanged ? SurfacePaintConnectResult::kSuccessUnchanged : SurfacePaintConnectResult::kSuccess;
}

void Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  State& s = *state_;
  std::lock_guard<std::mutex> guard(s.lock);
  s.layer = nil;
  s.width = 0;
  s.height = 0;
}

bool Presenter::RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height,
                                       std::function<bool(GuestOutputRefreshContext& context)> refresher,
                                       bool& is_8bpc_out_ref) {
  State& s = *state_;
  id<MTLTexture> texture = nil;
  {
    std::lock_guard<std::mutex> guard(s.lock);
    texture = s.outputs[mailbox_index];
    if (!texture || texture.width != frontbuffer_width || texture.height != frontbuffer_height) {
      MTLTextureDescriptor* desc =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:frontbuffer_width
                                                            height:frontbuffer_height
                                                         mipmapped:NO];
      desc.storageMode = MTLStorageModePrivate;
      desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      texture = [s.device newTextureWithDescriptor:desc];
      if (!texture) {
        REXLOG_ERROR("[native] Metal presenter: could not make a {}x{} guest output", frontbuffer_width,
                     frontbuffer_height);
        return false;
      }
      texture.label = @"bo2zm guest output";
      s.outputs[mailbox_index] = texture;
    }
  }
  RefreshContext context(is_8bpc_out_ref, (__bridge void*)texture, frontbuffer_width, frontbuffer_height,
                         (__bridge void*)s.queue);
  return refresher(context);
}

bool Presenter::RefreshWithClear(uint32_t width, uint32_t height, const float rgba[4]) {
  return RefreshGuestOutput(width, height, width, height, [rgba](GuestOutputRefreshContext& base) {
    auto& context = static_cast<RefreshContext&>(base);
    @autoreleasepool {
      id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)context.queue();
      id<MTLCommandBuffer> commands = [queue commandBuffer];
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)context.texture();
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
      id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
      [encoder endEncoding];
      [commands commit];
    }
    context.SetIs8bpc(true);
    return true;
  });
}

Presenter::PaintResult Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  // The SDK's UI drawers (ImGui) record Vulkan commands; they are not run on this presenter yet.
  (void)execute_ui_drawers;
  State& s = *state_;
  @autoreleasepool {
    CAMetalLayer* layer = nil;
    {
      std::lock_guard<std::mutex> guard(s.lock);
      layer = s.layer;
    }
    if (!layer) {
      return PaintResult::kNotPresentedConnectionOutdated;
    }
    id<MTLTexture> output = nil;
    {
      uint32_t index = UINT32_MAX;
      std::unique_lock<std::mutex> mailbox(ConsumeGuestOutput(index, nullptr, nullptr));
      if (index != UINT32_MAX) {
        std::lock_guard<std::mutex> guard(s.lock);
        output = s.outputs[index];
      }
    }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
      return PaintResult::kNotPresented;
    }
    id<MTLTexture> target = drawable.texture;
    id<MTLCommandBuffer> commands = [s.queue commandBuffer];
    commands.label = @"bo2zm present";
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    if (output) {
      // 1:1 and centred, cropped to the drawable.
      const NSUInteger width = std::min(output.width, target.width);
      const NSUInteger height = std::min(output.height, target.height);
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      [blit copyFromTexture:output
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake((output.width - width) / 2, (output.height - height) / 2, 0)
                 sourceSize:MTLSizeMake(width, height, 1)
                  toTexture:target
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake((target.width - width) / 2, (target.height - height) / 2, 0)];
      [blit endEncoding];
    }
    [commands presentDrawable:drawable];
    [commands commit];
  }
  return PaintResult::kPresented;
}

bool Presenter::CaptureGuestOutput(rex::ui::RawImage& image_out) {
  State& s = *state_;
  id<MTLTexture> output = nil;
  {
    uint32_t index = UINT32_MAX;
    std::unique_lock<std::mutex> mailbox(ConsumeGuestOutput(index, nullptr, nullptr));
    if (index != UINT32_MAX) {
      std::lock_guard<std::mutex> guard(s.lock);
      output = s.outputs[index];
    }
  }
  if (!output) {
    return false;
  }
  @autoreleasepool {
    const NSUInteger width = output.width;
    const NSUInteger height = output.height;
    const NSUInteger stride = width * 4;
    id<MTLBuffer> buffer = [s.device newBufferWithLength:stride * height options:MTLResourceStorageModeShared];
    if (!buffer) {
      return false;
    }
    id<MTLCommandBuffer> commands = [s.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:output
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:stride
        destinationBytesPerImage:stride * height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
      return false;
    }
    image_out.width = uint32_t(width);
    image_out.height = uint32_t(height);
    image_out.stride = stride;
    image_out.data.resize(stride * height);
    // BGRA to the RawImage's R8 G8 B8 X8.
    const uint8_t* source = static_cast<const uint8_t*>(buffer.contents);
    uint8_t* dest = image_out.data.data();
    for (size_t i = 0; i < size_t(stride) * height; i += 4) {
      dest[i + 0] = source[i + 2];
      dest[i + 1] = source[i + 1];
      dest[i + 2] = source[i + 0];
      dest[i + 3] = 0xFF;
    }
  }
  return true;
}

}  // namespace bo2zm::native::gpu::metal
