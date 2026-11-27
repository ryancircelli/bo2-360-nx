/**
 * @file        codegen/stack_access_test.cpp
 * @brief       Accesses through r1 use the direct stack macros
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_node.h>
#include <rex/codegen/test_support.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kLwzR3FromR1 = 0x80610008;
constexpr uint32_t kStwR3ToR1 = 0x90610008;
constexpr uint32_t kStwuR1 = 0x9421FFC0;
constexpr uint32_t kLfdF1FromR1 = 0xC8210010;
constexpr uint32_t kLwzxR3FromR1 = 0x7C61202E;
constexpr uint32_t kLwzR3FromR4 = 0x80640008;

std::string Generate(std::initializer_list<uint32_t> words) {
  std::vector<uint8_t> data;
  for (uint32_t w : words) {
    data.insert(data.end(), {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16),
                             static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w)});
  }
  TestModule module;
  module.Load(kBase, data.data(), data.size());
  RecompilerConfig config;
  config.projectName = "stacktest";
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
  return fn->emitCpp(emitCtx);
}

bool Has(const std::string& s, const char* t) {
  return s.find(t) != std::string::npos;
}

}  // namespace

TEST_CASE("A load through r1 uses the stack macro", "[codegen][stack]") {
  const auto code = Generate({kLwzR3FromR1, kBlr});
  CHECK(Has(code, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(code, "(REX_LOAD_U32"));
}

TEST_CASE("A write through r1 uses the stack macro", "[codegen][stack]") {
  CHECK(Has(Generate({kStwR3ToR1, kBlr}), "REX_STACK_STORE_U32"));
}

TEST_CASE("An update write through r1 uses the stack macro", "[codegen][stack]") {
  CHECK(Has(Generate({kStwuR1, kBlr}), "REX_STACK_STORE_U32"));
}

TEST_CASE("A float load through r1 uses the stack macro", "[codegen][stack]") {
  CHECK(Has(Generate({kLfdF1FromR1, kBlr}), "REX_STACK_LOAD_U64"));
}

TEST_CASE("An indexed load with r1 as the base uses the stack macro", "[codegen][stack]") {
  CHECK(Has(Generate({kLwzxR3FromR1, kBlr}), "REX_STACK_LOAD_U32"));
}

TEST_CASE("A load through another base keeps the offset form", "[codegen][stack]") {
  const auto code = Generate({kLwzR3FromR4, kBlr});
  CHECK(Has(code, "REX_LOAD_U32"));
  CHECK_FALSE(Has(code, "REX_STACK_LOAD"));
}

namespace {

constexpr uint32_t DForm(uint32_t op, uint32_t rt, uint32_t ra) {
  return op << 26 | rt << 21 | ra << 16 | 8;
}
constexpr uint32_t XForm(uint32_t xo, uint32_t rt, uint32_t ra) {
  return 31u << 26 | rt << 21 | ra << 16 | 5u << 11 | xo << 1;
}

// r1-based emission with the stack macros renamed back and r1 renamed to r4 must
// equal the r4-based emission: only the macro changes, update forms still write rA.
void CheckOnlyMacroDiffers(uint32_t r1Word, uint32_t r4Word) {
  const auto stack = Generate({r1Word, kBlr});
  INFO(stack);
  CHECK(Has(stack, "REX_STACK_"));
  const auto plain = Generate({r4Word, kBlr});
  CHECK_FALSE(Has(plain, "REX_STACK_"));
  auto normal = std::regex_replace(stack, std::regex("REX_STACK_"), "REX_");
  normal = std::regex_replace(normal, std::regex("\\br1\\b"), "r4");
  CHECK(normal == plain);
}

}  // namespace

TEST_CASE("Previously uncovered D-form ops through r1 use the stack macro", "[codegen][stack]") {
  // lhau, lfsu, lfdu, stfsu, stfdu, stmw r30
  for (uint32_t op : {43u, 49u, 51u, 53u, 55u}) {
    CheckOnlyMacroDiffers(DForm(op, 3, 1), DForm(op, 3, 4));
  }
  CheckOnlyMacroDiffers(DForm(47, 30, 1), DForm(47, 30, 4));
}

TEST_CASE("Previously uncovered X-form ops with r1 as rA use the stack macro", "[codegen][stack]") {
  // lwaux, lhaux, lwbrx, lfsux, lfdux, stfsux, stfdux, lhbrx
  for (uint32_t xo : {373u, 375u, 534u, 567u, 631u, 695u, 759u, 790u}) {
    CheckOnlyMacroDiffers(XForm(xo, 3, 1), XForm(xo, 3, 4));
  }
}
