/**
 * @file        codegen/save_restore_inline_test.cpp
 * @brief       inline_save_restore_helpers emits the helper bodies at the call site
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_node.h>
#include <rex/codegen/test_support.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

std::vector<uint8_t> MakeBinary() {
  const uint32_t words[] = {
      // +0x00 __savegprlr_29
      0xFBA1FFE0, 0xFBC1FFE8, 0xFBE1FFF0, 0x9181FFF8, 0x4E800020,
      // +0x14 __restgprlr_29
      0xEBA1FFE0, 0xEBC1FFE8, 0xEBE1FFF0, 0x8181FFF8, 0x7D8803A6, 0x4E800020,
      // +0x2C caller: mflr r12; bl __savegprlr_29; li r31,5; b __restgprlr_29
      0x7D8802A6, 0x4BFFFFD1, 0x3BE00005, 0x4BFFFFDC,
  };
  std::vector<uint8_t> data;
  for (uint32_t w : words)
    data.insert(data.end(), {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16),
                             static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w)});
  return data;
}

struct Fixture {
  TestModule module;
  std::vector<uint8_t> data = MakeBinary();
  std::optional<CodegenContext> ctx;
  FunctionNode* save = nullptr;
  FunctionNode* rest = nullptr;
  FunctionNode* caller = nullptr;

  explicit Fixture(bool inlineHelpers) {
    module.Load(kBase, data.data(), data.size());
    RecompilerConfig config;
    config.projectName = "savetest";
    config.inlineSaveRestoreHelpers = inlineHelpers;
    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(config)));
    ctx->analysisState().format = "xex";
    ctx->analysisState().loadAddress = kBase;
    ctx->analysisState().entryPoint = kBase;
    ctx->analysisState().imageSize = static_cast<uint32_t>(data.size());
    save = add(0x00, 0x14, "__savegprlr_29");
    rest = add(0x14, 0x18, "__restgprlr_29");
    caller = add(0x2C, 0x10, "sub_8200002C", false);
    ctx->graph.addCallToFunction(kBase + 0x2C, kBase + 0x30, CallTarget::function(save));
    ctx->graph.addTailCallToFunction(kBase + 0x2C, kBase + 0x38, CallTarget::function(rest));
    caller->seal();
  }

  FunctionNode* add(uint32_t off, uint32_t size, const char* name, bool seal = true) {
    auto* fn = ctx->graph.addFunction(kBase + off, size, FunctionAuthority::DISCOVERED, true);
    REQUIRE(fn != nullptr);
    fn->discover({{kBase + off, size}}, {}, {});
    if (seal)
      fn->seal();
    fn->setName(name);
    return fn;
  }

  std::string emit(const FunctionNode* node) {
    EmitContext emitCtx{ctx->binary(), ctx->Config(), ctx->graph, kBase, nullptr, nullptr};
    return node->emitCpp(emitCtx);
  }
};

// The helper body's statements, in order, without comments and wrapper lines.
std::vector<std::string> Statements(const std::string& code) {
  std::vector<std::string> out;
  std::istringstream in(code);
  for (std::string line; std::getline(in, line);) {
    const size_t first = line.find_first_not_of('\t');
    if (first == std::string::npos)
      continue;
    line = line.substr(first);
    if (const size_t mark = line.find("  // nv-helper"); mark != std::string::npos)
      line.resize(mark);
    if (line.starts_with("REX_STORE") || line.starts_with("ctx.r") || line.starts_with("ctx.lr"))
      out.push_back(line);
  }
  return out;
}

}  // namespace

TEST_CASE("Save/restore helpers stay calls by default", "[codegen][savegpr]") {
  Fixture fx(false);
  const std::string code = fx.emit(fx.caller);
  INFO(code);
  CHECK(code.find("__savegprlr_29(ctx, base);") != std::string::npos);
  CHECK(code.find("__restgprlr_29(ctx, base);") != std::string::npos);
}

TEST_CASE("inline_save_restore_helpers emits the helper bodies store for store",
          "[codegen][savegpr]") {
  Fixture fx(true);
  const std::string code = fx.emit(fx.caller);
  INFO(code);
  CHECK(code.find("__savegprlr_29(ctx, base);") == std::string::npos);
  CHECK(code.find("__restgprlr_29(ctx, base);") == std::string::npos);

  // Same guest frame writes and register loads as the helpers, in order, so
  // anything that reads the save area later (longjmp's jmp_buf restore,
  // RtlUnwind walking frames, a context switch) sees exactly what a call wrote.
  std::vector<std::string> expected = {"ctx.r12.u64 = ctx.lr;", "ctx.lr = 0x82000034;"};
  for (const auto& s : Statements(fx.emit(fx.save)))
    expected.push_back(s);
  expected.push_back("ctx.r31.s64 = 5;");
  for (const auto& s : Statements(fx.emit(fx.rest)))
    expected.push_back(s);
  CHECK(Statements(code) == expected);
  CHECK(expected.size() == 12);

  // The tail branch into __restgprlr_29 still returns after the inlined loads.
  const size_t lr = code.find("ctx.lr = ctx.r12.u64;");
  REQUIRE(lr != std::string::npos);
  CHECK(code.find("return;", lr) != std::string::npos);
}
