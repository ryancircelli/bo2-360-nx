/**
 * @file        codegen/jump_table_bounds_test.cpp
 * @brief       Jump table bounds when the switch index is a copy of the compared register
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_region.h>
#include "codegen/decoded_binary.h"
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

void AppendWord(std::vector<uint8_t>& data, uint32_t word) {
  data.insert(data.end(), {static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
                           static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)});
}

// MSVC byte-offset switch on a copied index (Forza Horizon 826B5D18 shape):
//   +0x00 mr r30,r4  /  cmplwi cr6,r4,2   (order depends on copyFirst)
//   +0x08 bgt cr6,+0x40
//   +0x0C lis r12,0x8200 ; addi r12,r12,0x48   table
//   +0x14 lbzx r0,r12,r30 ; rlwinm r0,r0,2,0,29
//   +0x1C lis r12,0x8200 ; addi r12,r12,0x30   base
//   +0x24 add r12,r12,r0 ; mtctr r12 ; bctr
//   +0x30 three cases ; +0x48 table bytes 0,2,4 then more valid-looking offsets
std::vector<uint8_t> MakeSwitch(bool copyFirst) {
  std::vector<uint8_t> d;
  AppendWord(d, copyFirst ? 0x7C9E2378 : 0x2B040002);  // mr r30,r4 | cmplwi cr6,r4,2
  AppendWord(d, copyFirst ? 0x2B040002 : 0x7C9E2378);
  AppendWord(d, 0x41990038);  // bgt cr6,+0x38 (to +0x40)
  AppendWord(d, 0x3D808200);  // lis r12,0x8200
  AppendWord(d, 0x398C0048);  // addi r12,r12,0x48
  AppendWord(d, 0x7C0CF0AE);  // lbzx r0,r12,r30
  AppendWord(d, 0x5400103A);  // rlwinm r0,r0,2,0,29
  AppendWord(d, 0x3D808200);  // lis r12,0x8200
  AppendWord(d, 0x398C0030);  // addi r12,r12,0x30
  AppendWord(d, 0x7D8C0214);  // add r12,r12,r0
  AppendWord(d, 0x7D8903A6);  // mtctr r12
  AppendWord(d, 0x4E800420);  // bctr
  AppendWord(d, 0x38600001);  // +0x30 li r3,1
  AppendWord(d, 0x4E800020);  //       blr
  AppendWord(d, 0x38600002);  // +0x38 li r3,2
  AppendWord(d, 0x4E800020);  //       blr
  AppendWord(d, 0x38600003);  // +0x40 li r3,3
  AppendWord(d, 0x4E800020);  //       blr
  AppendWord(d, 0x00020406);  // +0x48 table: 0,2,4 then 6 (past the bound)
  AppendWord(d, 0x01030500);  //       offsets that still land inside the region
  return d;
}

std::vector<uint32_t> Targets(bool copyFirst) {
  auto data = MakeSwitch(copyFirst);
  TestModule module;
  module.Load(kBase, data.data(), data.size());
  DecodedBinary decoded(BinaryView::fromModule(module));
  decoded.decode();
  CodeRegion region{kBase, kBase + static_cast<uint32_t>(data.size())};
  auto jt = detectJumpTable(decoded, kBase + 0x2C, region, kBase, kBase + 0x48);
  REQUIRE(jt.has_value());
  return jt->targets;
}

}  // namespace

TEST_CASE("Jump table bound found when the index is copied before the compare",
          "[codegen][jumptable]") {
  const std::vector<uint32_t> want{kBase + 0x30, kBase + 0x38, kBase + 0x40};
  CHECK(Targets(true) == want);
}

TEST_CASE("Jump table bound found when the index is copied after the compare",
          "[codegen][jumptable]") {
  const std::vector<uint32_t> want{kBase + 0x30, kBase + 0x38, kBase + 0x40};
  CHECK(Targets(false) == want);
}
