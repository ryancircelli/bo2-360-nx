/**
 * @file        codegen/conditional_branch_test.cpp
 * @brief       Tests for conditional branch emission to in-function labels
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_node.h>
#include <rex/codegen/test_support.h>
#include <rex/codegen/phases.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

void AppendWord(std::vector<uint8_t>& data, uint32_t word) {
  data.insert(data.end(), {static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
                           static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)});
}

// A function whose blocks are interleaved with an overlapping shadow node:
//
//   +0x00  blr                 } block 1
//   +0x04  cmpwi cr6,r11,1     }
//   +0x08  bne cr6, +0x24  ------+--> T1          } block 2
//   +0x0C  cmpwi cr6,r11,0     }                   }
//   +0x10  bne cr6, +0x1C  --------+--> T2         }
//   +0x14  li r3,1             }                   }
//   +0x18  blr                 }                   }
//   +0x1C  li r3,2   <------------------+  T2      } block 3
//   +0x20  blr                                   }
//   +0x24  li r3,3   <------------+  T1           } block 4
//   +0x28  blr                                   }
//
// The shadow node (base +0x04, block +0x04..+0x14) contains both branch sites
// but neither target, so the graph's by-base lookup resolves the caller to the
// shadow node and classifies both branch targets as Unknown - even though they
// are blocks of the outer function being emitted.
std::vector<uint8_t> MakeOverlappedBinary() {
  std::vector<uint8_t> data;
  AppendWord(data, 0x4E800020);  // +0x00 blr
  AppendWord(data, 0x2F0B0001);  // +0x04 cmpwi cr6,r11,1
  AppendWord(data, 0x409A001C);  // +0x08 bne cr6,+0x1C (to +0x24)
  AppendWord(data, 0x2F0B0000);  // +0x0C cmpwi cr6,r11,0
  AppendWord(data, 0x409A000C);  // +0x10 bne cr6,+0x0C (to +0x1C)
  AppendWord(data, 0x38600001);  // +0x14 li r3,1
  AppendWord(data, 0x4E800020);  // +0x18 blr
  AppendWord(data, 0x38600002);  // +0x1C li r3,2
  AppendWord(data, 0x4E800020);  // +0x20 blr
  AppendWord(data, 0x38600003);  // +0x24 li r3,3
  AppendWord(data, 0x4E800020);  // +0x28 blr
  return data;
}

struct EmitFixture {
  TestModule module;
  std::vector<uint8_t> data;
  std::optional<CodegenContext> ctx;

  explicit EmitFixture(std::vector<uint8_t> bytes) : data(std::move(bytes)) {
    module.Load(kBase, data.data(), data.size());
    RecompilerConfig config;
    config.projectName = "branchtest";
    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(config)));
    ctx->analysisState().format = "xex";
    ctx->analysisState().loadAddress = kBase;
    ctx->analysisState().entryPoint = kBase;
    ctx->analysisState().imageSize = static_cast<uint32_t>(data.size());
  }

  std::string emit(const FunctionNode* node) {
    EmitContext emitCtx{ctx->binary(), ctx->Config(), ctx->graph, kBase, nullptr, nullptr};
    return node->emitCpp(emitCtx);
  }
};

}  // namespace

TEST_CASE("Conditional branch binds to own block shadowed by an overlapping node",
          "[codegen][branch]") {
  EmitFixture fx(MakeOverlappedBinary());

  auto* outer = fx.ctx->graph.addFunction(kBase, 0x2C, FunctionAuthority::DISCOVERED, true);
  REQUIRE(outer != nullptr);
  outer->discover({{kBase + 0x00, 0x04},
                   {kBase + 0x04, 0x14},
                   {kBase + 0x1C, 0x04},
                   {kBase + 0x24, 0x04}},
                  {}, {});
  outer->seal();

  // Overlapping node with a higher base that contains both branch sites but
  // neither target. It shadows the outer function in getFunctionContaining.
  auto* shadow = fx.ctx->graph.addFunction(kBase + 0x04, 0x10, FunctionAuthority::DISCOVERED, true);
  REQUIRE(shadow != nullptr);
  shadow->discover({{kBase + 0x04, 0x10}}, {}, {});
  shadow->seal();

  REQUIRE(fx.ctx->graph.classifyTarget(kBase + 0x24, kBase + 0x08, false) ==
          TargetKind::Unknown);

  const std::string code = fx.emit(outer);

  INFO("emitted code:\n" << code);
  CHECK(code.find("goto loc_82000024") != std::string::npos);
  CHECK(code.find("goto loc_8200001C") != std::string::npos);
  CHECK(code.find("REX_FATAL") == std::string::npos);
}

TEST_CASE("Conditional branch within a function without overlapping nodes", "[codegen][branch]") {
  EmitFixture fx(MakeOverlappedBinary());

  auto* fn = fx.ctx->graph.addFunction(kBase, 0x2C, FunctionAuthority::DISCOVERED, true);
  REQUIRE(fn != nullptr);
  fn->discover({{kBase + 0x00, 0x04},
                {kBase + 0x04, 0x14},
                {kBase + 0x1C, 0x04},
                {kBase + 0x24, 0x04}},
               {}, {});
  fn->seal();

  const std::string code = fx.emit(fn);

  INFO("emitted code:\n" << code);
  CHECK(code.find("goto loc_82000024") != std::string::npos);
  CHECK(code.find("goto loc_8200001C") != std::string::npos);
  CHECK(code.find("REX_FATAL") == std::string::npos);
}

TEST_CASE("Conditional branch to a genuinely unknown target stays fatal", "[codegen][branch]") {
  std::vector<uint8_t> data;
  AppendWord(data, 0x2F0B0001);   // +0x00 cmpwi cr6,r11,1
  AppendWord(data, 0x409A0100);   // +0x04 bne cr6,+0x100 (outside every block)
  AppendWord(data, 0x4E800020);   // +0x08 blr
  EmitFixture fx(std::move(data));

  auto* fn = fx.ctx->graph.addFunction(kBase, 0x0C, FunctionAuthority::DISCOVERED, true);
  REQUIRE(fn != nullptr);
  fn->discover({{kBase, 0x0C}}, {}, {});
  fn->seal();

  const std::string code = fx.emit(fn);

  INFO("emitted code:\n" << code);
  CHECK(code.find("REX_FATAL(\"Unresolved branch from 0x82000004 to 0x82000104\")") !=
        std::string::npos);
}

TEST_CASE("Validation rejects an unresolved branch into another function body", "[codegen][branch]") {
  std::vector<uint8_t> data;
  AppendWord(data, 0x4800000C);  // b base+0x0C, an alternate entry inside the owner
  AppendWord(data, 0x00000000);
  AppendWord(data, 0x38600001);  // owner entry
  AppendWord(data, 0x38600002);  // alternate entry
  AppendWord(data, 0x4E800020);
  EmitFixture fx(std::move(data));
  auto* thunk = fx.ctx->graph.addFunction(kBase, 4, FunctionAuthority::DISCOVERED, true);
  thunk->discover({{kBase, 4}}, {}, {});
  auto* owner = fx.ctx->graph.addFunction(kBase + 8, 12, FunctionAuthority::DISCOVERED, true);
  owner->discover({{kBase + 8, 12}}, {}, {});
  fx.ctx->graph.addUnresolvedJumpToFunction(kBase, kBase, kBase + 12, false, false);

  REQUIRE_FALSE(phases::Validate(*fx.ctx).has_value());
  CHECK(fx.ctx->errors.Count(AnalysisErrors::Category::UnresolvedCall) == 1);
}

TEST_CASE("Validation accepts a resolved tail call to a function entry", "[codegen][branch]") {
  std::vector<uint8_t> data;
  AppendWord(data, 0x48000008);  // b base+8
  AppendWord(data, 0x00000000);
  AppendWord(data, 0x4E800020);
  EmitFixture fx(std::move(data));
  auto* thunk = fx.ctx->graph.addFunction(kBase, 4, FunctionAuthority::DISCOVERED, true);
  thunk->discover({{kBase, 4}}, {}, {});
  auto* target = fx.ctx->graph.addFunction(kBase + 8, 4, FunctionAuthority::DISCOVERED, true);
  target->discover({{kBase + 8, 4}}, {}, {});
  fx.ctx->graph.addUnresolvedJumpToFunction(kBase, kBase, kBase + 8, false, false);

  REQUIRE(phases::Validate(*fx.ctx).has_value());
  CHECK(fx.emit(thunk).find("REX_FATAL") == std::string::npos);
}
