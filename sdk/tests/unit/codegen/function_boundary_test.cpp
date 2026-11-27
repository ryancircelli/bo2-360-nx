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
    ctx->initDecoded();
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

// No-return regressions adapted from Tom Clay's rexglue e9c1a0f6 (BSD-3-Clause).
namespace {
FunctionNode* Discover(Fixture& fx, uint32_t entry, uint32_t size,
                       const std::unordered_set<uint32_t>& known) {
  auto* node = fx.ctx->graph.addFunction(entry, size, FunctionAuthority::PDATA, true);
  auto result = discoverBlocks(fx.ctx->decoded(), entry,
      {kBase, kBase + uint32_t(fx.data.size())}, known, size);
  node->discover(std::move(result.blocks), std::move(result.instructions), std::move(result.labels));
  node->setFallThroughs(std::move(result.fallThroughs));
  node->seal();
  return node;
}
std::string Emit(Fixture& fx, const FunctionNode* node) {
  EmitContext emit{fx.ctx->binary(), fx.ctx->Config(), fx.ctx->graph, kBase, nullptr, nullptr};
  return node->emitCpp(emit);
}
}
TEST_CASE("Calls to no-return functions suppress synthetic fall-through", "[codegen][noreturn]") {
  // A calls B, B calls a declared no-return import, with a follower after each.
  Fixture fx{0x48000009, 0x4E800020, 0x48000009, 0x4E800020, 0x4E800020};
  auto& graph = fx.ctx->graph;
  graph.declareNoReturn(kBase + 16);
  const std::unordered_set<uint32_t> known{kBase, kBase + 4, kBase + 8, kBase + 12, kBase + 16};
  auto* a = Discover(fx, kBase, 4, known);
  auto* b = Discover(fx, kBase + 8, 4, known);
  Discover(fx, kBase + 4, 4, known);
  Discover(fx, kBase + 12, 4, known);
  auto* imp = graph.addImportFunction(kBase + 16, "__imp__ExTerminateThread");
  imp->discoverAsImport();
  imp->seal();
  CHECK(graph.isNoReturn(kBase));
  CHECK(graph.isNoReturn(kBase + 8));
  CHECK_FALSE(graph.isNoReturn(kBase + 4));
  CHECK_FALSE(graph.isNoReturn(kBase + 0x100));
  CHECK(Emit(fx, a).find("sub_82000004(ctx, base);") == std::string::npos);
  CHECK(Emit(fx, b).find("sub_8200000C(ctx, base);") == std::string::npos);
}
TEST_CASE("Unknown calls and a conditional return conservatively may return", "[codegen][noreturn]") {
  SECTION("unknown call") {
    Fixture fx{0x48000011};
    Discover(fx, kBase, 4, {kBase});
    CHECK_FALSE(fx.ctx->graph.isNoReturn(kBase));
  }
  SECTION("conditional return before no-return call") {
    Fixture fx{0x4D820020, 0x4800000D};
    Discover(fx, kBase, 8, {kBase});
    fx.ctx->graph.declareNoReturn(kBase + 16);
    CHECK_FALSE(fx.ctx->graph.isNoReturn(kBase));
  }
}
TEST_CASE("Loops and unconditional traps never return", "[codegen][noreturn]") {
  SECTION("self loop") {
    Fixture fx{0x48000000};
    Discover(fx, kBase, 4, {kBase});
    CHECK(fx.ctx->graph.isNoReturn(kBase));
  }
  SECTION("trap") {
    Fixture fx{0x0FE00000};
    Discover(fx, kBase, 4, {kBase});
    CHECK(fx.ctx->graph.isNoReturn(kBase));
  }
}
