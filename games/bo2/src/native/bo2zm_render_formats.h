// Which Xbox 360 color formats the renderer accepts for render targets and resolve destinations.
//
// Accepting only 8_8_8_8 would reject most of BO2's frame: it draws its 3D scene into 2_10_10_10,
// 2_10_10_10_AS_10_10_10_10 and 16_16_16_16_FLOAT targets.
//
// The functions below accept BO2's four-channel formats. Most of them are stored in RGBA8 host images: 10-bit
// channels lose some precision and values above 1 are clamped, which is acceptable for what BO2 keeps in them. The
// 16-bit formats need their range and get half-float or 16-bit images instead; that choice is made in
// bo2zm_native_targets.cpp (FormatHostTarget and FormatHostResolved). The functions here only decide whether a format
// is drawable at all.

#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

namespace bo2zm_formats {

namespace xenos = rex::graphics::xenos;

// Color render target formats (RB_COLOR_INFO) kept in an RGBA8 image.
inline bool ColorApprox(uint32_t format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (F(format)) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      return true;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_FLOAT:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
    case F::k_2_10_10_10_AS_10_10_10_10:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
      static bool warned[16] = {};
      if (!warned[format & 0xF]) {
        warned[format & 0xF] = true;
        REXLOG_INFO("[native] BO2: color target with format {} stored as RGBA8 (approximate)", format);
      }
      return true;
    }
    default:
      return false;
  }
}

// Resolve destination texture formats (RB_COPY_DEST_INFO) kept in an RGBA8 image.
inline bool CopyApprox(uint32_t format) {
  using F = xenos::ColorFormat;
  switch (F(format)) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return true;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
    case F::k_10_11_11:
    case F::k_11_11_10:
    case F::k_10_11_11_AS_16_16_16_16:
    case F::k_11_11_10_AS_16_16_16_16:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT: {
      static bool warned[64] = {};
      if (!warned[format & 0x3F]) {
        warned[format & 0x3F] = true;
        REXLOG_INFO("[native] BO2: copy to a texture with format {} stored as RGBA8 (approximate)", format);
      }
      return true;
    }
    default:
      return false;
  }
}

// The storage format of a color render target: formats that only differ in blending precision
// (2_10_10_10_AS_10_10_10_10 -> 2_10_10_10, ...) share their EDRAM storage, so they must share one image.
inline uint32_t Storage(uint32_t format) {
  return uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(format)));
}

// log2 of the bytes per texel of a resolve destination, for its tiled address.
inline uint32_t Log2BytesCopy(uint32_t format) {
  using F = xenos::ColorFormat;
  return (F(format) == F::k_16_16_16_16 || F(format) == F::k_16_16_16_16_FLOAT) ? 3 : 2;
}

}  // namespace bo2zm_formats
