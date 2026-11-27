// Adapted from Xenia Canary title_id_utils.h.
// Copyright 2025 Xenia Canary. All rights reserved.
// Released under the BSD license; see sdk/LICENSE.
#pragma once
#include <cstdint>

namespace rex::kernel::xam {
inline constexpr bool IsSystemExperienceTitle(uint32_t id) {
  const auto publisher = id >> 16;
  return publisher == 0x5848 || publisher == 0x584A ||
         id == 0x584E07D1 || id == 0x584E07D2;
}
inline constexpr bool IsSystemTitle(uint32_t id) {
  if (!id) return true;
  if ((id >> 24) != 'X' || (id >> 16) == 0x5841) {
    return (id >> 16) == 0xFFFE;
  }
  return true;
}
inline constexpr bool IsOriginalXboxTitle(uint32_t id) {
  if ((id >> 16) == 0xFFFE) return true;
  if ((id >> 24) == 0xFF) return false;
  return (id & 0x7FFF) < 0x7D0;
}
}  // namespace rex::kernel::xam
