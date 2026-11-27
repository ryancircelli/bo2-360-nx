/**
 * @file        codegen/vector_test.cpp
 * @brief       vmaxuw emission and vmsum/vexptefp overflow behaviour
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_node.h>
#include <rex/codegen/test_support.h>
#include <rex/ppc/intrinsics.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

std::string EmitOne(uint32_t insn) {
  std::vector<uint8_t> data = {uint8_t(insn >> 24), uint8_t(insn >> 16), uint8_t(insn >> 8),
                               uint8_t(insn),       0x4E,                0x80,
                               0x00,                0x20};  // insn; blr
  TestModule module;
  module.Load(kBase, data.data(), data.size());
  RecompilerConfig config;
  config.projectName = "vectest";
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  ctx.analysisState().format = "xex";
  ctx.analysisState().loadAddress = kBase;
  ctx.analysisState().entryPoint = kBase;
  ctx.analysisState().imageSize = uint32_t(data.size());
  auto* fn = ctx.graph.addFunction(kBase, 8, FunctionAuthority::DISCOVERED, true);
  fn->discover({{kBase, 8}}, {}, {});
  fn->seal();
  EmitContext e{ctx.binary(), ctx.Config(), ctx.graph, kBase, nullptr, nullptr};
  return fn->emitCpp(e);
}

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

simde__m128 V(float a, float b, float c, float d) { return simde_mm_set_ps(d, c, b, a); }
float Lane(simde__m128 v, int i) {
  float o[4];
  simde_mm_storeu_ps(o, v);
  return o[i];
}

}  // namespace

TEST_CASE("vmaxuw is emitted as unsigned max", "[codegen][vector]") {
  // vmaxuw v1,v2,v3
  const std::string code = EmitOne(0x10221882);
  INFO(code);
  CHECK(code.find("UNIMPLEMENTED") == std::string::npos);
  CHECK(code.find("max_epu32") != std::string::npos);
}

TEST_CASE("vmsum overflow of finite inputs gives QNaN, inf inputs pass through", "[vector]") {
  using namespace rex::ppc;
  const float big = -3.4028235e38f;
  auto r = VmxDot<3>(V(0, big, big, big), V(0, big, big, big));
  for (int i = 0; i < 4; i++) CHECK(Bits(Lane(r, i)) == 0x7FC00000);
  r = VmxDot<4>(V(big, big, big, big), V(big, big, big, big));
  for (int i = 0; i < 4; i++) CHECK(Bits(Lane(r, i)) == 0x7FC00000);
  // inf from input stays +inf; an unused lane holding inf is ignored by the 3-lane form
  r = VmxDot<3>(V(0, INFINITY, 1, 1), V(0, 1, 1, 1));
  for (int i = 0; i < 4; i++) CHECK(Lane(r, i) == INFINITY);
  r = VmxDot<3>(V(INFINITY, 1, 1, 1), V(1, 1, 1, 1));
  for (int i = 0; i < 4; i++) CHECK(Lane(r, i) == 3.0f);
  // plain value
  r = VmxDot<4>(V(1, 2, 3, 4), V(1, 1, 1, 1));
  CHECK(Lane(r, 0) == 10.0f);
}

TEST_CASE("vexptefp handles out-of-range inputs", "[vector]") {
  using namespace rex::ppc;
  auto r = vexptefp_ps(V(200.0f, 128.0f, INFINITY, 1000.5f));
  for (int i = 0; i < 4; i++) CHECK(Lane(r, i) == INFINITY);
  r = vexptefp_ps(V(-200.0f, -127.0f, -INFINITY, -1000.5f));
  for (int i = 0; i < 4; i++) CHECK(Lane(r, i) == 0.0f);
  r = vexptefp_ps(V(NAN, 0.0f, 1.0f, -1.0f));
  CHECK(std::isnan(Lane(r, 0)));
  CHECK(std::fabs(Lane(r, 1) - 1.0f) < 1e-3f);
  CHECK(std::fabs(Lane(r, 2) - 2.0f) < 2e-3f);
  CHECK(std::fabs(Lane(r, 3) - 0.5f) < 1e-3f);
}
