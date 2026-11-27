// XMA decoder unit tests on synthetic packets (no guest memory, no FFmpeg).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/helpers.h>

using rex::audio::XMA_CONTEXT_DATA;
using rex::audio::XmaContext;
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

TEST_CASE("ConvertFrame rounds to nearest and saturates", "[xma]") {
  constexpr uint32_t n = XmaContext::kSamplesPerFrame;
  std::vector<float> left(n, 0.0f), right(n, 0.0f);
  const float lsb = 1.0f / 32767.0f;
  left[0] = 1.7f * lsb;   // truncation gave 1
  left[1] = -1.7f * lsb;  // truncation gave -1
  left[2] = 0.4f * lsb;
  left[3] = 1.0f;
  left[4] = 2.0f;  // FFmpeg output can exceed [-1, 1]
  right[0] = 2.6f * lsb;

  auto be16 = [](const std::vector<uint8_t>& out, size_t i) {
    return static_cast<int16_t>((out[i * 2] << 8) | out[i * 2 + 1]);
  };

  std::vector<uint8_t> mono(n * 2);
  const uint8_t* planes[2] = {reinterpret_cast<const uint8_t*>(left.data()),
                              reinterpret_cast<const uint8_t*>(right.data())};
  XmaContext::ConvertFrame(planes, false, mono.data());
  CHECK(be16(mono, 0) == 2);
  CHECK(be16(mono, 1) == -2);
  CHECK(be16(mono, 2) == 0);
  CHECK(be16(mono, 3) == 32767);
  CHECK(be16(mono, 4) == 32767);

  std::vector<uint8_t> stereo(n * 4);
  XmaContext::ConvertFrame(planes, true, stereo.data());
  CHECK(be16(stereo, 0) == 2);  // L0
  CHECK(be16(stereo, 1) == 3);  // R0, interleaved
  CHECK(be16(stereo, 2) == -2);
}

TEST_CASE("Loop end below the packet header is no loop end", "[xma]") {
  // Synthetic looped stream: frames start at these bit offsets in packet 0 (header is 32 bits).
  const uint32_t frames[] = {32, 1200, 2600, 4100};
  static const uint32_t kZero[sizeof(XMA_CONTEXT_DATA) / 4] = {};
  auto loop_hits = [&](uint32_t loop_end) {
    XMA_CONTEXT_DATA data(kZero);
    data.loop_count = 2;
    data.loop_end = loop_end;
    int hits = 0;
    for (uint32_t f : frames) {
      data.input_buffer_read_offset = f;
      hits += XmaContext::IsLoopEnd(data);
    }
    return hits;
  };
  CHECK(loop_hits(2600) == 1);  // a real loop end still loops
  CHECK(loop_hits(32) == 1);    // a loop end on the first frame is valid
  CHECK(loop_hits(0) == 0);     // unset loop end: was clamped to 32 and looped on frame 0
  CHECK(loop_hits(31) == 0);    // inside the header

  XMA_CONTEXT_DATA off(kZero);
  off.loop_end = 2600;
  off.input_buffer_read_offset = 2600;
  CHECK_FALSE(XmaContext::IsLoopEnd(off));  // loop_count 0: looping disabled
}
