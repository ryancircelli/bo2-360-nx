// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// v0.10's Presenter::CaptureGuestOutput gave the capture and video recorders the guest image. v0.11's presenter has
// no readback of the guest output, so it is off for now: CaptureGuestOutput reports "no image" and both recorders
// log that there is nothing to capture. (Open: a readback through the Vulkan presenter's guest output image.)

#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/metal_runtime.h>

#include "../bo2zm_native_vk.h"

namespace bo2zm::native {

// R8 G8 B8 X8 rows, stride bytes apart.
struct RawImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  std::vector<uint8_t> data;
};

// v0.11 + Metal plugin: the plugin's live presenter reads its guest output back (rex::gpu::v10::metal::CaptureGuestOutputRgba);
// the presenter argument is not needed. Without the Metal plugin there is nothing to read.
inline bool CaptureGuestOutput(rex::gpu::Presenter*, RawImage& image) {
  uint32_t w = 0, h = 0, stride = 0;
  std::vector<uint8_t> data;
  if (!rex::gpu::v10::metal::CaptureGuestOutputRgba(w, h, stride, data)) return false;
  image.width = w;
  image.height = h;
  image.stride = stride;
  image.data = std::move(data);
  return true;
}

}  // namespace bo2zm::native
