// Signature matching for XDK library routines (codegen/xdk_signatures.cpp): a routine linked at another
// address (other branch displacements, lis immediates and lis-relative offsets) still matches; any other
// instruction change does not.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "codegen/xdk_signatures.h"

namespace {
std::vector<uint8_t> Code(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> out;
  for (uint32_t w : words)
    for (int s = 24; s >= 0; s -= 8)
      out.push_back(uint8_t(w >> s));
  return out;
}
// lis r11,hi ; lwz r3,lo(r11) ; bl disp ; addi r4,r4,1 ; cmpwi r4,0 ; bne disp ; blr
std::vector<uint32_t> Routine(uint16_t hi, uint16_t lo, uint32_t bl, uint16_t bne) {
  return {0x3D600000u | hi, 0x806B0000u | lo, 0x48000001u | (bl & 0x03FFFFFC), 0x38840001u,
          0x2C040000u,      0x40820000u | (bne & 0xFFFC), 0x4E800020u};
}
}  // namespace

TEST_CASE("xdk signature matches the routine at any link address", "[codegen][xdk]") {
  auto ref = Code(Routine(0x8200, 0x1234, 0x1000, 0x20));
  auto sig = rex::codegen::MakeXdkSignature("test", ref);
  REQUIRE(sig.words == 7);

  // padding, the relinked routine, padding, an altered copy (addi r4,r4,2)
  std::vector<uint32_t> image = {0, 0};
  auto moved = Routine(0x8310, 0x5678, 0x2F0000, 0xFFF0);
  image.insert(image.end(), moved.begin(), moved.end());
  image.push_back(0);
  auto altered = Routine(0x8200, 0x1234, 0x1000, 0x20);
  altered[3] = 0x38840002u;
  image.insert(image.end(), altered.begin(), altered.end());
  auto bytes = Code(image);

  auto hits = rex::codegen::FindXdkSignature(bytes.data(), uint32_t(bytes.size()), 0x82000000, sig);
  REQUIRE(hits == std::vector<uint32_t>{0x82000008});
}

TEST_CASE("xdk signature keeps offsets from registers lis did not load", "[codegen][xdk]") {
  // lwz r3,8(r4) vs lwz r3,12(r4): a real field offset, so a different routine
  auto a = rex::codegen::MakeXdkSignature("a", Code({0x80640008u, 0, 0, 0x4E800020u}));
  auto b = rex::codegen::MakeXdkSignature("b", Code({0x8064000Cu, 0, 0, 0x4E800020u}));
  REQUIRE(a.hash != b.hash);
}
