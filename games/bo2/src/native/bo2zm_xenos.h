// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The Xenos names the native renderer uses, on v0.11's rex::xenos. v0.10 had them in rex::graphics (xenos.h,
// registers.h); v0.11 moved the types to rex/xenos/*.h and made registers an enum. The renderer indexes its register
// array with plain integers, so the register names come as constants here, generated from the SDK's register table.

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>
#include <rex/xenos/texture.h>
#include <rex/xenos/ucode.h>

namespace rex::xenos {

// A guest 32-bit word read through the GPU's endian mode.
inline uint32_t GpuSwap(uint32_t value, Endian endian) {
  switch (endian) {
    case Endian::k8in16:
      return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
    case Endian::k8in32:
      return __builtin_bswap32(value);
    case Endian::k16in32:
      return (value << 16) | (value >> 16);
    default:
      return value;
  }
}
inline uint16_t GpuSwap(uint16_t value, Endian endian) {
  return (endian == Endian::k8in16 || endian == Endian::k8in32) ? uint16_t((value << 8) | (value >> 8)) : value;
}

// The EDRAM mode in RB_MODECONTROL (v0.10 xenos::EdramMode; v0.11 keeps the values in state.h).
enum class EdramMode : uint32_t {
  kNoOperation = 0,
  kColorDepth = 4,
  kDepthOnly = 5,
  kCopy = 6,
};

// Occlusion query results D3D reads (little endian: D3D swaps them itself; A and B are summed).
struct alignas(uint32_t) xe_gpu_depth_sample_counts {
  uint32_t Total_A, Total_B, ZFail_A, ZFail_B, ZPass_A, ZPass_B, StencilFail_A, StencilFail_B;
};
static_assert(sizeof(xe_gpu_depth_sample_counts) == 32);

inline float GpuSwap(float value, Endian endian) {
  return std::bit_cast<float>(GpuSwap(std::bit_cast<uint32_t>(value), endian));
}

}  // namespace rex::xenos

namespace rex {
// Float to the Xenos 16-bit float (v0.10 rex/math.h).
inline uint16_t float_to_xenos_half(float value, bool preserve_denormal = false, bool round_to_nearest_even = false) {
  uint32_t integer_value = std::bit_cast<uint32_t>(value);
  uint32_t abs_value = integer_value & 0x7FFFFFFFu;
  uint32_t result;
  if (abs_value >= 0x47FFE000u) {
    result = 0x7FFFu;  // saturate
  } else {
    if (abs_value < 0x38800000u) {
      if (preserve_denormal) {
        uint32_t shift = std::min(uint32_t(113u - (abs_value >> 23u)), uint32_t(24u));
        result = (0x800000u | (abs_value & 0x7FFFFFu)) >> shift;
      } else {
        result = 0u;
      }
    } else {
      result = abs_value + 0xC8000000u;  // rebias the exponent
    }
    if (round_to_nearest_even) {
      result += 0xFFFu + ((result >> 13u) & 1u);
    }
    result = (result >> 13u) & 0x7FFFu;
  }
  return uint16_t(result | ((integer_value & 0x80000000u) >> 16u));
}
}  // namespace rex

namespace rex::graphics {
namespace xenos = rex::xenos;
#define REXGPU_REGISTER(index, type, name) inline constexpr uint32_t XE_GPU_REG_##name = index;
#include <rex/xenos/register_table.inc>
#undef REXGPU_REGISTER
inline constexpr uint32_t kRegisterCount = rex::xenos::RegisterFile::kRegisterCount;
}  // namespace rex::graphics
