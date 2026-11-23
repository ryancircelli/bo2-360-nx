/**
 * @file        codegen/jump_table_test.cpp
 * @brief       Byte-offset switch table whose index computation is an addi that precedes the table address
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_region.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>

#include "codegen/decoded_binary.h"

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

void AppendWord(std::vector<uint8_t>& data, uint32_t word) {
  data.insert(data.end(), {static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
                           static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)});
}

// The MW2012 pattern: the table address has a zero low half (lis r12,0x8200; addi r12,r12,0) and the index is
// computed by an addi earlier in program order (addi r11,r27,-1). The backward scan used to fold that addi into the
// table address (0x81FFFFFF), shifting every entry by one.
std::vector<uint8_t> MakeBinary() {
  std::vector<uint8_t> d = {0x0C, 0x00, 0x08, 0x04};  // +0x00 byte-offset table (4 entries)
  d.resize(0x10, 0);
  AppendWord(d, 0x397BFFFF);  // +0x10 addi r11,r27,-1
  AppendWord(d, 0x2B0B0003);  // +0x14 cmplwi cr6,r11,3
  AppendWord(d, 0x3D808200);  // +0x18 lis r12,0x8200
  AppendWord(d, 0x398C0000);  // +0x1C addi r12,r12,0     table = 0x82000000
  AppendWord(d, 0x7C0C58AE);  // +0x20 lbzx r0,r12,r11
  AppendWord(d, 0x3D808200);  // +0x24 lis r12,0x8200
  AppendWord(d, 0x398C0040);  // +0x28 addi r12,r12,0x40  base = 0x82000040
  AppendWord(d, 0x7D8C0214);  // +0x2C add r12,r12,r0
  AppendWord(d, 0x7D8903A6);  // +0x30 mtctr r12
  AppendWord(d, 0x4E800420);  // +0x34 bctr
  d.resize(0x40, 0);
  for (uint32_t w : {0x38600001u, 0x4E800020u, 0x38600002u, 0x4E800020u})  // +0x40.. case bodies
    AppendWord(d, w);
  return d;
}

}  // namespace

TEST_CASE("Byte-offset jump table ignores an earlier index addi", "[codegen][jumptable]") {
  auto data = MakeBinary();
  TestModule module;
  module.Load(kBase, data.data(), data.size());
  BinaryView view = BinaryView::fromModule(module);
  DecodedBinary decoded(view);
  decoded.decode();

  auto jt = detectJumpTable(decoded, kBase + 0x34, CodeRegion{kBase + 0x10, kBase + 0x50}, kBase + 0x10,
                            kBase + 0x50);
  REQUIRE(jt.has_value());
  CHECK(jt->tableAddress == kBase);
  REQUIRE(jt->targets.size() == 4);
  CHECK(jt->targets[0] == kBase + 0x4C);
  CHECK(jt->targets[1] == kBase + 0x40);
  CHECK(jt->targets[2] == kBase + 0x48);
  CHECK(jt->targets[3] == kBase + 0x44);
}
