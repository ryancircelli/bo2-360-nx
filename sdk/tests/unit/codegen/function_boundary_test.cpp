// Regression coverage adapted from Tom Clay's rexglue 11742bf4 (BSD-3-Clause).
#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>
#include <rex/codegen/phases.h>
#include "codegen/decoded_binary.h"

using namespace rex::codegen;
namespace {
constexpr uint32_t kBase = 0x82000000;
struct Fixture {
  std::vector<uint8_t> data;
  TestModule module;
  std::optional<CodegenContext> ctx;
  Fixture(std::initializer_list<uint32_t> words) : Fixture(std::vector<uint32_t>(words)) {}
  Fixture(const std::vector<uint32_t>& words) {
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
  graph.addTailCallToFunction(kBase, kBase + 4, CallTarget::function(next));
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
  for (const auto& branch : result.unresolvedBranches)
    fx.ctx->graph.addUnresolvedJumpToFunction(entry, branch.site, branch.target, branch.isCall,
                                             branch.isConditional);
  fx.ctx->graph.tryResolveFunction(entry);
  if (node->canSeal()) node->seal();
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
  graph.tryResolveFunction(kBase);
  graph.tryResolveFunction(kBase + 8);
  graph.sealAllReady();
  CHECK(graph.isNoReturn(kBase));
  CHECK(graph.isNoReturn(kBase + 8));
  CHECK_FALSE(graph.isNoReturn(kBase + 4));
  CHECK_FALSE(graph.isNoReturn(kBase + 0x100));
  CHECK(Emit(fx, a).find("sub_82000008(ctx, base);") != std::string::npos);
  CHECK(Emit(fx, a).find("REX_FATAL") == std::string::npos);
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

TEST_CASE("Return cuts preserve guarded and backward-connected code", "[codegen][boundary]") {
  SECTION("unguarded return cuts an overgrown gap function") {
    Fixture fx{0x4E800020, 0x38600001, 0x4E800020};
    BlockDiscoveryResult result;
    for (uint32_t a = kBase; a < kBase + 12; a += 4)
      result.instructions.push_back(fx.ctx->decoded().get(a));
    CHECK(findReturnCut(result, kBase) == kBase + 4);
  }
  SECTION("conditional branch guards later code") {
    Fixture fx{0x41820008, 0x4E800020, 0x4E800020};
    auto result = fx.scan({kBase});
    CHECK_FALSE(findReturnCut(result, kBase));
  }
  SECTION("later branch connects back into the prefix") {
    Fixture fx{0x60000000, 0x4E800020, 0x4BFFFFFC};
    BlockDiscoveryResult result;
    for (uint32_t a = kBase; a < kBase + 12; a += 4)
      result.instructions.push_back(fx.ctx->decoded().get(a));
    CHECK_FALSE(findReturnCut(result, kBase));
  }
}
TEST_CASE("Out-of-line switch cases are scanned past their first instruction", "[codegen][boundary]") {
  Fixture fx{0x4E800420, 0x60000000, 0x38600001, 0x38630001, 0x4E800020};
  JumpTable table;
  table.bctrAddress = kBase;
  table.targets = {kBase + 8};
  const std::unordered_map<uint32_t, JumpTable> tables{{kBase, table}};
  const auto result = discoverBlocks(fx.ctx->decoded(), kBase, {kBase, kBase + 20},
                                     {kBase}, 4, &tables);
  REQUIRE(result.blocks.size() == 2);
  CHECK(result.blocks.back().base == kBase + 8);
  CHECK(result.blocks.back().size == 12);
}

TEST_CASE("Gap discovery registers uncovered code after a nonreturning branch", "[codegen][gapfill]") {
  // The old whole-region splitter never split at b ., so it missed +4 entirely.
  Fixture fx{0x48000000, 0x38600001, 0x4E800020};
  Discover(fx, kBase, 4, {kBase});
  fx.ctx->scan.codeRegions = {{kBase, kBase + 12}};
  REQUIRE(phases::GapFill(*fx.ctx, nullptr));
  const auto* gap = fx.ctx->graph.getFunction(kBase + 4);
  REQUIRE(gap != nullptr);
  CHECK(gap->authority() == FunctionAuthority::GAP_FILL);
  CHECK(gap->end() == kBase + 12);
}

TEST_CASE("Declared-end fall-through calls the next function without a recorded edge", "[codegen][boundary]") {
  Fixture fx{0x38600001, 0x38600002, 0x4E800020};
  auto* node = Discover(fx, kBase, 4, {kBase});
  Discover(fx, kBase + 4, 8, {kBase + 4});
  const auto cpp = Emit(fx, node);
  CHECK(cpp.find("sub_82000004(ctx, base);") != std::string::npos);
  CHECK(cpp.find("REX_FATAL") == std::string::npos);
}

TEST_CASE("Gap discovery accepts instructions supported by the emitter's decoder", "[codegen][gapfill]") {
  SECTION("VMX128 from NFSMW 8269DA60") {
    Fixture fx{0x48000000, 0x81630000, 0x1000038C, 0x18C407F0, 0x4E800020};
    Discover(fx, kBase, 4, {kBase});
    fx.ctx->scan.codeRegions = {{kBase, kBase + 20}};
    REQUIRE(phases::GapFill(*fx.ctx, nullptr));
    const auto* gap = fx.ctx->graph.getFunction(kBase + 4);
    REQUIRE(gap != nullptr);
    CHECK(gap->end() == kBase + 20);
  }
  SECTION("integer carry instruction from NFSMW 821DF948") {
    Fixture fx{0x48000000, 0x3964FFFF, 0x7D6A0194, 0x4E800020};
    Discover(fx, kBase, 4, {kBase});
    fx.ctx->scan.codeRegions = {{kBase, kBase + 16}};
    REQUIRE(phases::GapFill(*fx.ctx, nullptr));
    REQUIRE(fx.ctx->graph.getFunction(kBase + 4) != nullptr);
  }
}

TEST_CASE("Gap discovery skips a stray literal word that falls into known code", "[codegen][gapfill]") {
  // Seen as 96 spurious one-word 'lbz' functions in FH1 speech; the word is data before a function.
  Fixture fx{0x48000000, 0x88680000, 0x4E800020};
  Discover(fx, kBase, 4, {kBase});
  Discover(fx, kBase + 8, 4, {kBase + 8});
  fx.ctx->scan.codeRegions = {{kBase, kBase + 12}};
  REQUIRE(phases::GapFill(*fx.ctx, nullptr));
  CHECK(fx.ctx->graph.getFunction(kBase + 4) == nullptr);
}

TEST_CASE("A call that ends a function does not fall into the next function", "[codegen][boundary][noreturn]") {
  // A: bl C at its declared end; N follows A; C returns.
  Fixture fx{0x48000009, 0x4E800020, 0x4E800020};
  const std::unordered_set<uint32_t> known{kBase, kBase + 4, kBase + 8};
  Discover(fx, kBase + 8, 4, known);
  Discover(fx, kBase + 4, 4, known);
  auto* a = Discover(fx, kBase, 4, known);
  auto& graph = fx.ctx->graph;
  graph.tryResolveFunction(kBase);
  graph.sealAllReady();
  CHECK(a->fallThroughs().empty());
  const auto cpp = Emit(fx, a);
  CHECK(cpp.find("sub_82000008(ctx, base);") != std::string::npos);
  CHECK(cpp.find("sub_82000004(ctx, base);") == std::string::npos);
  CHECK(graph.isNoReturn(kBase));
  CHECK_FALSE(graph.isNoReturn(kBase + 8));
  // Without a declared end there is no proof the function stops at the call.
  CHECK(fx.scan(known).fallThroughs.contains(kBase + 4));
}

TEST_CASE("A call followed by an entry inside the declared range still falls through", "[codegen][boundary][noreturn]") {
  // A spans 12 bytes: bl C, then M (a known entry inside A's range): li r3,2; blr. C returns.
  Fixture fx{0x4800000D, 0x38600002, 0x4E800020, 0x4E800020};
  const std::unordered_set<uint32_t> known{kBase, kBase + 4, kBase + 12};
  Discover(fx, kBase + 12, 4, known);
  Discover(fx, kBase + 4, 8, known);
  auto* a = Discover(fx, kBase, 12, known);
  auto& graph = fx.ctx->graph;
  graph.tryResolveFunction(kBase);
  graph.sealAllReady();
  CHECK(a->fallThroughs().contains(kBase + 4));
  CHECK(Emit(fx, a).find("sub_82000004(ctx, base);") != std::string::npos);
  CHECK_FALSE(graph.isNoReturn(kBase));
}

TEST_CASE("setjmp and longjmp are found by the XDK runtime's shape", "[codegen][boundary]") {
  for (uint32_t body : {0xB8u, 0x2B8u}) {  // without and with the VMX register block
    std::vector<uint32_t> words(0x400, 0x60000000);
    const uint32_t set = 4, jump = set + body / 4 + 8;
    words[set + 5] = 0x7C0802A6;  // mflr r0
    words[set + 6] = 0x7C800026;  // mfcr r4
    words[set + 7] = 0xD9C30000;  // stfd f14,0(r3)
    words[set + body / 4] = 0xF8230090;  // std r1,144(r3)
    words[jump] = 0x7C0802A6;      // mflr r0
    words[jump + 1] = 0x9421FFB0;  // stwu r1,-80(r1)
    words[jump + 3] = 0x7C862378;  // mr r6,r4
    words[jump + (body + 0x24) / 4] = 0xE8270090;  // ld r1,144(r7)
    words[0x3F0] = 0xF8230090;  // the same store with no setjmp around it

    Fixture fx{words};
    auto& config = fx.ctx->Config();
    phases::detectSetjmpLongjmp(fx.ctx->binary(), config);
    CHECK(config.setJmpAddress == kBase + set * 4);
    CHECK(config.longJmpAddress == kBase + jump * 4);

    RecompilerConfig configured;
    configured.setJmpAddress = 0x82001000;
    phases::detectSetjmpLongjmp(fx.ctx->binary(), configured);
    CHECK(configured.setJmpAddress == 0x82001000);
    CHECK(configured.longJmpAddress == kBase + jump * 4);
  }
}
