// Regression coverage adapted from Tom Clay's rexglue 11742bf4 (BSD-3-Clause).
#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>
#include "codegen/decoded_binary.h"

using namespace rex::codegen;
namespace {
constexpr uint32_t kBase = 0x82000000;
struct Fixture {
  std::vector<uint8_t> data;
  TestModule module;
  std::optional<CodegenContext> ctx;
  Fixture(std::initializer_list<uint32_t> words) {
    for (auto w : words)
      data.insert(data.end(), {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)});
    module.Load(kBase, data.data(), data.size());
    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), RecompilerConfig{}));
    ctx->decoded().decode();
  }
  BlockDiscoveryResult scan(std::unordered_set<uint32_t> known, uint32_t size = 0) {
    return discoverBlocks(ctx->decoded(), kBase, {kBase, kBase + uint32_t(data.size())}, known, size);
  }
};
}
TEST_CASE("Linear discovery stops before a known function and emits a tail call", "[codegen][boundary]") {
  Fixture fx{0x38600001, 0x38600002, 0x4E800020};
  auto result = fx.scan({kBase, kBase + 4});
  REQUIRE(result.blocks.size() == 1);
  CHECK(result.blocks[0].size == 4);
  CHECK(result.instructions.size() == 1);
  CHECK(result.fallThroughs.contains(kBase + 4));
  REQUIRE(result.unresolvedBranches.size() == 1);
  CHECK(result.unresolvedBranches[0].target == kBase + 4);
  auto& graph = fx.ctx->graph;
  auto* next = graph.addFunction(kBase + 4, 8, FunctionAuthority::CONFIG, true);
  auto* node = graph.addFunction(kBase, 0, FunctionAuthority::DISCOVERED, true);
  node->discover(std::move(result.blocks), std::move(result.instructions), std::move(result.labels));
  node->setFallThroughs(std::move(result.fallThroughs));
  node->seal();
  EmitContext emit{fx.ctx->binary(), fx.ctx->Config(), graph, kBase, nullptr, nullptr};
  const auto cpp = node->emitCpp(emit);
  CHECK(cpp.find(next->name() + "(ctx, base);") != std::string::npos);
  CHECK(cpp.find("// blr") == std::string::npos);
}
TEST_CASE("Discovery keeps scanning when the next address is not a function", "[codegen][boundary]") {
  Fixture fx{0x38600001, 0x38600002, 0x4E800020};
  auto result = fx.scan({kBase});
  REQUIRE(result.blocks.size() == 1);
  CHECK(result.blocks[0].size == 12);
  CHECK(result.fallThroughs.empty());
}
TEST_CASE("A branch to the entry remains an internal loop", "[codegen][boundary]") {
  Fixture fx{0x38600001, 0x4BFFFFFC, 0x4E800020};
  auto result = fx.scan({kBase, kBase + 8});
  CHECK(result.labels.contains(kBase));
  CHECK(result.fallThroughs.empty());
  CHECK(result.tailCalls.empty());
}
