/**
 ******************************************************************************
 * ReXGlue SDK: GPU trace texture load probe                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors. All rights reserved.                  *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

namespace rex::graphics::trace {

// Texture cache load probe for the GPU trace replayer (tools/gputest). The
// texture caches call NotifyTextureLoadWindow once a load has read its guest
// source and before the texture is marked up to date, which is the window in
// which a guest write must invalidate the load. The replayer records what was
// loaded there and, in race mode, applies the next recorded guest write to the
// texture's memory at that point. Compiled in only with REX_GPU_TRACE.
struct TextureLoadEvent {
  const void* texture = nullptr;  // cache object identity
  uint32_t base_address = 0;      // physical
  uint32_t base_size = 0;
  uint32_t mips_address = 0;      // physical
  uint32_t mips_size = 0;
  bool load_base = false;
  bool load_mips = false;
  uint32_t format = 0;  // xenos::TextureFormat (base format)
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  bool scaled_resolve = false;
  // 0: resident (copy from the shared memory buffer), 1: CPU guest memory.
  uint32_t source = 0;
};

class TextureLoadProbe {
 public:
  virtual ~TextureLoadProbe() = default;
  virtual void OnTextureLoadWindow(const TextureLoadEvent& event) = 0;
};

inline TextureLoadProbe*& TextureLoadProbeSlot() {
  static TextureLoadProbe* probe = nullptr;
  return probe;
}

// TextureT is TextureCache::Texture (or a backend subclass).
template <typename TextureT>
inline void NotifyTextureLoadWindow(const TextureT& texture, bool load_base, bool load_mips,
                                    uint32_t source) {
  TextureLoadProbe* probe = TextureLoadProbeSlot();
  if (!probe || (!load_base && !load_mips)) {
    return;
  }
  const auto& key = texture.key();
  TextureLoadEvent event;
  event.texture = &texture;
  event.base_address = uint32_t(key.base_page) << 12;
  event.base_size = load_base ? texture.GetGuestBaseSize() : 0;
  event.mips_address = uint32_t(key.mip_page) << 12;
  event.mips_size = load_mips ? texture.GetGuestMipsSize() : 0;
  event.load_base = load_base;
  event.load_mips = load_mips;
  event.format = uint32_t(key.format);
  event.width = key.GetWidth();
  event.height = key.GetHeight();
  event.depth = key.GetDepthOrArraySize();
  event.scaled_resolve = key.scaled_resolve != 0;
  event.source = source;
  probe->OnTextureLoadWindow(event);
}

}  // namespace rex::graphics::trace
