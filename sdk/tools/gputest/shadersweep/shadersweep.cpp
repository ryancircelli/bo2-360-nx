/**
 * rexgpu-shadersweep: translate every given Xenos microcode file (refxenos --dump-shaders output, host-endian,
 * named vs_<hash>.bin / ps_<hash>.bin) with the SDK SPIR-V translator. Prints one line per file and exits 1 if any
 * shader fails to translate or yields an empty binary. No GPU or Vulkan device needed.
 */
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>

using namespace rex;
using namespace rex::graphics;

int main(int argc, char** argv) {
  if (argc < 2) {
    fmt::print(stderr, "usage: rexgpu-shadersweep FILE...\n");
    return 2;
  }
  int bad = 0;
  for (int i = 1; i < argc; ++i) {
    std::string path = argv[i];
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty() || bytes.size() % 4) {
      fmt::print("FAIL {} unreadable or not dword sized\n", path);
      ++bad;
      continue;
    }
    std::vector<uint32_t> dwords(bytes.size() / 4);
    std::memcpy(dwords.data(), bytes.data(), bytes.size());
    std::string base = path.substr(path.find_last_of('/') + 1);
    bool is_vs = base.rfind("ps_", 0) != 0;
    SpirvShader shader(is_vs ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel, 0, dwords.data(),
                       dwords.size(), std::endian::little);
    string::StringBuffer disasm;
    shader.AnalyzeUcode(disasm);
    SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), false, false, false);
    uint32_t dyn_regs = shader.GetDynamicAddressableRegisterCount(63);
    SpirvShaderTranslator::Modification mod(is_vs ? translator.GetDefaultVertexShaderModification(dyn_regs)
                                                  : translator.GetDefaultPixelShaderModification(dyn_regs));
    if (is_vs) {
      mod.vertex.interpolator_mask = shader.writes_interpolators();
    } else {
      // Xenos copies interpolants into the first registers, bounded by the register count.
      uint32_t n = std::min<uint32_t>(std::max<uint32_t>(1, shader.register_static_address_bound()),
                                      xenos::kMaxInterpolators);
      mod.pixel.interpolator_mask = (1u << n) - 1;
      mod.pixel.interpolators_centroid = 0;
    }
    Shader::Translation* translation = shader.GetOrCreateTranslation(mod.value);
    bool ok = translator.TranslateAnalyzedShader(*translation) && translation->is_valid() &&
              !translation->translated_binary().empty();
    fmt::print("{} {} {} dwords, {} spirv bytes\n", ok ? "OK  " : "FAIL", base, dwords.size(),
               translation->translated_binary().size());
    for (const auto& e : translation->errors()) {
      fmt::print("  error: {}\n", e.message);
    }
    bad += !ok;
  }
  fmt::print("{} shaders, {} failed\n", argc - 1, bad);
  return bad ? 1 : 0;
}
