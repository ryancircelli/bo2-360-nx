#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "rex/graphics/metal/metal_msl_translator.h"
#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/string/buffer.h"

using namespace rex::graphics;

namespace {

struct CfWords {
  uint32_t word0;
  uint32_t word1;
};

void AppendCfPair(std::vector<uint32_t>& ucode, CfWords a, CfWords b) {
  ucode.push_back(a.word0);
  ucode.push_back((a.word1 & 0xFFFFu) | (b.word0 << 16));
  ucode.push_back((b.word0 >> 16) | (b.word1 << 16));
}

CfWords Exec(uint32_t address, bool end) {
  return {address | (1u << 12) | (1u << 16),
          (1u << 11) | ((end ? 2u : 1u) << 12)};
}

CfWords PredicatedJump(uint32_t target) {
  return {target | (1u << 14), (1u << 10) | (1u << 11) | (11u << 12)};
}

void AppendCubeFetch(std::vector<uint32_t>& ucode, bool computed_lod,
                     int lod_bias = 0) {
  constexpr uint32_t kIdentitySwizzle = 0x688;
  ucode.push_back(1u);
  ucode.push_back(kIdentitySwizzle | (1u << 12) | (1u << 14) | (1u << 16) |
                  (computed_lod ? (1u << 28) : 0u));
  ucode.push_back((uint32_t(lod_bias) & 0x7Fu) << 2 | (3u << 14));
}

std::string Translate(const std::vector<uint32_t>& ucode) {
  DxbcShader shader(xenos::ShaderType::kPixel, 0, ucode.data(), ucode.size(),
                    std::endian::little);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  DxbcShaderTranslator dxbc(rex::ui::GraphicsProvider::GpuVendorID::kApple,
                            true, false, true, true, 1, 1, false);
  if (!dxbc.TranslateAnalyzedShader(*shader.GetOrCreateTranslation(0))) {
    std::fprintf(stderr, "DXBC translation failed\n%s\n",
                 shader.ucode_disassembly().c_str());
    std::exit(1);
  }
  metal::MslShaderTranslator msl{metal::MslTranslatorConfig{}};
  metal::MslTranslationResult result = msl.Translate(shader, 0);
  if (!result.ok) {
    std::fprintf(stderr, "MSL translation failed: %s\n%s\n",
                 result.reason.c_str(), shader.ucode_disassembly().c_str());
    std::exit(1);
  }
  return result.source;
}

bool Contains(const std::string& source, const std::string& text) {
  return source.find(text) != std::string::npos;
}

void Require(bool condition, const char* message, const std::string& source) {
  if (condition) {
    return;
  }
  std::fprintf(stderr, "%s\n%s\n", message, source.c_str());
  std::exit(1);
}

void CompileMetal(const std::filesystem::path& directory, const char* name,
                  const std::string& source) {
  std::filesystem::path input = directory / (std::string(name) + ".metal");
  std::filesystem::path output = directory / (std::string(name) + ".air");
  {
    std::ofstream stream(input);
    stream << source;
  }
  std::string command =
      "xcrun -sdk macosx metal -std=metal3.1 -c \"" + input.string() +
      "\" -o \"" + output.string() + "\"";
  int status = std::system(command.c_str());
  if (status != 0) {
    std::fprintf(stderr, "Metal compilation failed for %s\n%s\n", name,
                 source.c_str());
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 2;
  }
  std::filesystem::path directory = argv[1];
  std::filesystem::create_directories(directory);

  std::vector<uint32_t> jump_ucode;
  AppendCfPair(jump_ucode, PredicatedJump(2), Exec(2, false));
  AppendCfPair(jump_ucode, Exec(3, true), {0, 0});
  AppendCubeFetch(jump_ucode, false);
  AppendCubeFetch(jump_ucode, false);
  std::string jump_source = Translate(jump_ucode);
  Require(Contains(jump_source, "uint qj0 = p0 ? 1u : 0u;"),
          "predicated jump did not start from p0", jump_source);
  Require(Contains(jump_source,
                   "qj0 = qj0 & quad_shuffle_xor(qj0, 1)") &&
              Contains(jump_source,
                       "qj0 = qj0 & quad_shuffle_xor(qj0, 2)"),
          "predicated jump is not a non-short-circuiting quad reduction",
          jump_source);
  Require(Contains(jump_source, "if (!((qj0 != 0u)))"),
          "forward jump does not use the quad vote", jump_source);
  CompileMetal(directory, "quad_jump", jump_source);

  std::vector<uint32_t> cube_ucode;
  AppendCfPair(cube_ucode, Exec(1, true), {0, 0});
  AppendCubeFetch(cube_ucode, true, 16);
  std::string cube_source = Translate(cube_ucode);
  Require(Contains(cube_source, ", bias("),
          "computed-LOD cube fetch does not use implicit LOD plus bias",
          cube_source);
  Require(!Contains(cube_source, "gradientcube(") &&
              !Contains(cube_source, ", level("),
          "computed-LOD cube fetch still uses explicit LOD or gradients",
          cube_source);
  CompileMetal(directory, "cube_implicit_lod", cube_source);

  std::printf("PASS: quad_jump and cube_implicit_lod translated and compiled\n");
  return 0;
}

extern "C" void fable2_ring_writeback_stored() {}
