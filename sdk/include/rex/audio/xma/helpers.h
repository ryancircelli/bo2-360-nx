/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// This file contains some functions used to help parse XMA data.

#pragma once

#include <stdint.h>

namespace rex::audio::xma {

static constexpr uint32_t kMaxFrameLength = 0x7FFF;

// Get number of frames that /begin/ in this packet. Valid only for XMA2 packets.
inline uint8_t GetPacketFrameCount(const uint8_t* packet) {
  return packet[0] >> 2;
}

// Get the first frame offset in bits
inline uint32_t GetPacketFrameOffset(const uint8_t* packet) {
  uint32_t val =
      static_cast<uint16_t>(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3));
  return val + 32;
}

inline uint8_t GetPacketMetadata(const uint8_t* packet) {
  return packet[2] & 0x7;
}

inline bool IsPacketXma2Type(const uint8_t* packet) {
  return GetPacketMetadata(packet) == 1;
}

inline uint8_t GetPacketSkipCount(const uint8_t* packet) {
  return packet[3];
}

// Bit offset in `buffer` of the first frame that starts at or after packet `packet_index`, or
// UINT32_MAX when none starts in this buffer; `packet_index` is then where the walk continues in
// the next buffer. A packet with no frame start only continues a split frame: its own skip count
// leads to the next packet of the same stream, so a buffer that interleaves several streams does
// not merge them (xenia-canary 7015473a7, LEGO games froze). Skip 0xFF ends the stream here.
inline uint32_t FindNextFrameStart(const uint8_t* buffer, uint32_t& packet_index,
                                   uint32_t packet_count) {
  constexpr uint32_t kBytesPerPacket = 2048;
  while (packet_index < packet_count) {
    const uint8_t* packet = buffer + packet_index * kBytesPerPacket;
    const uint32_t frame_offset = GetPacketFrameOffset(packet);
    // The offset includes the 32-bit header; a frame may start in the last bits (split header).
    if (frame_offset < kBytesPerPacket * 8) {
      return packet_index * kBytesPerPacket * 8 + frame_offset;
    }
    const uint8_t skip = GetPacketSkipCount(packet);
    packet_index = skip == 0xFF ? packet_count : packet_index + skip + 1;
  }
  return UINT32_MAX;
}

}  // namespace rex::audio::xma
