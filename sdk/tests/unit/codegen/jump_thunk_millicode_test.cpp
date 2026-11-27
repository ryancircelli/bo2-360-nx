// Regressions adapted from Tom Clay's rexglue cc04cca27 (setjmp/longjmp thunks) and
// 32195385a (every copy of the millicode helpers), BSD-3-Clause.
#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>
#include <rex/codegen/phases.h>
#include "codegen/decoded_binary.h"

using namespace rex::codegen;
namespace {
constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kNop = 0x60000000, kBlr = 0x4E800020;
struct Fixture {
  std::vector<uint8_t> data;
  TestModule module;
  std::optional<CodegenContext> ctx;
  Fixture(const std::vector<uint32_t>& words, RecompilerConfig cfg = {}) {
    for (auto w : words)
      data.insert(data.end(), {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)});
    module.Load(kBase, data.data(), data.size());
    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(cfg)));
    ctx->initDecoded();
  }
  FunctionNode* discover(uint32_t entry, uint32_t size, const std::unordered_set<uint32_t>& known) {
    auto* node = ctx->graph.addFunction(entry, size, FunctionAuthority::PDATA, true);
    auto r = discoverBlocks(ctx->decoded(), entry, {kBase, kBase + uint32_t(data.size())}, known, size);
    node->discover(std::move(r.blocks), std::move(r.instructions), std::move(r.labels));
    node->setFallThroughs(std::move(r.fallThroughs));
    for (const auto& b : r.unresolvedBranches)
      ctx->graph.addUnresolvedJumpToFunction(entry, b.site, b.target, b.isCall, b.isConditional);
    return node;
  }
  std::string emit(const FunctionNode* node) {
    EmitContext e{ctx->binary(), ctx->Config(), ctx->graph, kBase, nullptr, nullptr};
    return node->emitCpp(e);
  }
};

// caller: bl thunk; blr | thunk words | target: li r3,0; blr
std::string ThunkCaller(bool oneInstruction, bool longjmp) {
  std::vector<uint32_t> w{0x48000009, kBlr};
  if (oneInstruction) w.insert(w.end(), {0x48000008, kNop});  // b +8
  else w.insert(w.end(), {kNop, 0x48000004});                 // nop; b +4
  w.insert(w.end(), {0x38600000, kBlr});
  RecompilerConfig cfg;
  (longjmp ? cfg.longJmpAddress : cfg.setJmpAddress) = kBase + 16;
  Fixture fx(w, cfg);
  const std::unordered_set<uint32_t> known{kBase, kBase + 8, kBase + 16};
  auto* caller = fx.discover(kBase, 8, known);
  fx.discover(kBase + 8, oneInstruction ? 4 : 8, known);
  fx.discover(kBase + 16, 8, known);
  fx.ctx->graph.tryResolveFunction(kBase);
  fx.ctx->graph.tryResolveFunction(kBase + 8);
  fx.ctx->graph.sealAllReady();
  return fx.emit(caller);
}

// __savegprlr_14..31 run: std r14..r31 at -0x98(r1).., then stw r12,-8(r1); blr
std::vector<uint32_t> SaveGprRun() {
  std::vector<uint32_t> w;
  for (uint32_t i = 0; i < 18; ++i) w.push_back(0xF9C1FF68 + i * 0x00200008);
  w.insert(w.end(), {0x9181FFF8, kBlr});
  return w;
}
}  // namespace

TEST_CASE("A call to a thunk that only branches to setjmp emits the setjmp expansion", "[codegen][setjmp]") {
  const auto body = ThunkCaller(true, false);
  CHECK(body.find("ppc_setjmp(") != std::string::npos);
  CHECK(body.find("sub_82000008") == std::string::npos);
}
TEST_CASE("A call to a thunk that only branches to longjmp emits the longjmp call", "[codegen][setjmp]") {
  const auto body = ThunkCaller(true, true);
  CHECK(body.find("ppc_longjmp(") != std::string::npos);
  CHECK(body.find("sub_82000008") == std::string::npos);
}
TEST_CASE("A function that does more than branch to setjmp is called as a function", "[codegen][setjmp]") {
  const auto body = ThunkCaller(false, false);
  CHECK(body.find("ppc_setjmp(") == std::string::npos);
  CHECK(body.find("sub_82000008(ctx, base);") != std::string::npos);
}

TEST_CASE("Every byte-identical copy of a helper run is registered, the first with the plain name",
          "[codegen][millicode]") {
  auto w = SaveGprRun();
  const uint32_t second = kBase + 4 * (uint32_t(w.size()) + 1);
  w.push_back(kNop);
  const auto copy = SaveGprRun();
  w.insert(w.end(), copy.begin(), copy.end());
  w.push_back(kNop);
  auto near = SaveGprRun();
  near.back() = kNop;  // differs in its last word: not a copy
  const uint32_t third = kBase + 4 * uint32_t(w.size());
  w.insert(w.end(), near.begin(), near.end());
  Fixture fx(w);
  (void)phases::Register(*fx.ctx);  // fails later on the missing pdata; helpers come first
  auto& g = fx.ctx->graph;
  REQUIRE(g.getFunction(kBase));
  CHECK(g.getFunction(kBase)->name() == "__savegprlr_14");
  CHECK(g.getFunction(kBase + 17 * 4)->name() == "__savegprlr_31");
  REQUIRE(g.getFunction(second));
  CHECK(g.getFunction(second)->name() == "__savegprlr_14_82000054");
  CHECK(g.getFunction(second + 4)->name() == "__savegprlr_15_82000058");
  CHECK(g.getFunction(second)->size() == 80);
  CHECK(g.getFunction(third) == nullptr);
}
