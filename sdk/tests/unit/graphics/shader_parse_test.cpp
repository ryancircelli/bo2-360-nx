// ALU microcode parsing checks for fixes ported from Xenia Canary
// (BSD-3-Clause).

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/shader/shader.h>

using namespace rex::graphics;

namespace {

// Vector op on r0, r0, r0 and scalar adds on r0 (identity swizzles).
ParsedAluInstruction ParseWithVectorOp(ucode::AluVectorOpcode vector_opcode) {
  uint32_t words[3] = {
      (0xFu << 16) | (1u << 20) |  // vector and scalar write masks
          (uint32_t(ucode::AluScalarOpcode::kAdds) << 26),
      0,  // identity swizzles
      (uint32_t(vector_opcode) << 24) | (7u << 29),  // all sources are temps
  };
  ucode::AluInstruction op;
  std::memcpy(&op, words, sizeof(op));
  ParsedAluInstruction instr;
  ParseAluInstruction(op, xenos::ShaderType::kVertex, instr);
  return instr;
}

}  // namespace

// 92ada8ebc: scalar `b` is Z, not X, when the vector op also reads source 3.
TEST_CASE("Two-component scalar b follows the vector operand count", "[shader]") {
  ParsedAluInstruction mad = ParseWithVectorOp(ucode::AluVectorOpcode::kMad);
  REQUIRE(mad.scalar_operand_count == 1);
  CHECK(mad.scalar_operands[0].components[0] == SwizzleSource::kW);
  CHECK(mad.scalar_operands[0].components[1] == SwizzleSource::kZ);

  ParsedAluInstruction add = ParseWithVectorOp(ucode::AluVectorOpcode::kAdd);
  REQUIRE(add.scalar_operand_count == 1);
  CHECK(add.scalar_operands[0].components[0] == SwizzleSource::kW);
  CHECK(add.scalar_operands[0].components[1] == SwizzleSource::kX);
}
