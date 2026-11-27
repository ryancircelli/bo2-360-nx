// XMA decoder unit tests on synthetic packets (no guest memory, no FFmpeg).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/helpers.h>

namespace xma = rex::audio::xma;

namespace {
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketBits = kPacketBytes * 8;
constexpr uint32_t kNoFrameStart = 0x7FFF;  // 15-bit first-frame field: no frame starts here

// XMA2 packet header: frame count (6), first frame offset in bits after the header (15),
// metadata (3, 1 = XMA2), packet skip count (8).
void SetHeader(std::vector<uint8_t>& buf, uint32_t packet, uint32_t frame_field, uint8_t skip) {
  uint8_t* p = buf.data() + packet * kPacketBytes;
  p[0] = static_cast<uint8_t>((1 << 2) | (frame_field >> 13));
  p[1] = static_cast<uint8_t>(frame_field >> 5);
  p[2] = static_cast<uint8_t>(((frame_field & 0x1F) << 3) | 1);
  p[3] = skip;
}
}  // namespace

TEST_CASE("XMA header helpers decode the synthetic header", "[xma]") {
  std::vector<uint8_t> buf(kPacketBytes);
  SetHeader(buf, 0, 1234, 7);
  REQUIRE(xma::GetPacketFrameOffset(buf.data()) == 1234 + 32);
  REQUIRE(xma::GetPacketSkipCount(buf.data()) == 7);
  REQUIRE(xma::IsPacketXma2Type(buf.data()));
}

TEST_CASE("Next frame start follows the stream's skip chain", "[xma]") {
  // Two streams interleaved: A in even packets, B in odd ones (skip 1).
  std::vector<uint8_t> buf(6 * kPacketBytes);
  SetHeader(buf, 0, 100, 1);            // A
  SetHeader(buf, 1, 200, 1);            // B
  SetHeader(buf, 2, kNoFrameStart, 1);  // A: only the tail of a split frame
  SetHeader(buf, 3, 300, 1);            // B
  SetHeader(buf, 4, 400, 1);            // A
  SetHeader(buf, 5, 500, 1);            // B

  uint32_t index = 2;
  // A sequential walk returned packet 3, B's frame, and the context decoded the other stream.
  REQUIRE(xma::FindNextFrameStart(buf.data(), index, 6) == 4 * kPacketBits + 400 + 32);
  REQUIRE(index == 4);

  index = 0;
  REQUIRE(xma::FindNextFrameStart(buf.data(), index, 6) == 100 + 32);
}

TEST_CASE("A skip chain past the buffer carries into the next buffer", "[xma]") {
  std::vector<uint8_t> buf(4 * kPacketBytes);
  SetHeader(buf, 0, 100, 2);
  SetHeader(buf, 1, 100, 2);
  SetHeader(buf, 2, 100, 2);
  SetHeader(buf, 3, kNoFrameStart, 2);  // continues at packet 6 = packet 2 of the next buffer

  uint32_t index = 3;
  REQUIRE(xma::FindNextFrameStart(buf.data(), index, 4) == UINT32_MAX);
  REQUIRE(index == 6);  // AdvanceInputPacket subtracts the count: packet 2 of the other buffer
}

TEST_CASE("Skip 0xFF on a frameless packet ends the stream in this buffer", "[xma]") {
  std::vector<uint8_t> buf(3 * kPacketBytes);
  SetHeader(buf, 0, kNoFrameStart, 0xFF);
  SetHeader(buf, 1, 100, 0);
  SetHeader(buf, 2, 100, 0);

  uint32_t index = 0;
  REQUIRE(xma::FindNextFrameStart(buf.data(), index, 3) == UINT32_MAX);
  REQUIRE(index == 3);  // next buffer from its first packet
}

TEST_CASE("A frame may start in the last bits of a packet", "[xma]") {
  std::vector<uint8_t> buf(kPacketBytes);
  SetHeader(buf, 0, kPacketBits - 32 - 5, 0);  // 5 bits left: a split frame header
  uint32_t index = 0;
  REQUIRE(xma::FindNextFrameStart(buf.data(), index, 1) == kPacketBits - 5);
}
