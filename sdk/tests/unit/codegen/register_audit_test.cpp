/**
 * @file        codegen/register_audit_test.cpp
 * @brief       The generator's read-before-write audit (codegen/register_audit.cpp)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_node.h>
#include <rex/codegen/test_support.h>

#include "codegen/register_audit.h"

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBeqCr6Skip = 0x419A0008;   // beq cr6,+8
constexpr uint32_t kCmpwiCr6R3 = 0x2F030000;   // cmpwi cr6,r3,0
constexpr uint32_t kStdR31Save = 0xFBE1FFF8;   // std r31,-8(r1)
constexpr uint32_t kMfcrR12 = 0x7D800026;      // mfcr r12
constexpr uint32_t kAddzeR3 = 0x7C630194;      // addze r3,r3
constexpr uint32_t kMrR3R31 = 0x7FE3FB78;      // mr r3,r31
constexpr uint32_t kLiR31 = 0x3BE00001;        // li r31,1
constexpr uint32_t kMtctrR3 = 0x7C6903A6;      // mtctr r3
constexpr uint32_t kMfctrR3 = 0x7C6902A6;      // mfctr r3

std::vector<std::string> EarlyRegs(std::initializer_list<uint32_t> words) {
  std::vector<uint8_t> data;
  for (uint32_t w : words)
    data.insert(data.end(), {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16),
                             static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w)});
  TestModule module;
  module.Load(kBase, data.data(), data.size());
  RecompilerConfig config;
  config.projectName = "audittest";
  config.crRegistersAsLocalVariables = true;
  config.ctrAsLocalVariable = true;
  config.xerAsLocalVariable = true;
  config.nonVolatileRegistersAsLocalVariables = true;
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  ctx.analysisState().format = "xex";
  ctx.analysisState().loadAddress = kBase;
  ctx.analysisState().entryPoint = kBase;
  ctx.analysisState().imageSize = static_cast<uint32_t>(data.size());
  auto* fn = ctx.graph.addFunction(kBase, static_cast<uint32_t>(data.size()),
                                   FunctionAuthority::DISCOVERED, true);
  REQUIRE(fn != nullptr);
  fn->discover({{kBase, static_cast<uint32_t>(data.size())}}, {}, {});
  fn->seal();
  EmitContext emitCtx{ctx.binary(), ctx.Config(), ctx.graph, kBase, nullptr, nullptr};
  std::vector<std::string> regs;
  for (const auto& e : FindEarlyReads(fn->emitCpp(emitCtx)))
    regs.push_back(e.reg);
  return regs;
}

using Regs = std::vector<std::string>;

}  // namespace

TEST_CASE("A branch on a CR field nothing set is an early read", "[codegen][audit]") {
  CHECK(EarlyRegs({kBeqCr6Skip, kBlr, kBlr}) == Regs{"cr6"});
}

TEST_CASE("A branch after its compare is not", "[codegen][audit]") {
  CHECK(EarlyRegs({kCmpwiCr6R3, kBeqCr6Skip, kBlr, kBlr}).empty());
}

TEST_CASE("Prologue saves are not early reads", "[codegen][audit]") {
  CHECK(EarlyRegs({kStdR31Save, kBlr}).empty());
  CHECK(EarlyRegs({kMfcrR12, kBlr}).empty());
}

TEST_CASE("Carry, CTR and non-volatile reads are", "[codegen][audit]") {
  CHECK(EarlyRegs({kAddzeR3, kBlr}) == Regs{"xer"});
  CHECK(EarlyRegs({kMfctrR3, kBlr}) == Regs{"ctr"});
  CHECK(EarlyRegs({kMrR3R31, kBlr}) == Regs{"r31"});
}

TEST_CASE("Written first on every path, read later", "[codegen][audit]") {
  CHECK(EarlyRegs({kLiR31, kMrR3R31, kBlr}).empty());
  CHECK(EarlyRegs({kMtctrR3, kMfctrR3, kBlr}).empty());
}

TEST_CASE("Written on one path only is an early read", "[codegen][audit]") {
  // cmpwi cr6; beq cr6,+8 (skips the li); li r31,1; mr r3,r31; blr
  CHECK(EarlyRegs({kCmpwiCr6R3, kBeqCr6Skip, kLiR31, kMrR3R31, kBlr}) == Regs{"r31"});
}
