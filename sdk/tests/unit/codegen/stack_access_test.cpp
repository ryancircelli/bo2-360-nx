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
